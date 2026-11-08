// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// Exports training data for the burst denoiser from saved burst frames
// (Pictures/ProCam/*_burstN.json + .raw16, "keep burst frames" on). For
// every burst it aligns all frames to the sharpest one, as the renderer does,
// and writes temporal merges of the first k frames (k = 1, 2, 4, ... below
// the frame count) and of all frames, with the spatial denoise off, as
// linear CFA planes (black 0, white 1) in float16 .npy files of shape
// (4, height/2, width/2). For comparison, every level below the full merge is
// also written with the renderer's spatial denoise (k<N>_spatial.npy, same
// strength as RawPipeline::renderImage would use for that many frames).
// meta.json has the noise model and merge stats.
//
//   burstpairs [--max-frames N] <burst folder> <output folder> [stem...]
#include "../../src/rawframecheck.h"
#include "../../src/rawpipeline.h"
#include "../../src/rawpipelinemath.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>

using namespace RawPipeline;

namespace {

QTextStream out(stdout);

double number(const QJsonObject &m, const char *key)
{
    const QJsonValue v = m.value(QLatin1String(key));
    return v.isString() ? v.toString().toDouble() : v.toDouble();
}

// The sidecars name the frame's path on the phone: read it from next to the
// sidecar instead.
QJsonObject loadLocalFrame(const QString &sidecar, BayerFrame *frame)
{
    QFile file(sidecar);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read sidecar");
    QJsonObject m = QJsonDocument::fromJson(file.readAll()).object();
    const QFileInfo info(sidecar);
    m.insert(QStringLiteral("raw_path"), info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
             + QStringLiteral(".raw16"));
    QTemporaryFile local;
    if (!local.open() || local.write(QJsonDocument(m).toJson(QJsonDocument::Compact)) <= 0)
        throw std::runtime_error("Cannot write temporary sidecar");
    local.close();
    return loadRawFrame(local.fileName(), frame);
}

bool writeNpy(const QString &path, const Planes &planes)
{
    const QString shape = QStringLiteral("(4, %1, %2)").arg(planes.height).arg(planes.width);
    QByteArray header = QStringLiteral("{'descr': '<f2', 'fortran_order': False, 'shape': %1, }")
            .arg(shape).toLatin1();
    // Magic (6) + version (2) + length (2) + header, padded to 64 with '\n'.
    while ((10 + header.size() + 1) % 64) header.append(' ');
    header.append('\n');
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray prefix("\x93NUMPY\x01\x00", 8);
    prefix.append(char(header.size() & 0xff)).append(char(header.size() >> 8));
    file.write(prefix);
    file.write(header);
    std::vector<_Float16> row(std::size_t(planes.width));
    for (int k = 0; k < 4; ++k) {
        for (int y = 0; y < planes.height; ++y) {
            const float *src = &planes.phase[k][std::size_t(y) * planes.width];
            for (int x = 0; x < planes.width; ++x) row[x] = _Float16(src[x] / planes.clip);
            file.write(reinterpret_cast<const char *>(row.data()), qint64(row.size() * sizeof(_Float16)));
        }
    }
    return file.commit();
}

QJsonArray toArray(const std::array<double, 4> &values)
{
    QJsonArray a;
    for (double v : values) a.append(v);
    return a;
}

bool exportBurst(const QString &stem, const QStringList &sidecars, const QString &target, int maxFrames)
{
    QElapsedTimer timer;
    timer.start();
    std::vector<std::unique_ptr<BayerFrame>> frames;
    std::vector<QJsonObject> metadata;
    for (const QString &path : sidecars) {
        std::unique_ptr<BayerFrame> frame(new BayerFrame);
        QJsonObject m;
        try {
            m = loadLocalFrame(path, frame.get());
        } catch (const std::exception &e) {
            out << stem << ": skipping " << QFileInfo(path).fileName() << ": " << e.what() << Qt::endl;
            continue;
        }
        if (!rawFrameDamage(frame->pixels.data(), frame->width, frame->height, frame->black.data()).isEmpty()) {
            out << stem << ": skipping damaged " << QFileInfo(path).fileName() << Qt::endl;
            continue;
        }
        if (!frames.empty() && (frame->width != frames[0]->width || frame->height != frames[0]->height
                                || frame->cfa != frames[0]->cfa)) {
            out << stem << ": skipping " << QFileInfo(path).fileName() << ": geometry differs" << Qt::endl;
            continue;
        }
        frames.push_back(std::move(frame));
        metadata.push_back(m);
    }

    // As the renderer: frames more than 25% off the requested exposure are
    // late preview frames.
    std::vector<double> exposures;
    for (const QJsonObject &m : metadata) {
        const double t = number(m, "exposure_time_ns"), iso = number(m, "iso");
        exposures.push_back(t > 0 && iso > 0 ? t * iso : 1.0);
    }
    if (!frames.empty()) {
        double requested = number(metadata[0], "exposure_time_requested_ns")
                * number(metadata[0], "sensor_sensitivity_requested");
        if (!(requested > 0)) {
            std::vector<double> sorted = exposures;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            requested = sorted[sorted.size() / 2];
        }
        std::size_t kept = 0;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const double ratio = exposures[i] / requested;
            if (ratio < 0.8 || ratio > 1.25) continue;
            frames[kept].swap(frames[i]);
            std::swap(metadata[kept], metadata[i]);
            std::swap(exposures[kept], exposures[i]);
            ++kept;
        }
        frames.resize(kept);
        metadata.resize(kept);
        exposures.resize(kept);
    }
    if (maxFrames > 0 && int(frames.size()) > maxFrames) {
        frames.resize(maxFrames);
        metadata.resize(maxFrames);
        exposures.resize(maxFrames);
    }
    const int count = int(frames.size());
    if (count < 2) {
        out << stem << ": fewer than 2 usable frames, skipped" << Qt::endl;
        return false;
    }

