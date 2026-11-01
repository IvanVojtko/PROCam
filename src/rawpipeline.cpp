// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "rawpipeline.h"
#include "rawpipelinemath.h"
#include "raisr.h"
#include "dnglensshading.h"
#include "imageadjustments.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTransform>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace RawPipeline {
namespace {

double number(const QJsonObject &m, const char *key)
{
    const QJsonValue v = m.value(QLatin1String(key));
    return v.isString() ? v.toString().toDouble() : v.toDouble();
}

double rational(const QJsonValue &v)
{
    if (v.isDouble()) return v.toDouble();
    const QJsonArray r = v.toArray();
    if (r.size() != 2 || r[1].toDouble() == 0) throw std::runtime_error("Invalid RAW colour transform");
    return r[0].toDouble() / r[1].toDouble();
}

QJsonObject loadFrame(const QString &path, BayerFrame *frame)
{
    QFile metadata(path);
    if (!metadata.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read RAW metadata");
    const QJsonObject m = QJsonDocument::fromJson(metadata.readAll()).object();
    frame->width = m.value(QStringLiteral("width")).toInt() & ~1;
    frame->height = m.value(QStringLiteral("height")).toInt() & ~1;
    frame->white = m.value(QStringLiteral("white_level")).toDouble();
    const QStringList cfas{QStringLiteral("RGGB"), QStringLiteral("GRBG"), QStringLiteral("GBRG"), QStringLiteral("BGGR")};
    frame->cfa = cfas.indexOf(m.value(QStringLiteral("cfa")).toString());
    const QJsonArray black = m.value(QStringLiteral("black_level_pattern")).toArray();
    const int bits = m.value(QStringLiteral("bits_per_sample"))
            .toInt(m.value(QStringLiteral("format")).toString() == QLatin1String("RAW10") ? 10 : 16);
    const int width = m.value(QStringLiteral("width")).toInt();
    const int stride = m.value(QStringLiteral("row_stride")).toInt();
    if (frame->width < 32 || frame->height < 32 || frame->cfa < 0 || black.size() != 4
            || (bits != 10 && bits != 16) || !std::isfinite(frame->white) || frame->white <= 0
            || frame->white > 65535
            || qint64(stride) < (bits == 10 ? ((qint64(width) + 3) / 4) * 5 : qint64(width) * 2))
        throw std::runtime_error("RAW metadata is incomplete or unsupported");
    for (int k = 0; k < 4; ++k) {
        frame->black[k] = black[k].toDouble(-1);
        if (!std::isfinite(frame->black[k]) || frame->black[k] < 0 || frame->black[k] >= frame->white)
            throw std::runtime_error("Invalid RAW black levels");
    }
    // The per-frame black level, when the HAL reports a plausible one: in deep
    // shadows a one-count error is a visible colour cast after white balance.
    const QJsonArray dynamicBlack = m.value(QStringLiteral("dynamic_black_level")).toArray();
    if (dynamicBlack.size() == 4) {
        bool plausible = true;
        for (int k = 0; k < 4; ++k) {
            const double v = dynamicBlack[k].toDouble(-1);
            plausible = plausible && std::isfinite(v) && v >= 0 && std::abs(v - frame->black[k]) <= 16;
        }
        if (plausible)
            for (int k = 0; k < 4; ++k) frame->black[k] = dynamicBlack[k].toDouble();
    }
    QFile raw(m.value(QStringLiteral("raw_path")).toString());
    if (!raw.open(QIODevice::ReadOnly) || raw.size() < qint64(stride) * frame->height)
        throw std::runtime_error("Missing or truncated RAW frame");
    frame->pixels.resize(std::size_t(frame->width) * frame->height);
    QByteArray row(stride, Qt::Uninitialized);
    for (int y = 0; y < frame->height; ++y) {
        if (raw.read(row.data(), stride) != stride) throw std::runtime_error("Truncated RAW row");
        const auto *b = reinterpret_cast<const unsigned char *>(row.constData());
        uint16_t *out = &frame->pixels[std::size_t(y) * frame->width];
        if (bits == 10) {
            for (int x = 0; x < frame->width; ++x) {
                const int g = x / 4, o = x % 4;
                out[x] = uint16_t((uint16_t(b[g * 5 + o]) << 2) | ((b[g * 5 + 4] >> (o * 2)) & 3));
            }
        } else {
            for (int x = 0; x < frame->width; ++x) out[x] = uint16_t(b[x * 2] | (uint16_t(b[x * 2 + 1]) << 8));
        }
    }
    return m;
}

bool loadShading(const RenderSettings &settings, const QJsonObject &m, const BayerFrame &frame,
                 ShadingGrid *grid, QString *source)
{
    static const char *cfas[] = {"RGGB", "GRBG", "GBRG", "BGGR"};
    const QString cameraId = m.value(QStringLiteral("camera_id")).toString();
    if (settings.deviceModel.isEmpty() || cameraId.isEmpty()) return false;
    for (const QString &dir : settings.calibrationDirs) {
        DngLensShading::GainGrid gains;
        QString warning;
        if (!DngLensShading::loadGainGrid(dir, settings.deviceModel, cameraId,
                                          QLatin1String(cfas[frame.cfa]),
                                          m.value(QStringLiteral("width")).toInt(),
                                          m.value(QStringLiteral("height")).toInt(), &gains, &warning)) {
            if (!warning.isEmpty()) qWarning() << "rawpipeline lens shading:" << warning;
            continue;
        }
        grid->rows = gains.rows;
        grid->cols = gains.cols;
        for (int p = 0; p < 4; ++p) grid->phase[p].assign(gains.phase[p].cbegin(), gains.phase[p].cend());
        *source = dir;
        return grid->valid();
    }
    return false;
}

} // namespace

namespace {

// Upscaling filters from the first calibration directory that has them,
// loaded once.
const Raisr::Filters *upscaleFilters(const QStringList &dirs)
{
    static std::mutex mutex;
    static QString loadedFrom;
    static Raisr::Filters filters;
    std::lock_guard<std::mutex> lock(mutex);
    for (const QString &dir : dirs) {
        const QString path = QDir(dir).filePath(QStringLiteral("raisr-2x.bin"));
        if (!QFileInfo::exists(path)) continue;
        if (path != loadedFrom) {
            std::string error;
            Raisr::Filters loaded;
            if (!Raisr::load(QFile::encodeName(path).toStdString(), &loaded, &error)) {
                qWarning() << "rawpipeline:" << QString::fromStdString(error);
                continue;
            }
            filters = std::move(loaded);
            loadedFrom = path;
        }
        return &filters;
    }
    return nullptr;
}

// Centred crop of 1/zoom, even-aligned for the CFA, rendered at up to 2x.
ZoomRegion zoomRegion(double zoom, int width, int height)
{
    ZoomRegion region;
    if (!(zoom > 1.05) || !std::isfinite(zoom)) return region;
    region.width = std::max(16, int(std::lround(width / zoom / 2)) * 2);
    region.height = std::max(16, int(std::lround(height / zoom / 2)) * 2);
    region.x = ((width - region.width) / 2) & ~1;
    region.y = ((height - region.height) / 2) & ~1;
    region.scale = std::min(2.0, zoom);
    return region;
}

}

namespace {

// "faces": [[left, top, right, bottom], ...] normalised on the sensor frame,
// as the viewfinder detected them before the shot.
FaceRects facesFrom(const QJsonObject &m)
{
    FaceRects faces;
    for (const QJsonValue &value : m.value(QStringLiteral("faces")).toArray()) {
        const QJsonArray box = value.toArray();
        if (box.size() < 4) continue;
        std::array<float, 4> face;
        for (int k = 0; k < 4; ++k) face[k] = float(box[k].toDouble());
        if (face[2] > face[0] && face[3] > face[1]) faces.push_back(face);
    }
    return faces;
}

QJsonObject readMetadata(const QString &path)
{
    QFile metadata(path);
    if (!metadata.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read frame metadata");
    return QJsonDocument::fromJson(metadata.readAll()).object();
}

// JPEG burst frames (the bridge's fallback when the HAL refuses a second
// YUV stream), decoded and converted back to I420 (full-range BT.601).
void loadJpegFrame(const QJsonObject &m, YuvFrame *frame)
{
    QImage image(m.value(QStringLiteral("raw_path")).toString(), "JPG");
    if (image.isNull()) throw std::runtime_error("Missing or unreadable JPEG frame");
    image = image.convertToFormat(QImage::Format_RGB888);
    const int width = image.width() & ~1, height = image.height() & ~1;
    if (width < 32 || height < 32) throw std::runtime_error("JPEG frame too small");
    frame->width = width;
    frame->height = height;
    const std::size_t lumaSize = std::size_t(width) * height, chromaSize = lumaSize / 4;
    frame->data.resize(lumaSize + 2 * chromaSize);
    uint8_t *y = frame->data.data(), *u = y + lumaSize, *v = u + chromaSize;
    auto clamp = [](double value) { return uint8_t(std::max(0.0, std::min(255.0, std::round(value)))); };
    for (int row = 0; row < height; row += 2) {
        const uchar *lines[2] = {image.constScanLine(row), image.constScanLine(row + 1)};
        for (int x = 0; x < width; x += 2) {
            double cb = 0, cr = 0;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const uchar *p = lines[j] + (x + i) * 3;
                    const double r = p[0], g = p[1], b = p[2];
                    y[std::size_t(row + j) * width + x + i] = clamp(0.299 * r + 0.587 * g + 0.114 * b);
                    cb += -0.168736 * r - 0.331264 * g + 0.5 * b;
                    cr += 0.5 * r - 0.418688 * g - 0.081312 * b;
                }
            u[std::size_t(row / 2) * (width / 2) + x / 2] = clamp(128 + cb / 4);
            v[std::size_t(row / 2) * (width / 2) + x / 2] = clamp(128 + cr / 4);
        }
    }
}

// I420 frames written by the bridge for cameras without RAW output.
void loadYuvFrame(const QJsonObject &m, YuvFrame *frame)
{
    if (m.value(QStringLiteral("format")).toString() == QLatin1String("JPEG")) {
        loadJpegFrame(m, frame);
        return;
    }
    frame->width = m.value(QStringLiteral("width")).toInt();
    frame->height = m.value(QStringLiteral("height")).toInt();
    if (frame->width < 32 || frame->height < 32 || frame->width > 16384 || frame->height > 16384)
        throw std::runtime_error("YUV metadata is incomplete or unsupported");
    const qint64 size = qint64(frame->width) * frame->height
            + 2 * qint64((frame->width + 1) / 2) * ((frame->height + 1) / 2);
    QFile file(m.value(QStringLiteral("raw_path")).toString());
    if (!file.open(QIODevice::ReadOnly) || file.size() < size)
        throw std::runtime_error("Missing or truncated YUV frame");
    frame->data.resize(std::size_t(size));
    if (file.read(reinterpret_cast<char *>(frame->data.data()), size) != size)
        throw std::runtime_error("Truncated YUV frame");
}

bool renderYuvImage(const QStringList &metadataPaths, const RenderSettings &settings, QImage *image,
                    QJsonObject *info)
{
    QElapsedTimer timer;
    timer.start();
    const int count = metadataPaths.size();
    std::vector<YuvFrame> frames(count);
    std::vector<QJsonObject> metadata;
    for (int i = 0; i < count; ++i) {
        metadata.push_back(readMetadata(metadataPaths[i]));
        loadYuvFrame(metadata.back(), &frames[i]);
        if (frames[i].width != frames[0].width || frames[i].height != frames[0].height
                || metadata.back().value(QStringLiteral("camera_id")) != metadata.front().value(QStringLiteral("camera_id")))
            throw std::runtime_error("Burst frames differ in camera or geometry");
    }
    const qint64 loadMs = timer.restart();

    // Reference: the sharpest frame (least shake), as for RAW.
    int reference = 0;
    if (count > 1) {
        double best = -1;
        for (int i = 0; i < count; ++i) {
            const YuvFrame &f = frames[i];
            const int hw = f.width / 2, hh = f.height / 2;
            std::vector<float> gray(std::size_t(hw) * hh);
            for (int y = 0; y < hh; ++y)
                for (int x = 0; x < hw; ++x) {
                    const uint8_t *row = &f.data[std::size_t(2 * y) * f.width + 2 * x];
                    gray[std::size_t(y) * hw + x] = (row[0] + row[1] + row[f.width] + row[f.width + 1]) / 1020.0f;
                }
            const double sharp = sharpness(gray, hw, hh);
            if (sharp > best) {
                best = sharp;
                reference = i;
            }
        }
    }
    std::vector<const YuvFrame *> pointers{&frames[reference]};
    for (int i = 0; i < count; ++i)
        if (i != reference) pointers.push_back(&frames[i]);
    const QJsonObject &m = metadata[reference];

    YuvOptions options;
    options.spatialDenoise = std::max(0.0, settings.spatialDenoise);
    const double exposure = settings.exposure > 0 && std::isfinite(settings.exposure) ? settings.exposure : 1.0;
    options.brightness = std::max(0.25, settings.targetGain) * exposure;
    options.night = m.value(QStringLiteral("night_mode")).toBool();
    options.faces = facesFrom(m);
    if (settings.toneRefinements) {
        options.blackPoint = true;
        options.whitePoint = true;
        options.clarity = 0.4;
        options.saturation = 1.05;
        if (m.value(QStringLiteral("photo_aspect")).toString() == QLatin1String("16:9")) {
            const int longSide = std::max(frames[0].width, frames[0].height);
            const int shortSide = std::min(frames[0].width, frames[0].height);
            options.keptShortSide = std::min(1.0, longSide * 9.0 / 16.0 / shortSide);
        }
    }
    const double zoomRatio = settings.zoom > 0 ? settings.zoom : number(m, "zoom_ratio");
    options.zoom = zoomRegion(zoomRatio, frames[0].width & ~1, frames[0].height & ~1);
    if (options.zoom.active()) options.upscaleFilters = upscaleFilters(settings.calibrationDirs);
    YuvStats stats;
    const RgbImage rgb = renderYuvBurst(pointers, options, &stats);
    std::vector<YuvFrame>().swap(frames);
    if (rgb.width <= 0) throw std::runtime_error("YUV burst could not be rendered");
    QImage result(rgb.width, rgb.height, QImage::Format_RGB888);
    if (result.isNull()) throw std::runtime_error("Not enough memory for the rendered image");
    for (int y = 0; y < rgb.height; ++y)
        memcpy(result.scanLine(y), &rgb.rgb[std::size_t(y) * rgb.width * 3], std::size_t(rgb.width) * 3);
    *image = result;
    const qint64 renderMs = timer.elapsed();

    if (info) {
        QJsonObject p;
        p.insert(QStringLiteral("input"), m.value(QStringLiteral("format")).toString() == QLatin1String("JPEG")
                 ? QStringLiteral("jpeg") : QStringLiteral("yuv"));
        p.insert(QStringLiteral("frames"), count);
        p.insert(QStringLiteral("reference_frame"), reference);
        p.insert(QStringLiteral("merge_rejection"), stats.rejection);
        p.insert(QStringLiteral("luma_noise"), stats.lumaNoise);
        p.insert(QStringLiteral("chroma_noise"), stats.chromaNoise);
        p.insert(QStringLiteral("denoise_severity"), stats.severity);
        if (stats.faceLevel >= 0) p.insert(QStringLiteral("face_level"), stats.faceLevel);
        if (options.blackPoint || options.whitePoint) {
            p.insert(QStringLiteral("black_point"), stats.blackPoint);
            p.insert(QStringLiteral("white_scale"), stats.whiteScale);
        }
        p.insert(QStringLiteral("brightness"), options.brightness);
        if (options.zoom.active()) {
            QJsonObject z;
            z.insert(QStringLiteral("ratio"), zoomRatio);
            z.insert(QStringLiteral("crop"), QJsonArray{options.zoom.x, options.zoom.y, options.zoom.width, options.zoom.height});
            z.insert(QStringLiteral("scale"), options.zoom.scale);
            z.insert(QStringLiteral("learned_upscale"), options.upscaleFilters != nullptr);
            p.insert(QStringLiteral("zoom"), z);
        }
        QJsonObject timing;
        timing.insert(QStringLiteral("load"), double(loadMs));
        timing.insert(QStringLiteral("align"), std::round(stats.stageMs[0]));
        timing.insert(QStringLiteral("merge"), std::round(stats.stageMs[1]));
        timing.insert(QStringLiteral("finish"), std::round(stats.stageMs[2]));
        timing.insert(QStringLiteral("total"), double(loadMs + renderMs));
        p.insert(QStringLiteral("timing_ms"), timing);
        p.insert(QStringLiteral("threads"), int(std::thread::hardware_concurrency()));
        *info = p;
    }
    qDebug() << "rawpipeline yuv frames" << count << "reference" << reference << "rejection" << stats.rejection
             << "ms load" << loadMs << "render" << renderMs;
    return true;
}

}

namespace {

bool renderFullFrame(const QStringList &metadataPaths, const RenderSettings &settings, QImage *image,
                     QJsonObject *info, QString *error)
{
    try {
        if (metadataPaths.isEmpty()) throw std::runtime_error("No RAW frames to render");
        const QString format = readMetadata(metadataPaths.first()).value(QStringLiteral("format")).toString();
        if (format == QLatin1String("I420") || format == QLatin1String("JPEG"))
            return renderYuvImage(metadataPaths, settings, image, info);
        QElapsedTimer timer;
        timer.start();
        const int count = metadataPaths.size();
        std::vector<std::unique_ptr<BayerFrame>> frames;
        std::vector<QJsonObject> metadata;
        for (const QString &path : metadataPaths) {
            frames.emplace_back(new BayerFrame);
            metadata.push_back(loadFrame(path, frames.back().get()));
            const BayerFrame &f = *frames.back(), &first = *frames.front();
            if (f.width != first.width || f.height != first.height || f.cfa != first.cfa
                    || metadata.back().value(QStringLiteral("camera_id")) != metadata.front().value(QStringLiteral("camera_id")))
                throw std::runtime_error("Burst frames differ in camera or geometry");
        }
        const qint64 loadMs = timer.restart();

        // Exposure of each frame (time x gain); bursts are nominally equal.
        std::vector<double> exposures(count, 1.0);
        for (int i = 0; i < count; ++i) {
            const double t = number(metadata[i], "exposure_time_ns"), iso = number(metadata[i], "iso");
            exposures[i] = t > 0 && iso > 0 ? t * iso : 1.0;
        }

        // Reference: the sharpest frame (least shake). A burst spans half a
        // second at most, and on a real indoor burst at 1/15 s the sharpest
        // frame was the sixth, 10% sharper than the best of the first three.
        std::vector<std::vector<float>> grays(count);
        int reference = 0;
        if (count > 1) {
            double best = -1;
            for (int i = 0; i < count; ++i) {
                std::vector<float> gray = greenImage(*frames[i], exposures[0] / exposures[i]);
                const double s = sharpness(gray, frames[i]->width / 2, frames[i]->height / 2);
                if (s > best) {
                    best = s;
                    // Only the best is kept (the others are rebuilt for
                    // alignment), not 100 MB of green images at once.
                    std::vector<float>().swap(grays[reference]);
                    reference = i;
                    grays[i] = std::move(gray);
                }
            }
        }
        std::swap(frames[0], frames[reference]);
        std::swap(metadata[0], metadata[reference]);
        std::swap(exposures[0], exposures[reference]);
        std::swap(grays[0], grays[reference]);
        std::vector<double> gains(count);
        for (int i = 0; i < count; ++i) gains[i] = exposures[0] / exposures[i];
        const int planeWidth = frames[0]->width / 2, planeHeight = frames[0]->height / 2;
        // Green alone translates cleanly at sub-pixel shifts (super-resolution).
        if (count > 1 && grays[0].empty()) grays[0] = greenImage(*frames[0], 1.0);

        std::vector<Alignment> alignments;
        for (int i = 1; i < count; ++i) {
            if (grays[i].empty()) grays[i] = greenImage(*frames[i], gains[i]);
            alignments.push_back(align(grays[0], grays[i], planeWidth, planeHeight));
            std::vector<float>().swap(grays[i]);
        }
        std::vector<float>().swap(grays[0]);
        const qint64 alignMs = timer.restart();

        // Noise: trust the HAL profile only if it agrees with the image.
        const QJsonObject &m = metadata[0];
        NoiseModel measured;
        {
            const Planes planes = toPlanes(*frames[0], 1.0);
            measured = estimateNoise(planes);
        }
        std::vector<double> profileValues;
        for (const QJsonValue &v : m.value(QStringLiteral("noise_profile")).toArray()) profileValues.push_back(v.toDouble());
        NoiseModel profile;
        QString noiseSource = QStringLiteral("measured");
        NoiseModel noise = measured;
        if (noiseFromProfile(profileValues, &profile)) {
            const double ratio = profile.meanVariance(0.1) / measured.meanVariance(0.1);
            if (ratio > 1.0 / 3 && ratio < 3) {
                noise = profile;
                noiseSource = QStringLiteral("profile");
            } else {
                noiseSource = QStringLiteral("measured_profile_rejected");
            }
        }

        std::vector<const BayerFrame *> pointers;
        for (const auto &f : frames) pointers.push_back(f.get());
        MergeStats mergeStats;
        // Spatial denoise follows the expected noise at metered mid grey
        // after averaging the frames: light for clean shots (keeps detail),
        // strong two-step filtering for high-ISO single frames.
        const double exposureScale = settings.exposure > 0 && std::isfinite(settings.exposure) ? settings.exposure : 1.0;
        // Judged in the shadows the tone mapping lifts (up to 3x), where grain shows first.
        const double expectedShadow = 0.06 / (std::max(0.05, settings.targetGain) * exposureScale);
        const double expectedNoise = std::sqrt(noise.meanVariance(expectedShadow) / count) / expectedShadow;
        const double severity = std::max(0.0, std::min(1.0, (expectedNoise - 0.015) / 0.085));
        MergeOptions mergeOptions;
        mergeOptions.spatialStrength = (1.0 + 1.5 * severity) * std::max(0.0, settings.spatialDenoise);
        mergeOptions.spatialFloor = settings.spatialFloor >= 0 ? settings.spatialFloor : 0.5 * severity;
        mergeOptions.spatialPilot = settings.spatialPilot >= 0 ? settings.spatialPilot : 2.5 * severity;
        Planes merged = merge(pointers, gains, alignments, noise, mergeOptions, &mergeStats);
        const qint64 mergeOnlyMs = timer.restart();
        // Super-resolved fine detail, while the mosaics are still loaded; it
        // carries some noise the merge removed, so it fades with severity.
        std::vector<float> detail, detailCoverage;
        const double zoomRatio = settings.zoom > 0 ? settings.zoom : number(m, "zoom_ratio");
        const ZoomRegion zoom = zoomRegion(zoomRatio, frames[0]->width, frames[0]->height);
        const double detailStrength = settings.superResolution && count >= 3 ? 1.0 - severity : 0.0;
        if (detailStrength > 0.05)
            detail = superResolveGreen(pointers, gains, alignments, merged, noise, zoom.active() ? &zoom : nullptr,
                                       &detailCoverage);
        const qint64 superResolutionMs = timer.restart();
        pointers.clear();
        const int cfa = frames[0]->cfa;
        // Keep the reference geometry for shading, then free the mosaics.
        BayerFrame shape;
        shape.width = frames[0]->width;
        shape.height = frames[0]->height;
        shape.cfa = cfa;
        frames.clear();
        const qint64 mergeMs = mergeOnlyMs;

        FinishOptions options;
        options.cfa = cfa;
        const QJsonArray wb = m.value(QStringLiteral("color_correction_gains")).toArray();
        if (wb.size() == 4)
            for (int i = 0; i < 4; ++i) options.wbGains[i] = wb[i].toDouble(1.0);
        const QJsonArray matrix = m.value(QStringLiteral("capture_color_transform")).toArray();
        if (matrix.size() == 9)
            for (int i = 0; i < 9; ++i) options.colorMatrix[i] = rational(matrix[i]);
        for (double v : options.colorMatrix)
            if (!std::isfinite(v)) throw std::runtime_error("Invalid colour matrix");
        ShadingGrid shading;
        QString shadingSource;
        if (loadShading(settings, m, shape, &shading, &shadingSource)) options.shading = &shading;
        if (zoom.active()) {
            options.zoom = zoom;
            options.upscaleFilters = upscaleFilters(settings.calibrationDirs);
        }
        if (!detail.empty()) {
            options.detailGreen = &detail;
            options.detailStrength = detailStrength;
            options.detailCoverage = &detailCoverage;
        }
        const double exposure = settings.exposure > 0 && std::isfinite(settings.exposure) ? settings.exposure : 1.0;
        options.targetGain = std::max(0.05, settings.targetGain) * exposure;
        options.night = m.value(QStringLiteral("night_mode")).toBool();
        options.faces = facesFrom(m);
        if (settings.toneRefinements) {
            options.blackPoint = true;
            options.whitePoint = true;
            options.clarity = 0.4;
            options.saturation *= 1.05;
            if (m.value(QStringLiteral("photo_aspect")).toString() == QLatin1String("16:9")) {
                const int longSide = std::max(shape.width, shape.height);
                const int shortSide = std::min(shape.width, shape.height);
                options.keptShortSide = std::min(1.0, longSide * 9.0 / 16.0 / shortSide);
            }
        }
        // Noise relative to signal at metered mid grey, after merging.
        const double effectiveFrames = 1.0 + (count - 1) * (1.0 - mergeStats.rejection);
        const double midGrey = 0.18 / options.targetGain;
        options.relativeNoise = std::sqrt(noise.meanVariance(midGrey) / effectiveFrames) / midGrey;
        options.blackNoise = std::sqrt(noise.meanVariance(0.0) / effectiveFrames);
        FinishStats finishStats;
        const RgbImage rgb = finish(merged, options, &finishStats);
        Planes().phase.swap(merged.phase);
        QImage result(rgb.width, rgb.height, QImage::Format_RGB888);
        if (result.isNull()) throw std::runtime_error("Not enough memory for the rendered image");
        for (int y = 0; y < rgb.height; ++y)
            memcpy(result.scanLine(y), &rgb.rgb[std::size_t(y) * rgb.width * 3], std::size_t(rgb.width) * 3);
        const qint64 finishMs = timer.elapsed();
        *image = result;

        if (info) {
            QJsonObject p;
            p.insert(QStringLiteral("frames"), count);
            p.insert(QStringLiteral("reference_frame"), reference);
            p.insert(QStringLiteral("noise_model_source"), noiseSource);
            QJsonArray scale, offset;
            for (int i = 0; i < 4; ++i) {
                scale.append(noise.scale[i]);
                offset.append(noise.offset[i]);
            }
            p.insert(QStringLiteral("noise_scale"), scale);
            p.insert(QStringLiteral("noise_offset"), offset);
            p.insert(QStringLiteral("merge_rejection"), mergeStats.rejection);
            p.insert(QStringLiteral("relative_noise"), options.relativeNoise);
            p.insert(QStringLiteral("denoise_severity"), severity);
            p.insert(QStringLiteral("super_resolution"), detail.empty() ? 0.0 : detailStrength);
            if (zoom.active()) {
                QJsonObject z;
                z.insert(QStringLiteral("ratio"), zoomRatio);
                z.insert(QStringLiteral("crop"), QJsonArray{zoom.x, zoom.y, zoom.width, zoom.height});
                z.insert(QStringLiteral("scale"), zoom.scale);
                z.insert(QStringLiteral("learned_upscale"), options.upscaleFilters != nullptr);
                p.insert(QStringLiteral("zoom"), z);
            }
            p.insert(QStringLiteral("target_gain"), options.targetGain);
            p.insert(QStringLiteral("short_gain"), finishStats.shortGain);
            if (finishStats.faceLevel >= 0) p.insert(QStringLiteral("face_level"), finishStats.faceLevel);
            if (finishStats.faceBright >= 0) p.insert(QStringLiteral("face_bright"), finishStats.faceBright);
            if (finishStats.faceOutput >= 0) p.insert(QStringLiteral("face_output"), finishStats.faceOutput);
            if (options.blackPoint || options.whitePoint) {
                p.insert(QStringLiteral("tone_levels"), QJsonArray{finishStats.lowLevel, finishStats.highLevel});
                p.insert(QStringLiteral("black_point"), finishStats.blackPoint);
                p.insert(QStringLiteral("white_scale"), finishStats.whiteScale);
            }
            p.insert(QStringLiteral("long_gain"), finishStats.longGain);
            p.insert(QStringLiteral("lens_shading"), shadingSource.isEmpty() ? QJsonValue(false) : QJsonValue(shadingSource));
            QJsonObject timing;
            timing.insert(QStringLiteral("load"), double(loadMs));
            timing.insert(QStringLiteral("align"), double(alignMs));
            timing.insert(QStringLiteral("merge"), double(mergeMs));
            timing.insert(QStringLiteral("super_resolution"), double(superResolutionMs));
            timing.insert(QStringLiteral("finish"), double(finishMs));
            QJsonArray stages;
            for (double ms : finishStats.stageMs) stages.append(std::round(ms));
            timing.insert(QStringLiteral("finish_stages"), stages);
            p.insert(QStringLiteral("timing_ms"), timing);
            p.insert(QStringLiteral("threads"), int(std::thread::hardware_concurrency()));
            *info = p;
        }
        qDebug() << "rawpipeline frames" << count << "reference" << reference << "noise" << noiseSource
                 << "rejection" << mergeStats.rejection << "gains" << finishStats.shortGain << finishStats.longGain
                 << "ms load" << loadMs << "align" << alignMs << "merge" << mergeMs << "finish" << finishMs;
        return true;
    } catch (const std::bad_alloc &) {
        *error = QStringLiteral("Not enough memory for RAW processing");
        return false;
    } catch (const std::exception &e) {
        *error = QString::fromUtf8(e.what());
        return false;
    }
}

// Centre crop of the short side for a wider photo aspect ("16:9"; "4:3" is
// the sensor's own), before rotation, so portrait photos become 9:16.
void cropToAspect(QImage *image, const QString &aspect)
{
    if (aspect != QLatin1String("16:9") || image->isNull()) return;
    const bool landscape = image->width() >= image->height();
    const int longSide = landscape ? image->width() : image->height();
    const int shortSide = landscape ? image->height() : image->width();
    const int wanted = (longSide * 9 / 16) & ~1;
    if (wanted <= 0 || wanted >= shortSide) return;
    const int offset = ((shortSide - wanted) / 2) & ~1;
    *image = landscape ? image->copy(0, offset, image->width(), wanted)
                       : image->copy(offset, 0, wanted, image->height());
}

} // namespace

bool renderImage(const QStringList &metadataPaths, const RenderSettings &settings, QImage *image,
                 QJsonObject *info, QString *error)
{
    if (!renderFullFrame(metadataPaths, settings, image, info, error)) return false;
    const QString aspect = readMetadata(metadataPaths.first()).value(QStringLiteral("photo_aspect")).toString();
    cropToAspect(image, aspect);
    if (info) {
        if (!aspect.isEmpty()) info->insert(QStringLiteral("photo_aspect"), aspect);
        if (readMetadata(metadataPaths.first()).value(QStringLiteral("night_mode")).toBool())
            info->insert(QStringLiteral("night_mode"), true);
    }
    return true;
}

bool renderBurst(const QString &target, const QStringList &metadataPaths, int quality,
                 const RenderSettings &settings, QString *error)
{
    QFile first(metadataPaths.value(0));
    if (!first.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read burst metadata");
        return false;
    }
    QJsonObject m = QJsonDocument::fromJson(first.readAll()).object();
    QImage image;
    QJsonObject info;
    if (!renderImage(metadataPaths, settings, &image, &info, error)) return false;
    const int rotation = m.value(QStringLiteral("bracket_rotation")).toInt();
    if (rotation) image = image.transformed(QTransform().rotate(rotation));
    ImageAdjustments::apply(&image, 1.0, m.value(QStringLiteral("color_temperature_requested_kelvin")).toInt(),
                            m.value(QStringLiteral("color_tint_requested")).toInt());

    QJsonArray frames;
    for (const QString &path : metadataPaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        QJsonObject frame = QJsonDocument::fromJson(file.readAll()).object();
        frame.remove(QStringLiteral("raw_path"));
        frame.remove(QStringLiteral("_original_exif"));
        frames.append(frame);
    }
    m.remove(QStringLiteral("raw_path"));
    m.remove(QStringLiteral("_original_exif"));
    m.insert(QStringLiteral("burst_merged"), true);
    m.insert(QStringLiteral("bracket_count"), metadataPaths.size());
    m.insert(QStringLiteral("bracket_frames"), frames);
    m.insert(QStringLiteral("merge_algorithm"), QStringLiteral("tile_align_wiener_merge_exposure_fusion"));
    m.insert(QStringLiteral("raw_pipeline"), info);
    m.insert(QStringLiteral("width"), image.width());
    m.insert(QStringLiteral("height"), image.height());
    m.insert(QStringLiteral("pixels_rotated"), true);
    m.insert(QStringLiteral("render_engine"), QStringLiteral("rawpipeline_burst"));

    QDir().mkpath(QFileInfo(target).absolutePath());
    QSaveFile output(target);
    QImageWriter writer(&output, "JPG");
    writer.setQuality(quality);
    writer.setProgressiveScanWrite(m.value(QStringLiteral("progressive_jpeg")).toBool());
    if (!output.open(QIODevice::WriteOnly) || !writer.write(image) || !output.commit()) {
        *error = QStringLiteral("Cannot save merged burst JPEG");
        return false;
    }
    const QFileInfo targetInfo(target);
    QSaveFile sidecar(targetInfo.absolutePath() + QLatin1Char('/') + targetInfo.completeBaseName()
                      + QStringLiteral(".json"));
    const QByteArray json = QJsonDocument(m).toJson();
    if (!sidecar.open(QIODevice::WriteOnly) || sidecar.write(json) != json.size() || !sidecar.commit()) {
        QFile::remove(target);
        *error = QStringLiteral("Cannot save merged burst metadata");
        return false;
    }
    return true;
}

}