    // Reference: the sharpest frame, moved to the front.
    int reference = 0;
    double best = -1;
    for (int i = 0; i < count; ++i) {
        const std::vector<float> gray = greenImage(*frames[i], exposures[0] / exposures[i]);
        const double s = sharpness(gray, frames[i]->width / 2, frames[i]->height / 2);
        if (s > best) {
            best = s;
            reference = i;
        }
    }
    std::swap(frames[0], frames[reference]);
    std::swap(metadata[0], metadata[reference]);
    std::swap(exposures[0], exposures[reference]);
    std::vector<double> gains(count);
    for (int i = 0; i < count; ++i) gains[i] = exposures[0] / exposures[i];
    const int planeWidth = frames[0]->width / 2, planeHeight = frames[0]->height / 2;
    const std::vector<float> referenceGray = greenImage(*frames[0], 1.0);
    std::vector<Alignment> alignments;
    for (int i = 1; i < count; ++i)
        alignments.push_back(align(referenceGray, greenImage(*frames[i], gains[i]), planeWidth, planeHeight));

    // Noise: the HAL profile if it agrees with the image, as the renderer.
    const NoiseModel measured = estimateNoise(toPlanes(*frames[0], 1.0));
    NoiseModel noise = measured;
    QString noiseSource = QStringLiteral("measured");
    std::vector<double> profileValues;
    for (const QJsonValue &v : metadata[0].value(QStringLiteral("noise_profile")).toArray())
        profileValues.push_back(v.toDouble());
    NoiseModel profile;
    if (noiseFromProfile(profileValues, &profile)) {
        const double ratio = profile.meanVariance(0.1) / measured.meanVariance(0.1);
        if (ratio > 1.0 / 3 && ratio < 3) {
            noise = profile;
            noiseSource = QStringLiteral("profile");
        }
    }

    // The renderer's settings for this capture (capture sidecar).
    double targetGain = 1.0, exposure = 1.0;
    {
        QFile capture(QFileInfo(sidecars.value(0)).absolutePath() + QLatin1Char('/') + stem + QStringLiteral(".json"));
        if (capture.open(QIODevice::ReadOnly)) {
            const QJsonObject c = QJsonDocument::fromJson(capture.readAll()).object();
            const QString text = QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Compact));
            static const QRegularExpression gain(QStringLiteral("\"target_gain\":([0-9.eE+-]+)"));
            const QRegularExpressionMatch match = gain.match(text);
            if (match.hasMatch()) targetGain = match.captured(1).toDouble();
            if (c.value(QStringLiteral("render_exposure")).toDouble() > 0)
                exposure = c.value(QStringLiteral("render_exposure")).toDouble();
        }
    }

    QDir().mkpath(target);
    MergeOptions options;
    options.spatialStrength = 0.0;
    std::vector<int> levels;
    for (int k = 1; k < count; k *= 2) levels.push_back(k);
    levels.push_back(count);
    QJsonArray levelInfo;
    for (int k : levels) {
        std::vector<const BayerFrame *> pointers;
        for (int i = 0; i < k; ++i) pointers.push_back(frames[i].get());
        const std::vector<double> levelGains(gains.begin(), gains.begin() + k);
        const std::vector<Alignment> levelAlignments(alignments.begin(), alignments.begin() + (k - 1));
        MergeStats stats;
        const Planes merged = merge(pointers, levelGains, levelAlignments, noise, options, &stats);
        const QString name = k == count ? QStringLiteral("full.npy") : QStringLiteral("k%1.npy").arg(k);
        if (!writeNpy(target + QLatin1Char('/') + name, merged)) {
            out << stem << ": cannot write " << name << Qt::endl;
            return false;
        }
        QJsonObject level;
        level.insert(QStringLiteral("frames"), k);
        level.insert(QStringLiteral("file"), name);
        level.insert(QStringLiteral("rejection"), stats.rejection);
        if (k < count) {
            // As RawPipeline::renderFullFrame (spatialDenoise 1, automatic
            // floor and pilot).
            const double expectedShadow = 0.06 / (std::max(0.05, targetGain) * exposure);
            const double expectedNoise = std::sqrt(noise.meanVariance(expectedShadow) / k) / expectedShadow;
            const double severity = std::max(0.0, std::min(1.0, (expectedNoise - 0.015) / 0.085));
            MergeOptions spatial;
            spatial.spatialStrength = 1.0 + 1.5 * severity;
            spatial.spatialFloor = 0.5 * severity;
            spatial.spatialPilot = 2.5 * severity;
            MergeStats spatialStats;
            const Planes denoised = merge(pointers, levelGains, levelAlignments, noise, spatial, &spatialStats);
            const QString spatialName = QStringLiteral("k%1_spatial.npy").arg(k);
            if (!writeNpy(target + QLatin1Char('/') + spatialName, denoised)) {
                out << stem << ": cannot write " << spatialName << Qt::endl;
                return false;
            }
            level.insert(QStringLiteral("spatial_file"), spatialName);
            level.insert(QStringLiteral("severity"), severity);
        }
        levelInfo.append(level);
    }

    const QJsonObject &m = metadata[0];
    QJsonObject meta;
    meta.insert(QStringLiteral("stem"), stem);
    meta.insert(QStringLiteral("frames"), count);
    meta.insert(QStringLiteral("reference_sidecar"), QFileInfo(sidecars.value(0)).absolutePath() + QLatin1Char('/')
                + QFileInfo(m.value(QStringLiteral("raw_path")).toString()).completeBaseName() + QStringLiteral(".json"));
    meta.insert(QStringLiteral("width"), planeWidth);
    meta.insert(QStringLiteral("height"), planeHeight);
    meta.insert(QStringLiteral("cfa"), frames[0]->cfa);
    meta.insert(QStringLiteral("iso"), number(m, "iso"));
    meta.insert(QStringLiteral("exposure_time_ns"), number(m, "exposure_time_ns"));
    meta.insert(QStringLiteral("noise_source"), noiseSource);
    meta.insert(QStringLiteral("target_gain"), targetGain);
    meta.insert(QStringLiteral("render_exposure"), exposure);
    // variance(x) = scale * x + offset per CFA phase, x in the .npy units.
    meta.insert(QStringLiteral("noise_scale"), toArray(noise.scale));
    meta.insert(QStringLiteral("noise_offset"), toArray(noise.offset));
    meta.insert(QStringLiteral("as_shot_neutral"), m.value(QStringLiteral("as_shot_neutral")));
    meta.insert(QStringLiteral("color_transform"), m.value(QStringLiteral("color_transform")));
    meta.insert(QStringLiteral("levels"), levelInfo);
    QSaveFile metaFile(target + QStringLiteral("/meta.json"));
    if (!metaFile.open(QIODevice::WriteOnly)) return false;
    metaFile.write(QJsonDocument(meta).toJson());
    if (!metaFile.commit()) return false;
    out << stem << ": " << count << " frames, ISO " << number(m, "iso") << ", levels";
    for (const QJsonValue &v : levelInfo)
        out << " " << v.toObject().value(QStringLiteral("frames")).toInt()
            << " (rejection " << QString::number(v.toObject().value(QStringLiteral("rejection")).toDouble(), 'f', 3) << ")";
    out << ", " << timer.elapsed() << " ms" << Qt::endl;
    return true;
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    int maxFrames = 0;
    if (args.size() >= 2 && args[0] == QLatin1String("--max-frames")) {
        maxFrames = args[1].toInt();
        args = args.mid(2);
    }
    if (args.size() < 2) {
        out << "usage: burstpairs [--max-frames N] <burst folder> <output folder> [stem...]" << Qt::endl;
        return 2;
    }
    const QDir source(args[0]);
    const QString output = args[1];
    const QStringList wanted = args.mid(2);

    // stem -> sidecars in frame order.
    static const QRegularExpression burstName(QStringLiteral("^(.+)_burst(\\d+)\\.json$"));
    std::map<QString, std::map<int, QString>> bursts;
    for (const QString &name : source.entryList({QStringLiteral("*_burst*.json")}, QDir::Files)) {
        const QRegularExpressionMatch match = burstName.match(name);
        if (match.hasMatch() && QFileInfo::exists(source.filePath(match.captured(1) + QStringLiteral("_burst")
                                                                  + match.captured(2) + QStringLiteral(".raw16"))))
            bursts[match.captured(1)][match.captured(2).toInt()] = source.absoluteFilePath(name);
    }
    int exported = 0, skipped = 0;
    for (const auto &burst : bursts) {
        if (!wanted.isEmpty() && !wanted.contains(burst.first)) continue;
        if (burst.second.size() < 2) {
            ++skipped;
            continue;
        }
        if (QFileInfo::exists(output + QLatin1Char('/') + burst.first + QStringLiteral("/meta.json"))) {
            out << burst.first << ": already exported" << Qt::endl;
            ++exported;
            continue;
        }
        QStringList sidecars;
        for (const auto &frame : burst.second) sidecars << frame.second;
        try {
            if (exportBurst(burst.first, sidecars, output + QLatin1Char('/') + burst.first, maxFrames)) ++exported;
            else ++skipped;
        } catch (const std::exception &e) {
            out << burst.first << ": failed: " << e.what() << Qt::endl;
            ++skipped;
        }
    }
    out << "exported " << exported << ", skipped " << skipped << " (single frames or unusable)" << Qt::endl;
    return 0;
}
