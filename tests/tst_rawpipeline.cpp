// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <QtTest>
#include <QImage>
#include <cmath>
#include <QElapsedTimer>
#include <random>
#include <thread>
#include "../src/rawpipelinemath.h"
#include "../src/rawpipeline.h"
#include "../src/rawframecheck.h"
#include "../src/raisr.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
using namespace RawPipeline;

namespace {

// Linear RGB scene in [0, 1+] used to synthesise sensor frames.
struct Scene {
    int width = 0, height = 0;
    std::vector<float> rgb;
    float at(int x, int y, int c) const
    {
        x = std::max(0, std::min(width - 1, x));
        y = std::max(0, std::min(height - 1, y));
        return rgb[(std::size_t(y) * width + x) * 3 + c];
    }
};

// Value noise used for natural-looking texture.
float valueNoise(float x, float y, unsigned seed)
{
    auto hash = [seed](int ix, int iy) {
        unsigned h = unsigned(ix) * 374761393u + unsigned(iy) * 668265263u + seed * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return float((h ^ (h >> 16)) & 0xffff) / 65535.0f;
    };
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const float a = hash(ix, iy), b = hash(ix + 1, iy), c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

// Detailed synthetic photo: textured "foliage" and "wall", coloured objects
// with hard edges, thin lines and a zone plate for resolution checks.
Scene detailScene(int width, int height)
{
    Scene scene;
    scene.width = width;
    scene.height = height;
    scene.rgb.resize(std::size_t(width) * height * 3);
    const float scale = width / 1024.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = x / scale, v = y / scale;
            float texture = 0, amplitude = 0.5f, frequency = 1.0f / 64;
            for (int octave = 0; octave < 6; ++octave) {
                texture += amplitude * valueNoise(u * frequency, v * frequency, 7 + octave);
                amplitude *= 0.55f;
                frequency *= 2.1f;
            }
            float r = 0.10f + 0.25f * texture, g = 0.18f + 0.35f * texture, b = 0.06f + 0.12f * texture;
            if (v < 260) { // wall with brick lines
                const bool mortar = std::fmod(v, 40.0f) < 3 || std::fmod(u + (int(v / 40) % 2) * 45, 90.0f) < 3;
                const float brick = 0.35f + 0.25f * texture;
                r = mortar ? 0.55f : brick * 1.2f;
                g = mortar ? 0.52f : brick * 0.65f;
                b = mortar ? 0.48f : brick * 0.45f;
            }
            const float dx = u - 760, dy = v - 470;
            if (dx * dx + dy * dy < 90 * 90) { r = 0.70f; g = 0.12f; b = 0.08f; }      // red ball
            if (u > 120 && u < 330 && v > 520 && v < 700) { r = 0.08f; g = 0.15f; b = 0.55f; } // blue box
            if (u > 400 && u < 620 && v > 300 && v < 520) { // zone plate
                const float zx = u - 510, zy = v - 410;
                const float zone = 0.5f + 0.5f * std::cos((zx * zx + zy * zy) * 0.0016f * 3.14159f);
                r = g = b = 0.05f + 0.6f * zone;
            }
            if (std::abs(u - 900) < 1.0f || std::abs(v - 720) < 0.8f) { r = g = b = 0.9f; } // thin lines
            float *p = &scene.rgb[(std::size_t(y) * width + x) * 3];
            p[0] = r; p[1] = g; p[2] = b;
        }
    }
    return scene;
}

// High dynamic range test scene: bright sky with a sun-lit cloud gradient
// above a dark textured foreground, plus saturated colour patches.
Scene hdrScene(int width, int height)
{
    Scene scene;
    scene.width = width;
    scene.height = height;
    scene.rgb.resize(std::size_t(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float r, g, b;
            const float u = float(x) / width, v = float(y) / height;
            if (v < 0.45f) {
                // Sky: blue gradient with bright clouds, 3 to 8x brighter than
                // the foreground mid tones.
                const float cloud = 0.5f + 0.5f * std::sin(u * 23.0f + std::sin(v * 17.0f) * 3.0f);
                r = 0.35f + 0.55f * cloud;
                g = 0.55f + 0.40f * cloud;
                b = 0.95f + 0.05f * cloud;
                const float scale = 4.0f;
                r *= scale; g *= scale; b *= scale;
            } else {
                // Foreground: fine texture in deep shadow, brighter towards the right.
                const float texture = 0.5f + 0.5f * std::sin(x * 0.9f) * std::sin(y * 0.7f);
                const float light = 0.02f + 0.25f * u;
                r = light * (0.6f + 0.4f * texture);
                g = light * (0.5f + 0.5f * texture);
                b = light * (0.4f + 0.3f * texture);
                if (v > 0.8f && u > 0.1f && u < 0.3f) { r = 0.6f; g = 0.05f; b = 0.05f; }
                if (v > 0.8f && u > 0.4f && u < 0.6f) { r = 0.05f; g = 0.5f; b = 0.08f; }
            }
            float *p = &scene.rgb[(std::size_t(y) * width + x) * 3];
            p[0] = r; p[1] = g; p[2] = b;
        }
    }
    return scene;
}

// Mosaic `scene` shifted by (shiftX, shiftY) mosaic pixels, scaled by
// `exposure` (scene 1.0 maps to the white level when exposure is 1), with
// shot + read noise (variance = s * x + o in normalised units).
BayerFrame mosaicFrame(const Scene &scene, int cfa, int shiftX, int shiftY, double exposure,
                       double s, double o, unsigned seed,
                       const std::array<double, 3> &cameraResponse = {{0.5, 1.0, 0.7}})
{
    BayerFrame frame;
    frame.width = scene.width;
    frame.height = scene.height;
    frame.cfa = cfa;
    frame.white = 1023;
    frame.black = {{64, 64, 64, 64}};
    frame.pixels.resize(std::size_t(frame.width) * frame.height);
    std::mt19937 random(seed);
    std::normal_distribution<double> normal(0.0, 1.0);
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const int phase = (y & 1) * 2 + (x & 1);
            const int channel = phaseChannel(cfa, phase);
            double value = scene.at(x - shiftX, y - shiftY, channel) * exposure * cameraResponse[channel];
            value = std::min(1.0, value);
            const double sigma = std::sqrt(std::max(0.0, s * value + o));
            value += sigma * normal(random);
            const double dn = 64 + value * (1023 - 64);
            frame.pixels[std::size_t(y) * frame.width + x] = uint16_t(std::max(0.0, std::min(1023.0, std::round(dn))));
        }
    }
    return frame;
}

// Sensor frame integrating a finely rendered scene (`factor` scene pixels per
// sensor pixel), so shifts can be fractional like real hand shake.
BayerFrame mosaicFromFine(const Scene &fine, int factor, double shiftX, double shiftY, double exposure,
                          double s, double o, unsigned seed,
                          const std::array<double, 3> &cameraResponse = {{0.5, 1.0, 0.7}})
{
    BayerFrame frame;
    frame.width = fine.width / factor;
    frame.height = fine.height / factor;
    frame.cfa = 0;
    frame.white = 1023;
    frame.black = {{64, 64, 64, 64}};
    frame.pixels.resize(std::size_t(frame.width) * frame.height);
    const int qx = int(std::lround(shiftX * factor)), qy = int(std::lround(shiftY * factor));
    std::mt19937 random(seed);
    std::normal_distribution<double> normal(0.0, 1.0);
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const int channel = phaseChannel(0, (y & 1) * 2 + (x & 1));
            double sum = 0;
            for (int j = 0; j < factor; ++j)
                for (int i = 0; i < factor; ++i) sum += fine.at(factor * x - qx + i, factor * y - qy + j, channel);
            double value = std::min(1.0, sum / (factor * factor) * exposure * cameraResponse[channel]);
            value += std::sqrt(std::max(0.0, s * value + o)) * normal(random);
            frame.pixels[std::size_t(y) * frame.width + x] =
                    uint16_t(std::max(0.0, std::min(1023.0, std::round(64 + value * 959))));
        }
    }
    return frame;
}

// Lens blur of a fine scene (Gaussian, sigma in scene pixels).
Scene blurred(const Scene &scene, double sigma)
{
    const int radius = int(std::ceil(3 * sigma));
    std::vector<double> kernel(2 * radius + 1);
    double total = 0;
    for (int i = -radius; i <= radius; ++i) total += kernel[i + radius] = std::exp(-0.5 * i * i / (sigma * sigma));
    for (double &k : kernel) k /= total;
    Scene temp = scene, out = scene;
    for (int y = 0; y < scene.height; ++y)
        for (int x = 0; x < scene.width; ++x)
            for (int c = 0; c < 3; ++c) {
                double v = 0;
                for (int i = -radius; i <= radius; ++i) v += kernel[i + radius] * scene.at(x + i, y, c);
                temp.rgb[(std::size_t(y) * scene.width + x) * 3 + c] = float(v);
            }
    for (int y = 0; y < scene.height; ++y)
        for (int x = 0; x < scene.width; ++x)
            for (int c = 0; c < 3; ++c) {
                double v = 0;
                for (int i = -radius; i <= radius; ++i) v += kernel[i + radius] * temp.at(x, y + i, c);
                out.rgb[(std::size_t(y) * scene.width + x) * 3 + c] = float(v);
            }
    return out;
}

FinishOptions finishOptions(int cfa)
{
    FinishOptions options;
    options.cfa = cfa;
    // Undo the synthetic camera response {0.5, 1.0, 0.7}.
    options.wbGains = {{2.0, 1.0, 1.0, 1.0 / 0.7}};
    return options;
}

double rmsError(const Planes &a, const Planes &b, int margin)
{
    double sum = 0;
    long count = 0;
    for (int p = 0; p < 4; ++p) {
        for (int y = margin; y < a.height - margin; ++y) {
            for (int x = margin; x < a.width - margin; ++x) {
                const std::size_t i = std::size_t(y) * a.width + x;
                const double d = a.phase[p][i] - b.phase[p][i];
                sum += d * d;
                ++count;
            }
        }
    }
    return std::sqrt(sum / std::max(1L, count));
}

void dump(const RgbImage &image, const char *name)
{
    const QByteArray dir = qgetenv("RAWPIPELINE_DUMP_DIR");
    if (dir.isEmpty()) return;
    QImage out(image.rgb.data(), image.width, image.height, image.width * 3, QImage::Format_RGB888);
    out.save(QString::fromLocal8Bit(dir) + QLatin1Char('/') + QLatin1String(name) + QLatin1String(".png"));
}

} // namespace

class RawPipelineTest : public QObject {
    Q_OBJECT
private slots:
    void fftRoundTrip()
    {
        float re[256], im[256], original[256];
        std::mt19937 random(1);
        for (int i = 0; i < 256; ++i) {
            original[i] = re[i] = float(random() % 1000) / 1000.0f;
            im[i] = 0;
        }
        fft16x16(re, im, false);
        // DC term is the sum of all samples.
        double sum = 0;
        for (float v : original) sum += v;
        QVERIFY(std::abs(re[0] - sum) < 1e-3);
        fft16x16(re, im, true);
        for (int i = 0; i < 256; ++i) QVERIFY(std::abs(re[i] / 256 - original[i]) < 1e-5);
    }

    void demosaicFlatAndGradient()
    {
        for (int cfa = 0; cfa < 4; ++cfa) {
            const int w = 32, h = 24;
            std::vector<float> mosaic(w * h);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int channel = phaseChannel(cfa, (y & 1) * 2 + (x & 1));
                    // Smooth per-channel ramps: MHC must reproduce them exactly in the interior.
                    mosaic[y * w + x] = 0.2f + 0.1f * channel + 0.005f * x + 0.003f * y;
                }
            }
            const std::vector<float> rgb = demosaic(mosaic, w, h, cfa);
            for (int y = 3; y < h - 3; ++y) {
                for (int x = 3; x < w - 3; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        const float expected = 0.2f + 0.1f * c + 0.005f * x + 0.003f * y;
                        QVERIFY2(std::abs(rgb[(y * w + x) * 3 + c] - expected) < 1e-4,
                                 qPrintable(QStringLiteral("cfa %1 at %2,%3 c%4").arg(cfa).arg(x).arg(y).arg(c)));
                    }
                }
            }
        }
    }

    void alignmentRecoversShift()
    {
        const Scene scene = detailScene(512, 384);
        QVERIFY(scene.width);
        const BayerFrame ref = mosaicFrame(scene, 0, 0, 0, 1.0, 2e-4, 1e-5, 1);
        // Shifts are in mosaic pixels; alignment works in plane pixels (half).
        const int shifts[][2] = {{6, -4}, {-38, 22}, {-56, 40}};
        for (const auto &shift : shifts) {
            const BayerFrame alt = mosaicFrame(scene, 0, shift[0], shift[1], 1.0, 2e-4, 1e-5, 2);
            const Planes rp = toPlanes(ref, 1.0), ap = toPlanes(alt, 1.0);
            const Alignment alignment = align(grayImage(rp), grayImage(ap), rp.width, rp.height);
            int correct = 0, interior = 0;
            for (int ty = 4; ty < alignment.tilesY - 4; ++ty) {
                for (int tx = 4; tx < alignment.tilesX - 4; ++tx) {
                    const int px = tx * MergeTileStride, py = ty * MergeTileStride;
                    // Skip tiles whose shifted source falls outside the frame.
                    if (px + shift[0] / 2 < 8 || py + shift[1] / 2 < 8
                            || px + shift[0] / 2 > rp.width - 8 || py + shift[1] / 2 > rp.height - 8) continue;
                    // Flat tiles (inside the ball, the box) match at any offset.
                    double sum = 0, sum2 = 0;
                    const std::vector<float> gray = grayImage(rp);
                    for (int y = py - 8; y < py + 8; ++y)
                        for (int x = px - 8; x < px + 8; ++x) {
                            const double v = gray[std::size_t(y) * rp.width + x];
                            sum += v; sum2 += v * v;
                        }
                    if (sum2 / 256 - (sum / 256) * (sum / 256) < 1e-4) continue;
                    ++interior;
                    const TileOffset o = alignment.at(tx, ty);
                    if (o.dx == shift[0] / 2 && o.dy == shift[1] / 2) ++correct;
                }
            }
            QVERIFY(interior > 50);
            QVERIFY2(correct >= interior * 9 / 10,
                     qPrintable(QStringLiteral("shift %1,%2: %3/%4 tiles").arg(shift[0]).arg(shift[1]).arg(correct).arg(interior)));
        }
    }

    void subpixelAlignment()
    {
        // Lens blur of ~0.6 sensor pixels, as real optics have.
        const Scene fine = blurred(detailScene(2048, 1536), 2.5);
        const std::array<double, 3> response{{0.5, 1.0, 0.7}};
        const BayerFrame ref = mosaicFromFine(fine, 4, 0, 0, 1.0, 1e-4, 1e-6, 1, response);
        const double shifts[][2] = {{0.5, 0.25}, {1.25, -0.75}, {-2.5, 1.5}};
        for (const auto &shift : shifts) {
            const BayerFrame alt = mosaicFromFine(fine, 4, shift[0], shift[1], 1.0, 1e-4, 1e-6, 2, response);
            const std::vector<float> rg = greenImage(ref, 1.0), ag = greenImage(alt, 1.0);
            const Alignment alignment = align(rg, ag, ref.width / 2, ref.height / 2);
            std::vector<double> errors;
            for (int ty = 4; ty < alignment.tilesY - 4; ++ty) {
                for (int tx = 4; tx < alignment.tilesX - 4; ++tx) {
                    const int px = tx * MergeTileStride, py = ty * MergeTileStride;
                    double sum = 0, sum2 = 0;
                    for (int y = py - 8; y < py + 8; ++y)
                        for (int x = px - 8; x < px + 8; ++x) {
                            const double v = rg[std::size_t(y) * (ref.width / 2) + x];
                            sum += v; sum2 += v * v;
                        }
                    // Only textured tiles: without texture there is nothing to
                    // align on, and misalignment there costs no detail either.
                    if (sum2 / 256 - (sum / 256) * (sum / 256) < 4e-4) continue;
                    const TileOffset o = alignment.at(tx, ty);
                    // Plane pixels are two sensor pixels.
                    errors.push_back(std::hypot(2 * (o.dx + o.fx) - shift[0], 2 * (o.dy + o.fy) - shift[1]));
                }
            }
            std::sort(errors.begin(), errors.end());
            const double median = errors[errors.size() / 2];
            qInfo("shift %.2f,%.2f: median error %.3f sensor px over %zu tiles", shift[0], shift[1], median, errors.size());
            QVERIFY2(median < 0.2, qPrintable(QStringLiteral("median error %1").arg(median)));
        }
    }

    void superResolutionAddsDetail()
    {
        // Lens-blurred fine scene, six frames shifted by fractions of a pixel.
        const int factor = 4;
        const Scene fine = blurred(detailScene(2048, 1536), 2.5);
        // Daylight burst: noise of the IMX766 at ISO 100 (measured from a
        // device capture), captured ~1.7 EV under the metered exposure.
        const double exposure = 0.3, s = 6.5e-5, o = 3e-7;
        const double shifts[][2] = {{0, 0}, {0.5, 0.25}, {1.25, -0.5}, {-0.75, 0.75}, {0.25, -1.25}, {1.5, 1.0}};
        std::vector<BayerFrame> frames;
        for (int i = 0; i < 6; ++i)
            frames.push_back(mosaicFromFine(fine, factor, shifts[i][0], shifts[i][1], exposure, s, o, 500 + i));
        const int width = frames[0].width, height = frames[0].height;
        // Truth: the scene's green at every sensor pixel, which no mosaic has.
        std::vector<float> truth(std::size_t(width) * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                double sum = 0;
                for (int j = 0; j < factor; ++j)
                    for (int i = 0; i < factor; ++i) sum += fine.at(factor * x + i, factor * y + j, 1);
                truth[std::size_t(y) * width + x] = float(std::min(1.0, sum / (factor * factor) * exposure));
            }
        std::vector<const BayerFrame *> pointers;
        for (const BayerFrame &f : frames) pointers.push_back(&f);
        const std::vector<double> gains(6, 1.0);
        const std::vector<float> reference = greenImage(frames[0], 1.0);
        std::vector<Alignment> alignments;
        for (int i = 1; i < 6; ++i) alignments.push_back(align(reference, greenImage(frames[i], 1.0), width / 2, height / 2));
        NoiseModel noise;
        QVERIFY(noiseFromProfile({s, o}, &noise));
        const Planes merged = merge(pointers, gains, alignments, noise, MergeOptions(), nullptr);
        // Current path: demosaic the merged planes.
        std::vector<float> mosaic(std::size_t(width) * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                mosaic[std::size_t(y) * width + x] = merged.phase[(y & 1) * 2 + (x & 1)][std::size_t(y / 2) * merged.width + x / 2];
        const std::vector<float> rgb = demosaic(mosaic, width, height, 0);
        QElapsedTimer timer;
        timer.start();
        const std::vector<float> green = superResolveGreen(pointers, gains, alignments, merged, noise);
        const qint64 srMs = timer.elapsed();
        auto rms = [&](auto value) {
            double sum = 0; long n = 0;
            for (int y = 24; y < height - 24; ++y)
                for (int x = 24; x < width - 24; ++x) {
                    const double d = value(x, y) - truth[std::size_t(y) * width + x];
                    if (std::isfinite(d)) { sum += d * d; ++n; }
                }
            return std::sqrt(sum / std::max(1L, n));
        };
        const double demosaicError = rms([&](int x, int y) { return rgb[(std::size_t(y) * width + x) * 3 + 1]; });
        const double srError = rms([&](int x, int y) { return green[std::size_t(y) * width + x]; });
        int holes = 0;
        for (float v : green) holes += !std::isfinite(v);
        // What finish() does: coarse structure from the merge, fine detail
        // from the super-resolved green.
        std::vector<float> demosaicked(std::size_t(width) * height), srFilled(green);
        for (std::size_t i = 0; i < demosaicked.size(); ++i) {
            demosaicked[i] = rgb[i * 3 + 1];
            if (!std::isfinite(srFilled[i])) srFilled[i] = demosaicked[i];
        }
        auto lowpass = [&](const std::vector<float> &v) {
            // 5-tap binomial (sigma ~1) separable blur.
            std::vector<float> t(v.size()), out(v.size());
            const float k[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                float a = 0; for (int i = -2; i <= 2; ++i) a += k[i + 2] * v[std::size_t(y) * width + std::max(0, std::min(width - 1, x + i))];
                t[std::size_t(y) * width + x] = a; }
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                float a = 0; for (int i = -2; i <= 2; ++i) a += k[i + 2] * t[std::size_t(std::max(0, std::min(height - 1, y + i))) * width + x];
                out[std::size_t(y) * width + x] = a; }
            return out;
        };
        const std::vector<float> srLow = lowpass(srFilled), dmLow = lowpass(demosaicked);
        const double combinedError = rms([&](int x, int y) {
            const std::size_t i = std::size_t(y) * width + x;
            return demosaicked[i] + (srFilled[i] - srLow[i]) - (demosaicked[i] - dmLow[i]);
        });
        qInfo("green error vs truth: demosaicked merge %.5f, super-resolved %.5f, combined %.5f (%lld ms, %d holes)",
              demosaicError, srError, combinedError, srMs, holes);
        QVERIFY2(combinedError < demosaicError * 0.95, "super-resolved detail should bring the result closer to the scene");

        if (!qgetenv("RAWPIPELINE_DUMP_DIR").isEmpty()) {
            FinishOptions plain = finishOptions(0);
            plain.targetGain = 1.0 / exposure;
            dump(finish(merged, plain, nullptr), "superres_off");
            FinishOptions with = plain;
            std::vector<float> detail = green;
            with.detailGreen = &detail;
            dump(finish(merged, with, nullptr), "superres_on");
        }

        // A bright square only in the alternate frames must not ghost in.
        std::vector<BayerFrame> moving = frames;
        for (int i = 1; i < 6; ++i)
            for (int y = 150; y < 200; ++y)
                for (int x = 200 + 10 * i; x < 250 + 10 * i; ++x) moving[i].pixels[std::size_t(y) * width + x] = 1000;
        pointers.clear();
        for (const BayerFrame &f : moving) pointers.push_back(&f);
        const std::vector<float> robust = superResolveGreen(pointers, gains, alignments, merged, noise);
        double ghost = 0;
        for (int y = 160; y < 190; ++y)
            for (int x = 262; x < 300; ++x)
                if (std::isfinite(robust[std::size_t(y) * width + x]))
                    ghost = std::max(ghost, double(robust[std::size_t(y) * width + x] - truth[std::size_t(y) * width + x]));
        qInfo("moving square: largest excess %.3f", ghost);
        QVERIFY2(ghost < 0.15, qPrintable(QStringLiteral("ghost %1").arg(ghost)));
    }

    void noiseEstimate()
    {
        const Scene scene = hdrScene(512, 384);
        const double s = 4e-4, o = 2e-5;
        const BayerFrame frame = mosaicFrame(scene, 0, 0, 0, 0.2, s, o, 3);
        const NoiseModel model = estimateNoise(toPlanes(frame, 1.0));
        // Quantisation (1/959 per step) adds ~9e-8 variance; ignore it.
        for (int p = 0; p < 4; ++p) {
            const double expected = s * 0.1 + o, estimated = model.variance(p, 0.1);
            QVERIFY2(estimated > expected * 0.7 && estimated < expected * 1.4,
                     qPrintable(QStringLiteral("phase %1: %2 vs %3").arg(p).arg(estimated).arg(expected)));
        }
        NoiseModel profile;
        QVERIFY(noiseFromProfile({s, o}, &profile));
        QCOMPARE(profile.variance(2, 0.5), s * 0.5 + o);
        QVERIFY(!noiseFromProfile({-1, 0}, &profile));
        QVERIFY(!noiseFromProfile({}, &profile));
    }

    void mergeReducesNoiseAndRejectsMotion()
    {
        const Scene scene = detailScene(512, 384);
        QVERIFY(scene.width);
        const double s = 1e-3, o = 4e-5, exposure = 0.5;
        const Planes clean = toPlanes(mosaicFrame(scene, 0, 0, 0, exposure, 0, 0, 9), 1.0);
        std::vector<Planes> frames;
        std::vector<Alignment> alignments;
        const int count = 8;
        for (int i = 0; i < count; ++i) {
            const int sx = (i % 3) * 4 - 4, sy = (i % 2) * 6 - 2;
            frames.push_back(toPlanes(mosaicFrame(scene, 0, i ? sx : 0, i ? sy : 0, exposure, s, o, 100 + i), 1.0));
        }
        const std::vector<float> refGray = grayImage(frames[0]);
        for (int i = 1; i < count; ++i)
            alignments.push_back(align(refGray, grayImage(frames[i]), frames[0].width, frames[0].height));
        std::vector<const Planes *> pointers;
        for (const Planes &f : frames) pointers.push_back(&f);
        NoiseModel noise;
        QVERIFY(noiseFromProfile({s, o}, &noise));
        MergeOptions options;
        options.spatialStrength = 0;
        MergeStats stats;
        QElapsedTimer timer;
        timer.start();
        const Planes merged = merge(pointers, alignments, noise, options, &stats);
        const qint64 mergeMs = timer.elapsed();
        const double single = rmsError(frames[0], clean, 16), burst = rmsError(merged, clean, 16);
        qInfo("merge %d frames 256x192 planes: %lld ms, rms single %.5f burst %.5f, rejection %.3f",
              count, mergeMs, single, burst, stats.rejection);
        // Ideal averaging gives sqrt(8) = 2.83; robustness costs a little.
        QVERIFY2(burst < single / 2.0, qPrintable(QStringLiteral("%1 vs %2").arg(burst).arg(single)));

        // A bright square present only in the alternates must not ghost into the result.
        std::vector<Planes> moving = frames;
        for (int i = 1; i < count; ++i) {
            for (int p = 0; p < 4; ++p) {
                for (int y = 80; y < 120; ++y)
                    for (int x = 100 + 4 * i; x < 140 + 4 * i; ++x)
                        moving[i].phase[p][std::size_t(y) * moving[i].width + x] = 0.95f;
            }
        }
        pointers.clear();
        for (const Planes &f : moving) pointers.push_back(&f);
        const Planes deghosted = merge(pointers, alignments, noise, options, nullptr);
        double ghost = 0;
        for (int y = 90; y < 110; ++y)
            for (int x = 150; x < 160; ++x)
                ghost = std::max(ghost, double(deghosted.phase[1][std::size_t(y) * deghosted.width + x]
                                               - clean.phase[1][std::size_t(y) * clean.width + x]));
        QVERIFY2(ghost < 0.12, qPrintable(QStringLiteral("ghost %1").arg(ghost)));
    }

    void mosaicMergeMatchesPlanes()
    {
        const Scene scene = detailScene(256, 192);
        std::vector<BayerFrame> mosaics;
        for (int i = 0; i < 3; ++i) mosaics.push_back(mosaicFrame(scene, 2, 2 * i, -2 * i, 0.5, 1e-3, 4e-5, 40 + i));
        const std::vector<double> gains{1.0, 1.0, 1.0};
        std::vector<Planes> planes;
        for (const BayerFrame &m : mosaics) planes.push_back(toPlanes(m, 1.0));
        const std::vector<float> gray = grayImage(planes[0]), mosaicGray = grayImage(mosaics[0], 1.0);
        for (std::size_t i = 0; i < gray.size(); ++i) QVERIFY(std::abs(gray[i] - mosaicGray[i]) < 1e-6);
        std::vector<Alignment> alignments;
        for (int i = 1; i < 3; ++i) alignments.push_back(align(gray, grayImage(planes[i]), planes[0].width, planes[0].height));
        NoiseModel noise;
        QVERIFY(noiseFromProfile({1e-3, 4e-5}, &noise));
        const Planes a = merge({&planes[0], &planes[1], &planes[2]}, alignments, noise, MergeOptions(), nullptr);
        const Planes b = merge({&mosaics[0], &mosaics[1], &mosaics[2]}, gains, alignments, noise, MergeOptions(), nullptr);
        QVERIFY(rmsError(a, b, 0) < 1e-6);
    }

    void renderBurstFromFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const Scene scene = detailScene(320, 240);
        QStringList paths;
        for (int i = 0; i < 4; ++i) {
            const bool packed = i == 3; // RAW10 frames are accepted too
            const BayerFrame frame = mosaicFrame(scene, 3, 2 * i, -2 * i, 0.4, 1e-3, 4e-5, 70 + i);
            const QString raw = dir.path() + QStringLiteral("/frame%1.raw").arg(i);
            QFile file(raw);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QByteArray bytes;
            for (int y = 0; y < frame.height; ++y) {
                if (packed) {
                    for (int x = 0; x < frame.width; x += 4) {
                        unsigned char low = 0;
                        for (int k = 0; k < 4; ++k) {
                            const uint16_t v = frame.pixels[std::size_t(y) * frame.width + x + k];
                            bytes.append(char(v >> 2));
                            low |= (v & 3) << (k * 2);
                        }
                        bytes.append(char(low));
                    }
                } else {
                    for (int x = 0; x < frame.width; ++x) {
                        const uint16_t v = frame.pixels[std::size_t(y) * frame.width + x];
                        bytes.append(char(v & 255));
                        bytes.append(char(v >> 8));
                    }
                }
            }
            file.write(bytes);
            file.close();
            QJsonArray matrix;
            for (int k = 0; k < 9; ++k) matrix.append(QJsonArray{k % 4 == 0 ? 1 : 0, 1});
            const QJsonObject m{{"camera_id", "0"}, {"width", frame.width}, {"height", frame.height},
                                {"format", packed ? "RAW10" : "RAW16"}, {"bits_per_sample", packed ? 10 : 16},
                                {"row_stride", packed ? frame.width * 5 / 4 : frame.width * 2}, {"cfa", "BGGR"},
                                {"white_level", 1023}, {"black_level_pattern", QJsonArray{64, 64, 64, 64}},
                                {"raw_path", raw}, {"iso", 400}, {"exposure_time_ns", 10000000},
                                {"color_correction_gains", QJsonArray{2.0, 1.0, 1.0, 1.43}},
                                {"capture_color_transform", matrix}, {"noise_profile", QJsonArray{1e-3, 4e-5}},
                                {"bracket_rotation", 90}};
            QFile metadata(dir.path() + QStringLiteral("/frame%1.json").arg(i));
            QVERIFY(metadata.open(QIODevice::WriteOnly));
            metadata.write(QJsonDocument(m).toJson());
            paths << metadata.fileName();
        }
        RenderSettings settings;
        settings.targetGain = 2.5;
        QString error;
        const QString target = dir.path() + QStringLiteral("/merged.jpg");
        QVERIFY2(renderBurst(target, paths, 95, settings, &error), qPrintable(error));
        const QImage image(target);
        QCOMPARE(image.size(), QSize(240, 320)); // rotated
        QFile sidecar(dir.path() + QStringLiteral("/merged.json"));
        QVERIFY(sidecar.open(QIODevice::ReadOnly));
        const QJsonObject m = QJsonDocument::fromJson(sidecar.readAll()).object();
        QCOMPARE(m.value("bracket_count").toInt(), 4);
        const QJsonObject info = m.value("raw_pipeline").toObject();
        QCOMPARE(info.value("frames").toInt(), 4);
        QCOMPARE(info.value("noise_model_source").toString(), QStringLiteral("profile"));
        QVERIFY(!m.contains("raw_path"));

        // "Mirror selfies": bracket_mirror flips the upright photo
        // left-right and changes nothing else.
        {
            QStringList mirrored;
            for (int i = 0; i < paths.size(); ++i) {
                QFile in(paths[i]);
                QVERIFY(in.open(QIODevice::ReadOnly));
                QJsonObject frame = QJsonDocument::fromJson(in.readAll()).object();
                frame.insert("bracket_mirror", true);
                QFile out(dir.path() + QStringLiteral("/mirror%1.json").arg(i));
                QVERIFY(out.open(QIODevice::WriteOnly));
                out.write(QJsonDocument(frame).toJson());
                mirrored << out.fileName();
            }
            const QString mirrorTarget = dir.path() + QStringLiteral("/merged-mirror.jpg");
            QVERIFY2(renderBurst(mirrorTarget, mirrored, 95, settings, &error), qPrintable(error));
            const QImage flipped = QImage(mirrorTarget).convertToFormat(QImage::Format_RGB888);
            const QImage normal = image.convertToFormat(QImage::Format_RGB888);
            QCOMPARE(flipped.size(), normal.size());
            auto meanDifference = [](const QImage &a, const QImage &b) {
                double sum = 0;
                for (int y = 0; y < a.height(); ++y) {
                    const uchar *pa = a.constScanLine(y), *pb = b.constScanLine(y);
                    for (int x = 0; x < a.width() * 3; ++x) sum += std::abs(int(pa[x]) - int(pb[x]));
                }
                return sum / (a.width() * a.height() * 3.0);
            };
            const double asMirror = meanDifference(flipped, normal.mirrored(true, false));
            const double asNormal = meanDifference(flipped, normal);
            QVERIFY2(asMirror < 2.0 && asNormal > 4.0 * asMirror,
                     qPrintable(QStringLiteral("mirror %1 normal %2").arg(asMirror).arg(asNormal)));
        }

        // Single frame through the same entry point.
        QImage single;
        QJsonObject singleInfo;
        QVERIFY2(renderImage({paths[0]}, RenderSettings(), &single, &singleInfo, &error), qPrintable(error));
        QCOMPARE(single.size(), QSize(320, 240));

        // A frame still at the preview's exposure (camera applied the burst
        // exposure late) is dropped; frames matching the request are kept,
        // and so are all frames when none matches it.
        auto rewrite = [&](int index, const QString &name, qint64 exposure, qint64 requested) {
            QFile in(paths[index]);
            if (!in.open(QIODevice::ReadOnly)) return QString();
            QJsonObject m = QJsonDocument::fromJson(in.readAll()).object();
            m.insert("exposure_time_ns", double(exposure));
            if (requested > 0) {
                m.insert("exposure_time_requested_ns", double(requested));
                m.insert("sensor_sensitivity_requested", 400);
            }
            QFile out(dir.path() + "/" + name);
            if (!out.open(QIODevice::WriteOnly)) return QString();
            out.write(QJsonDocument(m).toJson());
            return out.fileName();
        };
        const qint64 nominal = 10000000, late = 40000000;
        for (const qint64 requested : {nominal, qint64(0)}) {
            QStringList burst;
            for (int i = 0; i < 4; ++i)
                burst << rewrite(i, QStringLiteral("late%1_%2.json").arg(requested).arg(i),
                                 i == 1 ? late : nominal + 7000 * i, requested);
            QImage merged;
            QJsonObject mergedInfo;
            QVERIFY2(renderImage(burst, settings, &merged, &mergedInfo, &error), qPrintable(error));
            QCOMPARE(mergedInfo.value("frames").toInt(), 3);
            QCOMPARE(mergedInfo.value("exposure_outliers").toInt(), 1);
        }
        QStringList ignored;
        for (int i = 0; i < 3; ++i)
            ignored << rewrite(i, QStringLiteral("ignored%1.json").arg(i), late, nominal);
        QImage kept;
        QJsonObject keptInfo;
        QVERIFY2(renderImage(ignored, settings, &kept, &keptInfo, &error), qPrintable(error));
        QCOMPARE(keptInfo.value("frames").toInt(), 3);
        QVERIFY(!keptInfo.contains("exposure_outliers"));

        // Mismatched geometry is rejected without output.
        QFile other(paths[1]);
        QVERIFY(other.open(QIODevice::ReadOnly));
        QJsonObject bad = QJsonDocument::fromJson(other.readAll()).object();
        other.close();
        bad.insert("camera_id", "1");
        QFile badFile(dir.path() + "/bad.json");
        QVERIFY(badFile.open(QIODevice::WriteOnly));
        badFile.write(QJsonDocument(bad).toJson());
        badFile.close();
        QVERIFY(!renderBurst(dir.path() + "/bad.jpg", {paths[0], badFile.fileName()}, 90, settings, &error));
        QVERIFY(!QFile::exists(dir.path() + "/bad.jpg"));

        // Partly written frames (the Xperia 10 III's RAW: data in the first
        // quarter of the buffer, the rest never written) are left out; when
        // all are, the error says the camera's RAW output is damaged.
        auto damaged = [&](int index) {
            QFile in(paths[index]);
            if (!in.open(QIODevice::ReadOnly)) return QString();
            QJsonObject m = QJsonDocument::fromJson(in.readAll()).object();
            QFile source(m.value("raw_path").toString());
            if (!source.open(QIODevice::ReadOnly)) return QString();
            QByteArray bytes = source.readAll();
            const int stride = m.value("row_stride").toInt(), height = m.value("height").toInt();
            std::fill(bytes.begin() + qint64(stride) * (height / 4), bytes.end(), char(0));
            QFile raw(dir.path() + QStringLiteral("/damaged%1.raw").arg(index));
            if (!raw.open(QIODevice::WriteOnly)) return QString();
            raw.write(bytes);
            m.insert("raw_path", raw.fileName());
            QFile out(dir.path() + QStringLiteral("/damaged%1.json").arg(index));
            if (!out.open(QIODevice::WriteOnly)) return QString();
            out.write(QJsonDocument(m).toJson());
            return out.fileName();
        };
        QImage partly;
        QJsonObject partlyInfo;
        QVERIFY2(renderImage({paths[0], damaged(1), paths[2]}, settings, &partly, &partlyInfo, &error),
                 qPrintable(error));
        QCOMPARE(partlyInfo.value("frames").toInt(), 2);
        QCOMPARE(partlyInfo.value("damaged_frames").toInt(), 1);
        QVERIFY(!renderBurst(dir.path() + "/damaged.jpg", {damaged(0), damaged(1), damaged(2)}, 90, settings, &error));
        QVERIFY2(isRawFramesDamagedError(error), qPrintable(error));
        QVERIFY(!QFile::exists(dir.path() + "/damaged.jpg"));
        QVERIFY(!renderImage({damaged(0)}, settings, &partly, &partlyInfo, &error));
        QVERIFY2(isRawFramesDamagedError(error), qPrintable(error));
    }

    void damagedFrameCheck()
    {
        const int width = 64, height = 48;
        const double black[4] = {64, 64, 64, 64};
        std::vector<uint16_t> pixels(std::size_t(width) * height);
        // A dark frame: noise around the black level, some below it.
        unsigned seed = 1;
        for (uint16_t &v : pixels) {
            seed = seed * 1103515245u + 12345u;
            v = uint16_t(60 + (seed >> 16) % 9);
        }
        QVERIFY(rawFrameDamage(pixels.data(), width, height, black).isEmpty());
        // A few unwritten rows (under an eighth) are tolerated...
        std::fill(pixels.end() - 5 * width, pixels.end(), uint16_t(0));
        QVERIFY(rawFrameDamage(pixels.data(), width, height, black).isEmpty());
        // ...an eighth is not, nor three quarters.
        std::fill(pixels.end() - 6 * width, pixels.end(), uint16_t(0));
        QCOMPARE(rawFrameDamage(pixels.data(), width, height, black), QStringLiteral("6 of 48 rows unwritten"));
        std::fill(pixels.begin() + 12 * width, pixels.end(), uint16_t(0));
        QVERIFY(!rawFrameDamage(pixels.data(), width, height, black).isEmpty());
        // A row with one written pixel is written.
        for (int y = 12; y < height; ++y) pixels[std::size_t(y) * width + width - 1] = 64;
        QVERIFY(rawFrameDamage(pixels.data(), width, height, black).isEmpty());
        // A camera reporting black 0 is not judged: its dark rows can be 0.
        std::fill(pixels.begin(), pixels.end(), uint16_t(0));
        const double zero[4] = {0, 0, 0, 0};
        QVERIFY(rawFrameDamage(pixels.data(), width, height, zero).isEmpty());
        QVERIFY(!rawFrameDamage(pixels.data(), width, height, black).isEmpty());
    }

    void renderCapturedFile()
    {
        // RAWPIPELINE_RENDER=<sidecar.json>[,<sidecar.json>...] (a burst,
        // reference first) [RAWPIPELINE_OUT=<file.jpg>]
        // [RAWPIPELINE_CALIBRATION=<dir> RAWPIPELINE_MODEL=<model>]
        // [RAWPIPELINE_NO_SR=1]: render a device capture on the host (not
        // run by default).
        const QString metadata = qEnvironmentVariable("RAWPIPELINE_RENDER");
        if (metadata.isEmpty()) QSKIP("set RAWPIPELINE_RENDER=<sidecar.json> to run");
        RenderSettings settings;
        if (qEnvironmentVariableIsSet("RAWPIPELINE_CALIBRATION"))
            settings.calibrationDirs << qEnvironmentVariable("RAWPIPELINE_CALIBRATION");
        settings.deviceModel = qEnvironmentVariable("RAWPIPELINE_MODEL");
        if (qEnvironmentVariableIsSet("RAWPIPELINE_SPATIAL"))
            settings.spatialDenoise = qEnvironmentVariable("RAWPIPELINE_SPATIAL").toDouble();
        if (qEnvironmentVariableIsSet("RAWPIPELINE_FLOOR"))
            settings.spatialFloor = qEnvironmentVariable("RAWPIPELINE_FLOOR").toDouble();
        if (qEnvironmentVariableIsSet("RAWPIPELINE_PILOT"))
            settings.spatialPilot = qEnvironmentVariable("RAWPIPELINE_PILOT").toDouble();
        if (qEnvironmentVariableIsSet("RAWPIPELINE_TARGET_GAIN"))
            settings.targetGain = qEnvironmentVariable("RAWPIPELINE_TARGET_GAIN").toDouble();
        if (qEnvironmentVariableIsSet("RAWPIPELINE_NO_SR"))
            settings.superResolution = false;
        if (qEnvironmentVariableIsSet("RAWPIPELINE_NO_TONE"))
            settings.toneRefinements = false;
        QImage image;
        QJsonObject info;
        QString error;
        QElapsedTimer timer;
        timer.start();
        QVERIFY2(renderImage(metadata.split(QLatin1Char(',')), settings, &image, &info, &error),
                 qPrintable(error));
        qInfo("rendered in %lld ms: %s", timer.elapsed(), QJsonDocument(info).toJson(QJsonDocument::Compact).constData());
        const QString out = qEnvironmentVariable("RAWPIPELINE_OUT", QStringLiteral("rendered.jpg"));
        QVERIFY(image.save(out, nullptr, 92));
    }

    void denoiseStages()
    {
        // RAWPIPELINE_STAGES=<sidecar.json>: noise left after the spatial
        // denoise in the flattest dark patch of a device capture (diagnostic).
        const QString metadata = qEnvironmentVariable("RAWPIPELINE_STAGES");
        if (metadata.isEmpty()) QSKIP("set RAWPIPELINE_STAGES=<sidecar.json> to run");
        QFile file(metadata);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject m = QJsonDocument::fromJson(file.readAll()).object();
        BayerFrame frame;
        frame.width = m.value("width").toInt();
        frame.height = m.value("height").toInt();
        frame.white = m.value("white_level").toDouble();
        frame.cfa = 3;
        for (int k = 0; k < 4; ++k) frame.black[k] = m.value("black_level_pattern").toArray()[k].toDouble();
        QFile raw(m.value("raw_path").toString());
        QVERIFY(raw.open(QIODevice::ReadOnly));
        frame.pixels.resize(std::size_t(frame.width) * frame.height);
        raw.read(reinterpret_cast<char *>(frame.pixels.data()), qint64(frame.pixels.size() * 2));
        const Planes planes = toPlanes(frame, 1.0);
        const NoiseModel noise = estimateNoise(planes);
        // Flattest 64x64 block of the green plane among dark ones.
        auto residualVar = [&](const Planes &p, int bx, int by) {
            double sum2 = 0;
            for (int y = by + 1; y < by + 63; ++y)
                for (int x = bx + 1; x < bx + 63; ++x) {
                    const std::vector<float> &g = p.phase[1];
                    const std::size_t i = std::size_t(y) * p.width + x;
                    const double r = g[i] - 0.25 * (g[i - 1] + g[i + 1] + g[i - p.width] + g[i + p.width]);
                    sum2 += r * r;
                }
            return sum2 / (62 * 62);
        };
        int bestX = 0, bestY = 0;
        double best = 1e9;
        for (int by = 64; by < planes.height - 128; by += 64)
            for (int bx = 64; bx < planes.width - 128; bx += 64) {
                double mean = 0;
                for (int y = by; y < by + 64; ++y) for (int x = bx; x < bx + 64; ++x) mean += planes.phase[1][std::size_t(y) * planes.width + x];
                mean /= 4096;
                const double v = residualVar(planes, bx, by);
                if (mean < 0.02 && v < best) { best = v; bestX = bx; bestY = by; }
            }
        qInfo("patch %d,%d raw residual var %.3e", bestX, bestY, best);
        const char *names[] = {"ratio k1", "ratio k4 floor.5", "pilot 2, k2 floor.5", "pilot 3, k3 floor.5"};
        const double strengths[] = {1, 4, 2, 3};
        const double floors[] = {0, 0.5, 0.5, 0.5};
        const double pilots[] = {0, 0, 2, 3};
        for (int c = 0; c < 4; ++c) {
            MergeOptions options;
            options.spatialStrength = strengths[c];
            options.spatialFloor = floors[c];
            options.spatialPilot = pilots[c];
            const Planes out = merge({&planes}, {}, noise, options, nullptr);
            qInfo("%-22s residual var %.3e (%.1fx lower noise)", names[c], residualVar(out, bestX, bestY),
                  std::sqrt(best / residualVar(out, bestX, bestY)));
        }
    }

    void fullSizeBenchmark()
    {
        // RAWPIPELINE_BENCH=<frames>: time a full 4096x3072 burst (not run by default).
        const int count = qEnvironmentVariableIntValue("RAWPIPELINE_BENCH");
        if (count <= 0) QSKIP("set RAWPIPELINE_BENCH=<frames> to run");
        const Scene scene = detailScene(4096, 3072);
        std::vector<BayerFrame> frames;
        for (int i = 0; i < count; ++i) frames.push_back(mosaicFrame(scene, 3, 4 * i, -2 * i, 0.3, 2e-3, 1e-4, 300 + i));
        QElapsedTimer timer;
        timer.start();
        std::vector<const BayerFrame *> pointers;
        std::vector<double> gains(count, 1.0);
        for (const BayerFrame &f : frames) pointers.push_back(&f);
        const std::vector<float> ref = greenImage(frames[0], 1.0);
        std::vector<Alignment> alignments;
        for (int i = 1; i < count; ++i) alignments.push_back(align(ref, greenImage(frames[i], 1.0), 2048, 1536));
        const qint64 alignMs = timer.restart();
        NoiseModel noise;
        QVERIFY(noiseFromProfile({2e-3, 1e-4}, &noise));
        const Planes merged = merge(pointers, gains, alignments, noise, MergeOptions(), nullptr);
        const qint64 mergeMs = timer.restart();
        std::vector<float> detail = superResolveGreen(pointers, gains, alignments, merged, noise);
        const qint64 srMs = timer.restart();
        FinishOptions options = finishOptions(3);
        options.targetGain = 3.3;
        options.detailGreen = &detail;
        const RgbImage image = finish(merged, options, nullptr);
        qInfo("4096x3072 x%d on %u threads: align %lld ms, merge %lld ms, super-resolution %lld ms, finish %lld ms",
              count, std::thread::hardware_concurrency(), alignMs, mergeMs, srMs, timer.elapsed());
        QCOMPARE(image.width, 4096);
    }

    void burstPlanning()
    {
        const int64_t minT = 10000, maxT = 2000000000;
        // Bright, heavily clipped daylight: deep underexposure at base ISO.
        BurstPlan day = planBurst(4000000, 50, 0.15, minT, maxT, 50, 6400, 8);
        QCOMPARE(day.iso, 50);
        QVERIFY(day.exposureNs < 4000000 / 4);
        QVERIFY(std::abs(day.targetGain - std::pow(2.0, 2.5)) < 0.1);
        QVERIFY(day.frames >= 6 && day.frames <= 8);
        // Unclipped daylight: mild underexposure, fewer frames.
        BurstPlan soft = planBurst(4000000, 50, 0.0, minT, maxT, 50, 6400, 8);
        QVERIFY(std::abs(soft.underexposureEv - 0.5) < 0.05);
        QVERIFY(soft.frames < day.frames);
        // Night: metered at 1/15 s ISO 3200 -> the handheld limit applies and
        // the gain covers the rest, with the maximum frame count.
        BurstPlan night = planBurst(66666666, 3200, 0.001, minT, maxT, 50, 6400, 8);
        QVERIFY(night.exposureNs <= 66666667);
        QVERIFY(night.iso <= 3200);
        QCOMPARE(night.frames, 8);
        QVERIFY(std::abs(night.underexposureEv - 0.5) < 0.05);
        // Never beyond the sensor ranges; invalid input yields no plan.
        // Overcast handheld (preview 1/30 s ISO 213): short frames at a
        // higher gain rather than shake-prone 1/30 s ones.
        BurstPlan overcast = planBurst(33333333, 213, 0.0, minT, maxT, 50, 6400, 8);
        QVERIFY(overcast.exposureNs <= 16666667);
        QVERIFY(overcast.iso <= 800);
        // Dim room (preview 1/15 s ISO 584): 1/30 s, not 1/15 s.
        BurstPlan room = planBurst(66666666, 584, 0.0, minT, maxT, 50, 6400, 8);
        QVERIFY(room.exposureNs <= 33333334 && room.exposureNs > 16666667);
        QVERIFY(room.iso <= 3200);
        // Darker room (preview 1/20 s ISO 3262, a real capture that came
        // out shaken at 1/15 s): still 1/30 s.
        BurstPlan evening = planBurst(49992000, 3262, 0.01, minT, maxT, 100, 19200, 8);
        QVERIFY(evening.exposureNs <= 33333334);
        QVERIFY(evening.iso <= 3200);
        // Night mode in the dark room (preview 70 ms ISO 17912, AE at its
        // limit): every frame, longer than handheld (up to 1/5 s), more
        // light than metered, brought back down by a target gain below 1.
        BurstPlan nightRoom = planBurst(70000000, 17912, 0.0, minT, maxT, 100, 19200, 8, false, true);
        QCOMPARE(nightRoom.frames, 8);
        QVERIFY(nightRoom.exposureNs > 70000000 && nightRoom.exposureNs <= 200000000);
        QVERIFY(nightRoom.targetGain < 1.0);
        // 16 frames when the app asks for them (night mode), still at most 1/5 s each.
        BurstPlan night16 = planBurst(70000000, 17912, 0.0, minT, maxT, 100, 19200, 16, false, true);
        QCOMPARE(night16.frames, 16);
        QVERIFY(night16.exposureNs <= 200000000);
        // Measured motion replaces the fixed steps: a steady hand gets longer,
        // cleaner frames, a shaky one shorter frames at a higher gain.
        BurstScene steady, shaky, subjects;
        steady.motion = 0.002;   // ~8 px/s at 4096 px
        shaky.motion = 0.03;     // ~120 px/s
        subjects.motion = 0.002;
        subjects.subjectMotion = 0.2;
        BurstPlan roomSteady = planBurst(66666666, 584, 0.0, minT, maxT, 50, 6400, 8, false, false, steady);
        BurstPlan roomShaky = planBurst(66666666, 584, 0.0, minT, maxT, 50, 6400, 8, false, false, shaky);
        BurstPlan roomSubjects = planBurst(66666666, 584, 0.0, minT, maxT, 50, 6400, 8, false, false, subjects);
        QVERIFY(roomSteady.exposureNs > room.exposureNs && roomSteady.iso < room.iso);
        QVERIFY(roomShaky.exposureNs < room.exposureNs && roomShaky.exposureNs >= 4000000);
        QVERIFY(roomSubjects.exposureNs <= 16666667);
        // Night on a steady surface: up to 1/3 s; handheld night stays shorter.
        BurstPlan nightSteady = planBurst(70000000, 17912, 0.0, minT, maxT, 100, 19200, 16, false, true, steady);
        BurstScene still = steady;
        still.motion = 0.0;
        BurstPlan nightStill = planBurst(70000000, 17912, 0.0, minT, maxT, 100, 19200, 16, false, true, still);
        BurstPlan nightShaky = planBurst(70000000, 17912, 0.0, minT, maxT, 100, 19200, 16, false, true, shaky);
        QVERIFY(nightStill.exposureNs > 200000000 && nightStill.exposureNs <= 333333334);
        QVERIFY(nightSteady.exposureNs >= nightShaky.exposureNs);
        QVERIFY(nightShaky.exposureNs >= 66666666);
        // A face in hard light gets headroom (a front-camera YUV selfie).
        BurstScene blownFace;
        blownFace.faceLuma = 0.9;
        blownFace.faceClipped = 0.1;
        BurstPlan selfie = planBurst(20000000, 200, 0.0, minT, maxT, 50, 6400, 8, true);
        BurstPlan selfieFace = planBurst(20000000, 200, 0.0, minT, maxT, 50, 6400, 8, true, false, blownFace);
        QVERIFY(selfieFace.underexposureEv > selfie.underexposureEv + 1.0);
        // A backlit face (dark in the viewfinder): the YUV burst is exposed
        // brighter than the meter, the bright window may clip.
        BurstScene backlitFace;
        backlitFace.faceLuma = 0.05;
        BurstPlan window = planBurst(1600000, 100, 0.1, minT, maxT, 50, 6400, 8, true);
        BurstPlan windowFace = planBurst(1600000, 100, 0.1, minT, maxT, 50, 6400, 8, true, false, backlitFace);
        QVERIFY(windowFace.underexposureEv <= -1.9 && window.underexposureEv > 1.0);
        BurstPlan limited = planBurst(1000000000, 6400, 0.0, minT, 50000000, 50, 6400, 8);
        QVERIFY(limited.exposureNs <= 50000000 && limited.iso <= 6400);
        QVERIFY(limited.targetGain > 1.0 && limited.targetGain <= 16.0);
        QCOMPARE(planBurst(0, 100, 0, minT, maxT, 50, 6400, 8).frames, 0);
    }

    void clippedHighlightsStayFinite()
    {
        // Partly and fully clipped areas side by side (a bright cloud with a
        // blown core): every output pixel must be finite and the core white,
        // not black or tinted.
        const int w = 512, h = 384;
        Scene scene;
        scene.width = w;
        scene.height = h;
        scene.rgb.resize(std::size_t(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float dx = (x - w / 2) / 160.0f, dy = (y - h / 2) / 110.0f;
                const float cloud = std::max(0.0f, 1.0f - (dx * dx + dy * dy)) * 3.0f;
                float *p = &scene.rgb[(std::size_t(y) * w + x) * 3];
                p[0] = 0.45f + 0.9f * cloud; p[1] = 0.55f + 0.9f * cloud; p[2] = 0.75f + 0.9f * cloud;
            }
        }
        const Planes frame = toPlanes(mosaicFrame(scene, 3, 0, 0, 1.0, 1e-4, 1e-6, 5), 1.0);
        FinishOptions options = finishOptions(3);
        const RgbImage image = finish(frame, options, nullptr);
        dump(image, "clipped_cloud");
        double core[3] = {0, 0, 0};
        int n = 0;
        for (int y = h / 2 - 10; y < h / 2 + 10; ++y)
            for (int x = w / 2 - 10; x < w / 2 + 10; ++x, ++n)
                for (int c = 0; c < 3; ++c) core[c] += image.rgb[(std::size_t(y) * w + x) * 3 + c];
        for (double &v : core) v /= n;
        qInfo("blown core %.0f %.0f %.0f", core[0], core[1], core[2]);
        QVERIFY2(core[1] > 200, "blown core rendered dark");
        QVERIFY2(std::abs(core[0] - core[1]) < 20 && std::abs(core[2] - core[1]) < 20, "blown core tinted");
    }

    void largeClippedAreaNotBlack()
    {
        // A window onto a bright sky, clipped in every channel over a large
        // area, in a dim room (device capture 2026-10-31 rendered it black:
        // zero chroma noise in that brightness bin gave NaN weights).
        const int w = 512, h = 384;
        Scene scene;
        scene.width = w;
        scene.height = h;
        scene.rgb.resize(std::size_t(w) * h * 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const bool window = x > 64 && x < 320 && y > 32 && y < 224;
                float *p = &scene.rgb[(std::size_t(y) * w + x) * 3];
                p[0] = window ? 6.0f : 0.06f; p[1] = window ? 6.0f : 0.07f; p[2] = window ? 6.0f : 0.08f;
            }
        const Planes frame = toPlanes(mosaicFrame(scene, 3, 0, 0, 1.0, 1e-4, 1e-6, 9), 1.0);
        FinishOptions options = finishOptions(3);
        options.targetGain = 3.26;
        options.relativeNoise = 0.02;
        const RgbImage image = finish(frame, options, nullptr);
        double mean = 0;
        int n = 0;
        for (int y = 80; y < 180; ++y)
            for (int x = 120; x < 260; ++x, ++n)
                for (int c = 0; c < 3; ++c) mean += image.rgb[(std::size_t(y) * w + x) * 3 + c];
        mean /= 3.0 * n;
        qInfo("clipped window level %.0f", mean);
        QVERIFY2(mean > 200, "fully clipped window rendered dark");
    }

    void windowInDimRoom()
    {
        // A blown window in a dim room (device captures 2026-10-31): the
        // underexposed burst must come out as bright as a single frame at
        // the metered exposure, and the wall next to the window must not
        // darken (exposure fusion halo).
        const int w = 768, h = 576;
        Scene scene;
        scene.width = w;
        scene.height = h;
        scene.rgb.resize(std::size_t(w) * h * 3);
        auto inWindow = [](int x, int y) { return x > 96 && x < 288 && y > 64 && y < 208; };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float *p = &scene.rgb[(std::size_t(y) * w + x) * 3];
                const float wall = 0.05f + 0.01f * std::sin(x * 0.05f);
                for (int c = 0; c < 3; ++c) p[c] = inWindow(x, y) ? 8.0f : wall;
            }
        auto render = [&](double exposure, double targetGain) {
            const Planes frame = toPlanes(mosaicFrame(scene, 3, 0, 0, exposure, 1e-5, 1e-7, 11), 1.0);
            FinishOptions options = finishOptions(3);
            options.targetGain = targetGain;
            options.relativeNoise = 0.02;
            FinishStats stats;
            const RgbImage image = finish(frame, options, &stats);
            qInfo("exposure %.2f: short %.2f long %.2f", exposure, stats.shortGain, stats.longGain);
            return image;
        };
        auto mean = [&](const RgbImage &image, int x0, int y0, int x1, int y1) {
            double sum = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    for (int c = 0; c < 3; ++c) sum += image.rgb[(std::size_t(y) * w + x) * 3 + c];
            return sum / (3.0 * (x1 - x0) * (y1 - y0));
        };
        const RgbImage single = render(1.0, 1.0);
        const RgbImage burst = render(1.0 / 3.25, 3.25);
        const double singleFar = mean(single, 500, 350, 740, 550), burstFar = mean(burst, 500, 350, 740, 550);
        const double burstNear = mean(burst, 300, 64, 340, 208);
        qInfo("far wall: single %.1f, burst %.1f; burst wall next to the window %.1f", singleFar, burstFar, burstNear);
        QVERIFY2(burstFar > 0.85 * singleFar, "burst darker than a single frame");
        QVERIFY2(burstNear > 0.8 * burstFar, "dark halo next to the window");
    }

    void spatialDenoiseSingleFrame()
    {
        const Scene scene = detailScene(512, 384);
        const double s = 2e-3, o = 1e-4;
        const Planes clean = toPlanes(mosaicFrame(scene, 0, 0, 0, 0.5, 0, 0, 5), 1.0);
        const Planes noisy = toPlanes(mosaicFrame(scene, 0, 0, 0, 0.5, s, o, 6), 1.0);
        NoiseModel noise;
        QVERIFY(noiseFromProfile({s, o}, &noise));
        const Planes denoised = merge({&noisy}, {}, noise, MergeOptions(), nullptr);
        const double before = rmsError(noisy, clean, 16), after = rmsError(denoised, clean, 16);
        // The two-step setting used for high-ISO single frames.
        MergeOptions strong;
        strong.spatialStrength = 2.5;
        strong.spatialFloor = 0.5;
        strong.spatialPilot = 2.5;
        const double pilot = rmsError(merge({&noisy}, {}, noise, strong, nullptr), clean, 16);
        qInfo("single-frame spatial denoise rms %.5f -> %.5f (two-step %.5f)", before, after, pilot);
        QVERIFY(after < before * 0.8);
        QVERIFY2(pilot < after * 0.9, "two-step Wiener should beat the light single pass");
    }

    void finishKeepsHighlightsAndLiftsShadows()
    {
        const int w = 768, h = 512;
        const Scene scene = hdrScene(w, h);
        // Metered exposure puts the foreground mid tones at ~0.2; the sky (4x)
        // clips. The burst is captured 2 EV under so the sky survives.
        const double metered = 1.0, under = 0.25, s = 5e-4, o = 2e-5;
        const BayerFrame normal = mosaicFrame(scene, 0, 0, 0, metered, s, o, 11);
        const BayerFrame burst = mosaicFrame(scene, 0, 0, 0, metered * under, s, o, 12);
        NoiseModel noise;
        QVERIFY(noiseFromProfile({s, o}, &noise));

        FinishOptions options = finishOptions(0);
        options.targetGain = 1.0 / under;
        FinishStats stats;
        const Planes burstPlanes = toPlanes(burst, 1.0);
        const RgbImage hdr = finish(merge({&burstPlanes}, {}, noise, MergeOptions(), nullptr), options, &stats);
        dump(hdr, "hdr_scene_tonemapped");
        qInfo("hdr: short gain %.2f long gain %.2f highlight %.3f key %.4f",
              stats.shortGain, stats.longGain, stats.highlightLevel, stats.keyLevel);

        FinishOptions flat = finishOptions(0);
        flat.localToneMapping = 0;
        const RgbImage clipped = finish(toPlanes(normal, 1.0), flat, nullptr);
        dump(clipped, "hdr_scene_metered_single");

        auto meanRegion = [](const RgbImage &image, int x0, int y0, int x1, int y1, int c) {
            double sum = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) sum += image.rgb[(std::size_t(y) * image.width + x) * 3 + c];
            return sum / ((x1 - x0) * (y1 - y0));
        };
        auto stddevRegion = [](const RgbImage &image, int x0, int y0, int x1, int y1) {
            double sum = 0, sum2 = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const double v = image.rgb[(std::size_t(y) * image.width + x) * 3 + 1];
                    sum += v; sum2 += v * v;
                }
            const double n = (x1 - x0) * (y1 - y0);
            return std::sqrt(std::max(0.0, sum2 / n - (sum / n) * (sum / n)));
        };
        // Sky: the metered single frame is a flat white wall; the tone-mapped
        // burst keeps cloud structure below white.
        QVERIFY(meanRegion(clipped, 0, 0, w, h / 3, 1) > 240);
        QVERIFY(meanRegion(hdr, 0, 0, w, h / 3, 1) < 245);
        QVERIFY2(stddevRegion(hdr, 0, 0, w, h / 3) > 4 * stddevRegion(clipped, 0, 0, w, h / 3) + 4,
                 qPrintable(QStringLiteral("%1 vs %2").arg(stddevRegion(hdr, 0, 0, w, h / 3)).arg(stddevRegion(clipped, 0, 0, w, h / 3))));
        // Deep shadow at the left of the foreground is visible, not black.
        QVERIFY(meanRegion(hdr, 0, h * 6 / 10, w / 10, h * 7 / 10, 1) > 30);
        // Clipped sky renders neutral, not magenta.
        const double r = meanRegion(clipped, 0, 0, w, h / 3, 0), g = meanRegion(clipped, 0, 0, w, h / 3, 1),
                b = meanRegion(clipped, 0, 0, w, h / 3, 2);
        QVERIFY2(std::abs(r - g) < 8 && std::abs(b - g) < 8, qPrintable(QStringLiteral("%1 %2 %3").arg(r).arg(g).arg(b)));
    }

    void raisrBeatsBilinear()
    {
        // Filters trained on one synthetic scene restore another (different
        // seed and size) better than bilinear upscaling.
        auto tone = [](const Scene &scene) {
            Raisr::Image image;
            image.width = scene.width;
            image.height = scene.height;
            image.data.resize(std::size_t(scene.width) * scene.height);
            for (int y = 0; y < scene.height; ++y)
                for (int x = 0; x < scene.width; ++x)
                    image.data[std::size_t(y) * scene.width + x] = std::sqrt(std::max(0.0f, scene.at(x, y, 1)));
            return image;
        };
        Raisr::Trainer trainer(2.0);
        const Raisr::Image train = tone(blurred(detailScene(1024, 768), 1.0));
        trainer.calibrate({train});
        trainer.add(train, 1);
        const Raisr::Filters filters = trainer.solve();
        QVERIFY(filters.valid());
        QTemporaryDir dir;
        std::string error;
        const std::string path = QFile::encodeName(dir.filePath("raisr.bin")).toStdString();
        QVERIFY(Raisr::save(path, filters, &error));
        Raisr::Filters loaded;
        QVERIFY2(Raisr::load(path, &loaded, &error), error.c_str());
        QCOMPARE(loaded.weights, filters.weights);

        const Raisr::Image truth = tone(blurred(detailScene(800, 600), 1.0));
        const Raisr::Image low = Raisr::resize(truth, 400, 300);
        auto rms = [&](const Raisr::Image &image) {
            double sum = 0; long n = 0;
            for (int y = 8; y < truth.height - 8; ++y)
                for (int x = 8; x < truth.width - 8; ++x) {
                    const double d = image.data[std::size_t(y) * truth.width + x] - truth.data[std::size_t(y) * truth.width + x];
                    sum += d * d; ++n;
                }
            return std::sqrt(sum / n);
        };
        const double bilinear = rms(Raisr::resize(low, 800, 600));
        const double learned = rms(Raisr::upscale(low, 800, 600, loaded));
        qInfo("2x upscale error: bilinear %.5f, RAISR %.5f", bilinear, learned);
        QVERIFY(learned < 0.9 * bilinear);
    }

    void superResolutionZoom()
    {
        // 2x zoom: the centre half of the frame on a grid twice as fine.
        // Burst samples land between the sensor pixels, so the result is
        // closer to the scene than an upscaled crop.
        const int factor = 4;
        const Scene fine = blurred(detailScene(2048, 1536), 1.5);
        const double exposure = 0.3, s = 6.5e-5, o = 3e-7;
        const double shifts[][2] = {{0, 0}, {0.5, 0.25}, {1.25, -0.5}, {-0.75, 0.75}, {0.25, -1.25}, {1.5, 1.0},
                                    {-0.5, -0.75}, {0.75, 1.5}};
        const int count = 8;
        std::vector<BayerFrame> frames;
        for (int i = 0; i < count; ++i)
            frames.push_back(mosaicFromFine(fine, factor, shifts[i][0], shifts[i][1], exposure, s, o, 700 + i));
        const int width = frames[0].width, height = frames[0].height;
        ZoomRegion zoom;
        zoom.width = width / 2;
        zoom.height = height / 2;
        zoom.x = width / 4 & ~1;
        zoom.y = height / 4 & ~1;
        zoom.scale = 2.0;
        const int outWidth = zoom.outputWidth(), outHeight = zoom.outputHeight();
        // Truth: scene green over each output pixel (half a sensor pixel).
        std::vector<float> truth(std::size_t(outWidth) * outHeight);
        const int step = factor / 2;
        for (int y = 0; y < outHeight; ++y)
            for (int x = 0; x < outWidth; ++x) {
                double sum = 0;
                for (int j = 0; j < step; ++j)
                    for (int i = 0; i < step; ++i)
                        sum += fine.at(factor * zoom.x + step * x + i, factor * zoom.y + step * y + j, 1);
                truth[std::size_t(y) * outWidth + x] = float(sum / (step * step) * exposure);
            }
        std::vector<const BayerFrame *> pointers;
        for (const BayerFrame &f : frames) pointers.push_back(&f);
        const std::vector<double> gains(count, 1.0);
        const std::vector<float> reference = greenImage(frames[0], 1.0);
        std::vector<Alignment> alignments;
        for (int i = 1; i < count; ++i) alignments.push_back(align(reference, greenImage(frames[i], 1.0), width / 2, height / 2));
        NoiseModel noise;
        QVERIFY(noiseFromProfile({s, o}, &noise));
        const Planes merged = merge(pointers, gains, alignments, noise, MergeOptions(), nullptr);
        std::vector<float> mosaic(std::size_t(width) * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                mosaic[std::size_t(y) * width + x] = merged.phase[(y & 1) * 2 + (x & 1)][std::size_t(y / 2) * merged.width + x / 2];
        const std::vector<float> rgb = demosaic(mosaic, width, height, 0);
        Raisr::Image crop;
        crop.width = zoom.width;
        crop.height = zoom.height;
        for (int y = 0; y < zoom.height; ++y)
            for (int x = 0; x < zoom.width; ++x)
                crop.data.push_back(rgb[(std::size_t(zoom.y + y) * width + zoom.x + x) * 3 + 1]);
        const Raisr::Image upscaled = Raisr::resize(crop, outWidth, outHeight);
        const std::vector<float> green = superResolveGreen(pointers, gains, alignments, merged, noise, &zoom);
        QCOMPARE(int(green.size()), outWidth * outHeight);
        auto rms = [&](auto value) {
            double sum = 0; long n = 0;
            for (int y = 16; y < outHeight - 16; ++y)
                for (int x = 16; x < outWidth - 16; ++x) {
                    const double d = value(std::size_t(y) * outWidth + x) - truth[std::size_t(y) * outWidth + x];
                    if (std::isfinite(d)) { sum += d * d; ++n; }
                }
            return std::sqrt(sum / std::max(1L, n));
        };
        int holes = 0;
        for (float v : green) holes += !std::isfinite(v);
        const double upscaledError = rms([&](std::size_t i) { return upscaled.data[i]; });
        const double srError = rms([&](std::size_t i) { return std::isfinite(green[i]) ? green[i] : upscaled.data[i]; });
        qInfo("2x zoom green error vs truth: upscaled crop %.5f, super-resolved %.5f (%d holes of %d)",
              upscaledError, srError, holes, outWidth * outHeight);
        QVERIFY(srError < 0.9 * upscaledError);
        QVERIFY(holes < outWidth * outHeight / 50);

        // finish() renders just the zoomed crop at the output size.
        FinishOptions options = finishOptions(0);
        options.targetGain = 1.0 / exposure;
        options.zoom = zoom;
        std::vector<float> detail = green;
        options.detailGreen = &detail;
        const RgbImage image = finish(merged, options, nullptr);
        QCOMPARE(image.width, outWidth);
        QCOMPARE(image.height, outHeight);
        QCOMPARE(image.rgb.size(), std::size_t(outWidth) * outHeight * 3);
        double mean = 0;
        for (uint8_t v : image.rgb) mean += v;
        mean /= image.rgb.size();
        QVERIFY(mean > 40 && mean < 220);
    }

    // A camera that adds a constant to its tone-mapped luma when the exposure
    // rises (the Xperia 10 III's +0.155 for 2.3x): the curve recovers it, and
    // a frame covering too little of the range gives none.
    void yuvCameraCurve()
    {
        const int width = 640, height = 480;
        YuvFrame captured, metered, flat;
        captured.width = metered.width = flat.width = width;
        captured.height = metered.height = flat.height = height;
        const std::size_t size = std::size_t(width) * height * 3 / 2;
        captured.data.assign(size, 128);
        metered.data.assign(size, 128);
        flat.data.assign(size, 128);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const float v = 0.05f + 0.75f * float(x) / width;
                captured.data[std::size_t(y) * width + x] = uint8_t(v * 255 + 0.5f);
                metered.data[std::size_t(y) * width + x] = uint8_t(std::min(1.0f, v + 0.155f) * 255 + 0.5f);
                flat.data[std::size_t(y) * width + x] = 140;
            }
        const std::vector<float> curve = measureCameraCurve(captured, metered);
        QCOMPARE(int(curve.size()), 257);
        for (float x : {0.2f, 0.4f, 0.6f})
            QVERIFY2(std::abs(curve[int(x * 256)] - (x + 0.155f)) < 0.02f, qPrintable(QString::number(curve[int(x * 256)])));
        for (int i = 1; i <= 256; ++i) QVERIFY(curve[i] >= curve[i - 1]);
        QVERIFY(measureCameraCurve(flat, metered).empty());
    }

    void yuvToneRefinements()
    {
        // Front / ultra-wide: a hazy YUV burst (no real blacks) gets the same
        // black point as RAW, while its mid-tones keep their brightness.
        const int width = 640, height = 480;
        std::vector<YuvFrame> frames(4);
        for (int f = 0; f < 4; ++f) {
            std::mt19937 rng(50 + f);
            std::normal_distribution<double> noise(0.0, 1.5);
            YuvFrame &frame = frames[f];
            frame.width = width;
            frame.height = height;
            frame.data.assign(std::size_t(width) * height + 2 * std::size_t(width / 2) * (height / 2), 128);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const double v = 60 + 120.0 * x / width + 25 * ((x / 16 + y / 16) % 2) + noise(rng);
                    frame.data[std::size_t(y) * width + x] = uint8_t(std::max(0.0, std::min(255.0, v)));
                }
        }
        std::vector<const YuvFrame *> pointers;
        for (const YuvFrame &f : frames) pointers.push_back(&f);
        auto lumaStats = [&](const RgbImage &image, double fraction) {
            std::vector<float> l;
            for (std::size_t i = 0; i < image.rgb.size(); i += 3)
                l.push_back(0.299f * image.rgb[i] + 0.587f * image.rgb[i + 1] + 0.114f * image.rgb[i + 2]);
            const std::size_t k = std::min(l.size() - 1, std::size_t(fraction * l.size()));
            std::nth_element(l.begin(), l.begin() + k, l.end());
            return double(l[k]);
        };
        YuvOptions plain;
        YuvOptions refined;
        refined.blackPoint = refined.whitePoint = true;
        refined.clarity = 0.4;
        refined.saturation = 1.05;
        YuvStats stats;
        const RgbImage before = renderYuvBurst(pointers, plain, nullptr);
        const RgbImage after = renderYuvBurst(pointers, refined, &stats);
        qInfo("yuv tone: p0.5 %.0f -> %.0f, median %.0f -> %.0f, black point %.3f",
              lumaStats(before, 0.005), lumaStats(after, 0.005), lumaStats(before, 0.5), lumaStats(after, 0.5),
              stats.blackPoint);
        QVERIFY(stats.blackPoint > 0);
        QVERIFY(lumaStats(after, 0.005) < lumaStats(before, 0.005) - 20);
        QVERIFY(std::abs(lumaStats(after, 0.5) - lumaStats(before, 0.5)) < 0.08 * lumaStats(before, 0.5));
    }

    void yuvSkinKeepsSoftTexture()
    {
        // A selfie: two identical patches of textured skin, one of them a
        // detected face. Clarity and sharpening boost the face's texture
        // less; the other patch is rendered as without a face.
        const int width = 640, height = 480;
        auto inPatch = [](int x, int y) { return y >= 140 && y < 340 && ((x >= 80 && x < 240) || (x >= 400 && x < 560)); };
        std::mt19937 textureRng(7);
        std::normal_distribution<double> textureNoise(0.0, 9.0);
        std::vector<double> texture(std::size_t(width / 2) * (height / 2));
        for (double &t : texture) t = textureNoise(textureRng);
        std::vector<YuvFrame> frames(4);
        for (int f = 0; f < 4; ++f) {
            std::mt19937 rng(80 + f);
            std::normal_distribution<double> noise(0.0, 1.0);
            YuvFrame &frame = frames[f];
            frame.width = width;
            frame.height = height;
            frame.data.assign(std::size_t(width) * height + 2 * std::size_t(width / 2) * (height / 2), 128);
            uint8_t *u = &frame.data[std::size_t(width) * height];
            uint8_t *v = u + std::size_t(width / 2) * (height / 2);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    // The same texture in both patches (pores, stubble).
                    const int tx = (x >= 400 ? x - 320 : x) / 6, ty = y / 6;
                    const double base = inPatch(x, y) ? 150 + texture[std::size_t(ty) * (width / 2) + tx] : 90;
                    frame.data[std::size_t(y) * width + x] = uint8_t(std::max(0.0, std::min(255.0, base + noise(rng))));
                    if (inPatch(x, y) && x % 2 == 0 && y % 2 == 0) {
                        u[std::size_t(y / 2) * (width / 2) + x / 2] = 112; // skin: Cb < 0, Cr > 0
                        v[std::size_t(y / 2) * (width / 2) + x / 2] = 150;
                    }
                }
        }
        std::vector<const YuvFrame *> pointers;
        for (const YuvFrame &f : frames) pointers.push_back(&f);
        YuvOptions options;
        options.clarity = 0.4;
        const RgbImage plain = renderYuvBurst(pointers, options, nullptr);
        options.faces.push_back({{80.0f / width, 140.0f / height, 240.0f / width, 340.0f / height}});
        const RgbImage withFace = renderYuvBurst(pointers, options, nullptr);
        // Green channel's deviation from its 9x9 mean inside a patch.
        auto textureOf = [&](const RgbImage &image, int x0) {
            auto g = [&](int xx, int yy) { return double(image.rgb[(std::size_t(yy) * width + xx) * 3 + 1]); };
            double sum = 0;
            int n = 0;
            for (int y = 170; y < 310; ++y)
                for (int x = x0 + 30; x < x0 + 130; ++x, ++n) {
                    double mean = 0;
                    for (int dy = -4; dy <= 4; ++dy)
                        for (int dx = -4; dx <= 4; ++dx) mean += g(x + dx, y + dy);
                    const double d = g(x, y) - mean / 81;
                    sum += d * d;
                }
            return std::sqrt(sum / n);
        };
        const double facePlain = textureOf(plain, 80), faceSoft = textureOf(withFace, 80);
        const double otherPlain = textureOf(plain, 400), otherWithFace = textureOf(withFace, 400);
        qInfo("skin texture: face %.2f -> %.2f, other patch %.2f -> %.2f", facePlain, faceSoft, otherPlain, otherWithFace);
        QVERIFY(faceSoft < 0.9 * facePlain);
        QVERIFY(std::abs(otherWithFace - otherPlain) < 0.03 * otherPlain);
    }

    void yuvBurstReducesNoise()
    {
        // Front / ultra-wide cameras: display-referred 8-bit YUV frames with
        // the ISP's denoising off, shifted by hand shake. The merged render
        // must be much closer to the clean frame than one noisy frame.
        const int factor = 2;
        const Scene fine = blurred(detailScene(2048, 1536), 1.5);
        const int width = fine.width / factor, height = fine.height / factor;
        auto encode = [](double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055; };
        auto makeFrame = [&](double shiftX, double shiftY, double lumaSigma, double chromaSigma, unsigned seed) {
            std::mt19937 rng(seed);
            std::normal_distribution<double> gauss(0.0, 1.0);
            std::vector<float> yPlane(std::size_t(width) * height), uFull(yPlane.size()), vFull(yPlane.size());
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    double rgb[3] = {0, 0, 0};
                    for (int c = 0; c < 3; ++c) {
                        for (int j = 0; j < factor; ++j)
                            for (int i = 0; i < factor; ++i)
                                rgb[c] += fine.at(int(std::lround(factor * (x + shiftX))) + i,
                                                  int(std::lround(factor * (y + shiftY))) + j, c);
                        rgb[c] = encode(std::min(1.0, rgb[c] / (factor * factor) * 1.6));
                    }
                    const double luma = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
                    const std::size_t i = std::size_t(y) * width + x;
                    yPlane[i] = float(luma + lumaSigma * gauss(rng));
                    uFull[i] = float((rgb[2] - luma) / 1.772 + chromaSigma * gauss(rng));
                    vFull[i] = float((rgb[0] - luma) / 1.402 + chromaSigma * gauss(rng));
                }
            YuvFrame frame;
            frame.width = width;
            frame.height = height;
            const int cw = width / 2, ch = height / 2;
            frame.data.resize(std::size_t(width) * height + 2 * std::size_t(cw) * ch);
            auto quantise = [](double v) { return uint8_t(std::max(0, std::min(255, int(std::lround(v * 255))))); };
            for (std::size_t i = 0; i < yPlane.size(); ++i) frame.data[i] = quantise(yPlane[i]);
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x) {
                    double us = 0, vs = 0;
                    for (int j = 0; j < 2; ++j)
                        for (int i = 0; i < 2; ++i) {
                            us += uFull[std::size_t(2 * y + j) * width + 2 * x + i];
                            vs += vFull[std::size_t(2 * y + j) * width + 2 * x + i];
                        }
                    frame.data[std::size_t(width) * height + std::size_t(y) * cw + x] = quantise(us / 4 + 0.5);
                    frame.data[std::size_t(width) * height + std::size_t(cw) * ch + std::size_t(y) * cw + x] = quantise(vs / 4 + 0.5);
                }
            return frame;
        };
        const double shifts[][2] = {{0, 0}, {0.5, 0.5}, {1.5, -1.0}, {-1.0, 1.5}, {2.0, 0.5}, {-0.5, -1.5}};
        std::vector<YuvFrame> frames;
        for (int k = 0; k < 6; ++k) frames.push_back(makeFrame(shifts[k][0], shifts[k][1], 0.025, 0.015, 900 + k));
        const YuvFrame clean = makeFrame(0, 0, 0, 0, 1);
        YuvOptions options;
        options.sharpening = 0;
        auto render = [&](const std::vector<const YuvFrame *> &input) {
            YuvStats stats;
            const RgbImage image = renderYuvBurst(input, options, &stats);
            qInfo("yuv x%d: luma noise %.4f chroma noise %.4f severity %.2f rejection %.2f, %.0f/%.0f/%.0f ms",
                  int(input.size()), stats.lumaNoise, stats.chromaNoise, stats.severity, stats.rejection,
                  stats.stageMs[0], stats.stageMs[1], stats.stageMs[2]);
            return image;
        };
        const RgbImage truth = render({&clean});
        std::vector<const YuvFrame *> all;
        for (const YuvFrame &f : frames) all.push_back(&f);
        options.spatialDenoise = 0; // the single noisy frame as delivered
        const RgbImage single = render({&frames[0]});
        options.spatialDenoise = 1;
        const RgbImage merged = render(all);
        QCOMPARE(merged.width, width);
        QCOMPARE(merged.height, height);
        auto rms = [&](const RgbImage &image) {
            double sum = 0; long n = 0;
            for (int y = 24; y < height - 24; ++y)
                for (int x = 24; x < width - 24; ++x)
                    for (int c = 0; c < 3; ++c) {
                        const double d = double(image.rgb[(std::size_t(y) * width + x) * 3 + c])
                                - truth.rgb[(std::size_t(y) * width + x) * 3 + c];
                        sum += d * d; ++n;
                    }
            return std::sqrt(sum / n);
        };
        const double singleError = rms(single), mergedError = rms(merged);
        // The red ball keeps its colour.
        double ball[3] = {0, 0, 0}, ballTruth[3] = {0, 0, 0};
        int n = 0;
        for (int y = 460; y < 480; ++y)
            for (int x = 750; x < 770; ++x, ++n)
                for (int c = 0; c < 3; ++c) {
                    ball[c] += merged.rgb[(std::size_t(y) * width + x) * 3 + c];
                    ballTruth[c] += truth.rgb[(std::size_t(y) * width + x) * 3 + c];
                }
        qInfo("rgb error vs clean frame: single %.2f, merged %.2f; ball %.0f %.0f %.0f (clean %.0f %.0f %.0f)",
              singleError, mergedError, ball[0] / n, ball[1] / n, ball[2] / n,
              ballTruth[0] / n, ballTruth[1] / n, ballTruth[2] / n);
        QVERIFY2(mergedError < 0.6 * singleError, "merged YUV burst should remove most of the noise");
        for (int c = 0; c < 3; ++c) QVERIFY(std::abs(ball[c] - ballTruth[c]) / n < 8);
    }

    void yuvBurstFromFiles()
    {
        // The bridge's I420 frames + sidecars through renderBurst(): YUV path,
        // rotation applied, sidecar records the input.
        QTemporaryDir dir;
        const int width = 640, height = 480;
        QStringList sidecars;
        for (int k = 0; k < 3; ++k) {
            std::mt19937 rng(40 + k);
            std::normal_distribution<double> gauss(0.0, 6.0);
            QByteArray data(width * height + 2 * (width / 2) * (height / 2), Qt::Uninitialized);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    data[y * width + x] = char(std::max(0, std::min(255, int(40 + x / 4 + gauss(rng)))));
            for (int i = width * height; i < data.size(); ++i) data[i] = char(128 + (i % 7) - 3);
            const QString yuvPath = dir.filePath(QStringLiteral("f%1.warm.raw16").arg(k));
            QFile yuv(yuvPath);
            QVERIFY(yuv.open(QIODevice::WriteOnly));
            yuv.write(data);
            yuv.close();
            QJsonObject m;
            m.insert("format", "I420");
            m.insert("width", width);
            m.insert("height", height);
            m.insert("raw_path", yuvPath);
            m.insert("camera_id", "1");
            m.insert("bracket_rotation", 90);
            const QString sidecar = dir.filePath(QStringLiteral("f%1.warm.json").arg(k));
            QFile json(sidecar);
            QVERIFY(json.open(QIODevice::WriteOnly));
            json.write(QJsonDocument(m).toJson());
            json.close();
            sidecars << sidecar;
        }
        const QString target = dir.filePath("out.jpg");
        QString error;
        QVERIFY2(renderBurst(target, sidecars, 90, RenderSettings(), &error), qPrintable(error));
        const QImage image(target);
        QCOMPARE(image.width(), height); // rotated by 90
        QCOMPARE(image.height(), width);
        QFile json(dir.filePath("out.json"));
        QVERIFY(json.open(QIODevice::ReadOnly));
        const QJsonObject info = QJsonDocument::fromJson(json.readAll()).object().value("raw_pipeline").toObject();
        QCOMPARE(info.value("input").toString(), QStringLiteral("yuv"));
        QCOMPARE(info.value("frames").toInt(), 3);
    }

    void jpegBurstFromFiles()
    {
        // Fallback burst frames as JPEG (format "JPEG") take the YUV merge.
        QTemporaryDir dir;
        const int width = 640, height = 480;
        QStringList sidecars;
        for (int k = 0; k < 3; ++k) {
            std::mt19937 rng(70 + k);
            std::normal_distribution<double> gauss(0.0, 6.0);
            QImage image(width, height, QImage::Format_RGB888);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const int n = int(gauss(rng));
                    image.setPixel(x, y, qRgb(qBound(0, 200 + n, 255), qBound(0, 40 + x / 8 + n, 255), qBound(0, 60 + n, 255)));
                }
            const QString path = dir.filePath(QStringLiteral("f%1.warm.raw16").arg(k));
            QVERIFY(image.save(path, "JPG", 100));
            QJsonObject m;
            m.insert("format", "JPEG");
            m.insert("width", width);
            m.insert("height", height);
            m.insert("raw_path", path);
            m.insert("camera_id", "2");
            const QString sidecar = dir.filePath(QStringLiteral("f%1.warm.json").arg(k));
            QFile json(sidecar);
            QVERIFY(json.open(QIODevice::WriteOnly));
            json.write(QJsonDocument(m).toJson());
            json.close();
            sidecars << sidecar;
        }
        const QString target = dir.filePath("out.jpg");
        QString error;
        QVERIFY2(renderBurst(target, sidecars, 95, RenderSettings(), &error), qPrintable(error));
        const QImage image(target);
        QCOMPARE(image.size(), QSize(width, height));
        // Colour survives the JPEG -> YUV -> RGB round trip.
        const QRgb centre = image.pixel(width / 2, height / 2);
        QVERIFY2(qAbs(qRed(centre) - 200) < 12 && qAbs(qBlue(centre) - 60) < 12, "colour changed");
        QFile json(dir.filePath("out.json"));
        QVERIFY(json.open(QIODevice::ReadOnly));
        const QJsonObject info = QJsonDocument::fromJson(json.readAll()).object().value("raw_pipeline").toObject();
        QCOMPARE(info.value("input").toString(), QStringLiteral("jpeg"));
    }

    void yuvUnderexposedBurstKeepsHighlights()
    {
        // A lit face that clips at the metered exposure (selfie, device
        // 2026-10-31): the burst is captured 1.2 EV darker and fused back.
        // The face keeps its texture, the room its metered brightness.
        const int width = 512, height = 384;
        const Scene scene = blurred(detailScene(width, height), 0.7);
        auto encode = [](double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(std::max(0.0, v), 1 / 2.4) - 0.055; };
        auto linearAt = [&](int x, int y) {
            const double dx = x - 300, dy = y - 170;
            const double base = 0.6 * scene.at(x, y, 1);
            // Face: bright, gently textured skin.
            return dx * dx + dy * dy < 70 * 70 ? 1.3 + 0.5 * scene.at(x, y, 1) : base;
        };
        auto makeFrame = [&](double exposure, unsigned seed) {
            std::mt19937 rng(seed);
            std::normal_distribution<double> gauss(0.0, 0.01);
            YuvFrame frame;
            frame.width = width;
            frame.height = height;
            frame.data.assign(std::size_t(width) * height * 3 / 2, 128);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const double v = encode(std::min(1.0, linearAt(x, y) * exposure)) + gauss(rng);
                    frame.data[std::size_t(y) * width + x] = uint8_t(std::max(0, std::min(255, int(std::lround(v * 255)))));
                }
            return frame;
        };
        const double gain = std::pow(2.0, 1.2);
        std::vector<YuvFrame> frames;
        for (int k = 0; k < 4; ++k) frames.push_back(makeFrame(1.0 / gain, 300 + k));
        const YuvFrame metered = makeFrame(1.0, 400);
        YuvOptions options;
        options.sharpening = 0;
        const RgbImage single = renderYuvBurst({&metered}, options, nullptr);
        options.brightness = gain;
        std::vector<const YuvFrame *> pointers;
        for (const YuvFrame &f : frames) pointers.push_back(&f);
        const RgbImage fused = renderYuvBurst(pointers, options, nullptr);
        auto stats = [&](const RgbImage &image, int x0, int y0, int x1, int y1, double *stddev) {
            double sum = 0, sum2 = 0; long n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x, ++n) {
                    const double v = image.rgb[(std::size_t(y) * width + x) * 3 + 1];
                    sum += v; sum2 += v * v;
                }
            const double mean = sum / n;
            if (stddev) *stddev = std::sqrt(std::max(0.0, sum2 / n - mean * mean));
            return mean;
        };
        double singleFaceSd = 0, fusedFaceSd = 0;
        const double singleFace = stats(single, 270, 140, 330, 200, &singleFaceSd);
        const double fusedFace = stats(fused, 270, 140, 330, 200, &fusedFaceSd);
        const double singleRoom = stats(single, 20, 300, 200, 370, nullptr);
        const double fusedRoom = stats(fused, 20, 300, 200, 370, nullptr);
        qInfo("face: metered %.0f (sd %.1f), fused %.0f (sd %.1f); room: metered %.0f, fused %.0f",
              singleFace, singleFaceSd, fusedFace, fusedFaceSd, singleRoom, fusedRoom);
        QVERIFY2(fusedFace < 245, "face still blown");
        QVERIFY2(fusedFaceSd > 2 * singleFaceSd + 1, "face texture not recovered");
        QVERIFY2(std::abs(fusedRoom - singleRoom) < 0.15 * singleRoom, "room brightness changed");
    }

    void yuvWarmCastAndDarkShadows()
    {
        // Device selfies 2026-10-31: under warm lamps the ISP left a strong
        // orange cast, and darker-captured bursts had crushed shadows.
        const int width = 512, height = 384;
        const Scene scene = blurred(detailScene(width, height), 0.7);
        auto encode = [](double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(std::max(0.0, v), 1 / 2.4) - 0.055; };
        auto makeFrame = [&](double level, const double tint[3], double exposure, unsigned seed) {
            std::mt19937 rng(seed);
            std::normal_distribution<double> gauss(0.0, 0.006);
            YuvFrame frame;
            frame.width = width;
            frame.height = height;
            frame.data.resize(std::size_t(width) * height * 3 / 2);
            std::vector<double> cbs(std::size_t(width) * height), crs(cbs.size());
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    // Neutral (grey) surfaces with texture, lit by the tint.
                    const double v = level * (0.3 + 0.7 * scene.at(x, y, 1) / 0.6);
                    double rgb[3];
                    for (int k = 0; k < 3; ++k) rgb[k] = encode(std::min(1.0, v * tint[k] * exposure));
                    const double luma = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
                    const std::size_t i = std::size_t(y) * width + x;
                    frame.data[i] = uint8_t(std::max(0, std::min(255, int(std::lround((luma + gauss(rng)) * 255)))));
                    cbs[i] = (rgb[2] - luma) / 1.772;
                    crs[i] = (rgb[0] - luma) / 1.402;
                }
            const int cw = width / 2, ch = height / 2;
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x) {
                    const std::size_t i = std::size_t(2 * y) * width + 2 * x;
                    frame.data[std::size_t(width) * height + std::size_t(y) * cw + x] = uint8_t(std::lround(128 + 255 * cbs[i]));
                    frame.data[std::size_t(width) * height + std::size_t(cw) * ch + std::size_t(y) * cw + x] = uint8_t(std::lround(128 + 255 * crs[i]));
                }
            return frame;
        };
        auto meanRgb = [&](const RgbImage &image, double out[3]) {
            out[0] = out[1] = out[2] = 0;
            for (std::size_t i = 0; i < image.rgb.size(); i += 3)
                for (int k = 0; k < 3; ++k) out[k] += image.rgb[i + k];
            for (int k = 0; k < 3; ++k) out[k] /= image.rgb.size() / 3;
        };
        // Warm lamps: red x1.9, blue x0.5 in linear light (R/B 3.8).
        const double warm[3] = {1.9, 1.0, 0.5}, neutral[3] = {1.0, 1.0, 1.0};
        const YuvFrame warmFrame = makeFrame(0.25, warm, 1.0, 1);
        YuvOptions options;
        options.sharpening = 0;
        double warmRgb[3];
        meanRgb(renderYuvBurst({&warmFrame}, options, nullptr), warmRgb);
        const double inputRb = 1.9 / 0.5;
        auto linear = [](double v) { v /= 255; return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        const double outputRb = linear(warmRgb[0]) / linear(warmRgb[2]);
        qInfo("warm cast: linear R/B %.2f -> %.2f", inputRb, outputRb);
        QVERIFY2(outputRb < 0.75 * inputRb, "strong warm cast not reduced");
        QVERIFY2(outputRb > 1.5, "the lamp's warmth should partly stay");
        // A normal scene is left alone.
        const YuvFrame neutralFrame = makeFrame(0.25, neutral, 1.0, 2);
        double neutralRgb[3];
        meanRgb(renderYuvBurst({&neutralFrame}, options, nullptr), neutralRgb);
        QVERIFY(std::abs(neutralRgb[0] - neutralRgb[2]) < 3);

        // A dim room captured 1.2 EV darker: shadows come out lifted, not
        // crushed, compared with a frame at the metered exposure.
        const double gain = std::pow(2.0, 1.2);
        std::vector<YuvFrame> dark;
        for (int k = 0; k < 4; ++k) dark.push_back(makeFrame(0.05, neutral, 1.0 / gain, 10 + k));
        const YuvFrame meteredDark = makeFrame(0.05, neutral, 1.0, 20);
        std::vector<const YuvFrame *> pointers;
        for (const YuvFrame &f : dark) pointers.push_back(&f);
        options.brightness = 1.0;
        const RgbImage metered = renderYuvBurst({&meteredDark}, options, nullptr);
        options.brightness = gain;
        const RgbImage lifted = renderYuvBurst(pointers, options, nullptr);
        auto percentile = [](const RgbImage &image, double fraction) {
            std::vector<int> values;
            for (std::size_t i = 1; i < image.rgb.size(); i += 3) values.push_back(image.rgb[i]);
            std::nth_element(values.begin(), values.begin() + std::size_t(fraction * values.size()), values.end());
            return values[std::size_t(fraction * values.size())];
        };
        qInfo("dim room p5/p25/p50: metered %d/%d/%d, burst %d/%d/%d",
              percentile(metered, 0.05), percentile(metered, 0.25), percentile(metered, 0.5),
              percentile(lifted, 0.05), percentile(lifted, 0.25), percentile(lifted, 0.5));
        QVERIFY2(percentile(lifted, 0.25) > percentile(metered, 0.25), "shadows not lifted");
        QVERIFY2(percentile(lifted, 0.05) >= 6, "blacks crushed");
    }

    void detailRetention()
    {
        // RAWPIPELINE_DETAIL=1: share of the scene's fine detail (0.2-0.45
        // cycles/pixel) that survives each stage of a handheld burst
        // (diagnostic, not run by default).
        if (qgetenv("RAWPIPELINE_DETAIL").isEmpty()) QSKIP("set RAWPIPELINE_DETAIL=1 to run");
        const int factor = 4;
        const Scene fine = blurred(detailScene(2048, 1536), 1.2);
        const double exposure = 0.3, s = 6.5e-5, o = 3e-7;
        std::mt19937 rng(5);
        std::uniform_real_distribution<double> shake(-3.0, 3.0);
        std::vector<BayerFrame> frames;
        for (int i = 0; i < 6; ++i)
            frames.push_back(mosaicFromFine(fine, factor, i ? std::round(shake(rng) * 4) / 4 : 0,
                                            i ? std::round(shake(rng) * 4) / 4 : 0, exposure, s, o, 800 + i));
        const int width = frames[0].width, height = frames[0].height;
        std::vector<float> truth(std::size_t(width) * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                double sum = 0;
                for (int j = 0; j < factor; ++j)
                    for (int i = 0; i < factor; ++i) sum += fine.at(factor * x + i, factor * y + j, 1);
                truth[std::size_t(y) * width + x] = float(sum / (factor * factor) * exposure);
            }
        auto band = [&](const std::vector<float> &image) {
            // Fine-band energy of the difference-free image, 256x256 centre tile.
            const int n = 256, x0 = width / 2 - n / 2, y0 = height / 2 - n / 2;
            double energy = 0;
            for (int ky = -n / 2; ky < n / 2; ky += 2)
                for (int kx = -n / 2; kx < n / 2; kx += 2) {
                    const double r = std::sqrt(double(kx * kx + ky * ky)) / n;
                    if (r < 0.2 || r > 0.45) continue;
                    double re = 0, im = 0;
                    for (int y = 0; y < n; y += 2)
                        for (int x = 0; x < n; x += 2) {
                            const double v = image[std::size_t(y0 + y) * width + x0 + x];
                            const double a = -2 * M_PI * (double(kx) * x + double(ky) * y) / n;
                            re += v * std::cos(a);
                            im += v * std::sin(a);
                        }
                    energy += re * re + im * im;
                }
            return energy;
        };
        auto greenOf = [&](const Planes &planes) {
            std::vector<float> mosaic(std::size_t(width) * height);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    mosaic[std::size_t(y) * width + x] = planes.phase[(y & 1) * 2 + (x & 1)][std::size_t(y / 2) * planes.width + x / 2];
            const std::vector<float> rgb = demosaic(mosaic, width, height, 0);
            std::vector<float> g(mosaic.size());
            for (std::size_t i = 0; i < g.size(); ++i) g[i] = rgb[i * 3 + 1];
            return g;
        };
        const double reference = band(truth);
        std::vector<const BayerFrame *> pointers;
        for (const BayerFrame &f : frames) pointers.push_back(&f);
        const std::vector<double> gains(6, 1.0);
        NoiseModel noise;
        QVERIFY(noiseFromProfile({s, o}, &noise));
        const Planes single = toPlanes(frames[0], 1.0);
        const std::vector<float> ref = greenImage(frames[0], 1.0);
        std::vector<Alignment> alignments;
        for (int i = 1; i < 6; ++i) alignments.push_back(align(ref, greenImage(frames[i], 1.0), width / 2, height / 2));
        MergeOptions noSpatial;
        noSpatial.spatialStrength = 0;
        MergeOptions device; // as rendered on the phone at severity ~0.2
        device.spatialStrength = 1.3;
        device.spatialFloor = 0.1;
        device.spatialPilot = 0.5;
        const Planes mergedOnly = merge(pointers, gains, alignments, noise, noSpatial, nullptr);
        const Planes mergedDevice = merge(pointers, gains, alignments, noise, device, nullptr);
        std::vector<float> coverage;
        const std::vector<float> sr = superResolveGreen(pointers, gains, alignments, mergedDevice, noise, nullptr, &coverage);
        std::vector<float> srFilled = sr, dev = greenOf(mergedDevice);
        for (std::size_t i = 0; i < sr.size(); ++i) if (!std::isfinite(srFilled[i])) srFilled[i] = dev[i];
        double meanCoverage = 0;
        for (float c : coverage) meanCoverage += c;
        qInfo("fine detail kept vs scene: single frame %.2f, merge %.2f, merge+spatial denoise %.2f, super-resolved %.2f"
              " (coverage %.1f of 5 alternate frames)",
              band(greenOf(single)) / reference, band(greenOf(mergedOnly)) / reference,
              band(dev) / reference, band(srFilled) / reference, meanCoverage / coverage.size());
    }

    void noCheckerboardAtHardEdges()
    {
        // Leaves against a bright sky whose green clips, handheld with wind
        // moving the leaves on the right half in alternate frames. Neither the
        // demosaic (single frame) nor the super-resolved detail (burst) may
        // add a pixel-scale checkerboard ("dots" along branches) beyond the
        // scene's own share of that frequency.
        const int factor = 4, fineWidth = 2048, fineHeight = 1536;
        auto leaves = [&](float windShift) {
            Scene scene;
            scene.width = fineWidth;
            scene.height = fineHeight;
            scene.rgb.resize(std::size_t(fineWidth) * fineHeight * 3);
            for (int y = 0; y < fineHeight; ++y)
                for (int x = 0; x < fineWidth; ++x) {
                    const float u = x - (x > fineWidth / 2 ? windShift : 0.0f);
                    const float n = valueNoise(u / 23.0f, y / 23.0f, 3) * 0.6f + valueNoise(u / 9.0f, y / 9.0f, 4) * 0.4f;
                    // Thin diagonal branches plus leaf blobs.
                    const float branch = std::abs(std::fmod(u * 0.7f + y * 0.45f, 160.0f) - 80.0f);
                    const bool dark = n > 0.58f || branch < 5.0f;
                    float *p = &scene.rgb[(std::size_t(y) * fineWidth + x) * 3];
                    const float leaf = 0.04f + 0.08f * n;
                    p[0] = dark ? leaf * 0.9f : 1.25f;
                    p[1] = dark ? leaf * 1.3f : 1.3f;
                    p[2] = dark ? leaf * 0.5f : 1.3f;
                }
            return blurred(scene, 1.2);
        };
        const Scene still = leaves(0), windy = leaves(7);
        const double s = 6.5e-5, o = 3e-7;
        const double shifts[][2] = {{0, 0}, {0.5, 0.25}, {1.25, -0.5}, {-0.75, 0.75}, {0.25, -1.25}, {1.5, 1.0}};
        const int count = 6;
        std::vector<BayerFrame> frames;
        for (int i = 0; i < count; ++i)
            frames.push_back(mosaicFromFine(i % 2 ? windy : still, factor, shifts[i][0], shifts[i][1], 1.0, s, o, 900 + i));
        const int width = frames[0].width, height = frames[0].height;
        std::vector<const BayerFrame *> pointers;
        for (const BayerFrame &f : frames) pointers.push_back(&f);
        const std::vector<double> gains(count, 1.0);
        const std::vector<float> reference = greenImage(frames[0], 1.0);
        std::vector<Alignment> alignments;
        for (int i = 1; i < count; ++i) alignments.push_back(align(reference, greenImage(frames[i], 1.0), width / 2, height / 2));
        NoiseModel noise;
        QVERIFY(noiseFromProfile({s, o}, &noise));
        const Planes merged = merge(pointers, gains, alignments, noise, MergeOptions(), nullptr);
        FinishOptions options;
        options.cfa = 0;
        options.wbGains = {{2.0, 1.0, 1.0, 1.0 / 0.7}};
        options.targetGain = 1.0;
        options.relativeNoise = 0.01;
        // Diagonal Nyquist energy of the output luma: (a - b - c + d) / 4
        // over each 2x2 cell, mean absolute value.
        auto checker = [&](const RgbImage &image) {
            double sum = 0;
            long n = 0;
            auto y = [&](int xx, int yy) {
                const uint8_t *p = &image.rgb[(std::size_t(yy) * image.width + xx) * 3];
                return 0.25 * p[0] + 0.6 * p[1] + 0.15 * p[2];
            };
            for (int yy = 16; yy < image.height - 16; ++yy)
                for (int xx = 16; xx < image.width - 16; ++xx, ++n)
                    sum += std::abs(y(xx, yy) - y(xx + 1, yy) - y(xx, yy + 1) + y(xx + 1, yy + 1)) / 4;
            return sum / n;
        };
        // The scene's own share: a clean, noise-free capture of the same
        // view with every channel sampled at every pixel.
        RgbImage ideal;
        ideal.width = width;
        ideal.height = height;
        ideal.rgb.resize(std::size_t(width) * height * 3);
        {
            for (int yy = 0; yy < height; ++yy)
                for (int xx = 0; xx < width; ++xx) {
                    double rgb[3] = {0, 0, 0};
                    for (int c = 0; c < 3; ++c) {
                        for (int j = 0; j < factor; ++j)
                            for (int i = 0; i < factor; ++i) rgb[c] += still.at(factor * xx + i, factor * yy + j, c);
                        rgb[c] /= factor * factor;
                    }
                    uint8_t *p = &ideal.rgb[(std::size_t(yy) * width + xx) * 3];
                    for (int c = 0; c < 3; ++c)
                        p[c] = uint8_t(std::lround(255 * std::pow(std::min(1.0, rgb[c]), 1 / 2.2)));
                }
        }
        const RgbImage single = finish(toPlanes(frames[0], 1.0), options, nullptr);
        const RgbImage plain = finish(merged, options, nullptr);
        std::vector<float> coverage;
        std::vector<float> detail = superResolveGreen(pointers, gains, alignments, merged, noise, nullptr, &coverage);
        FinishOptions withDetail = options;
        withDetail.detailGreen = &detail;
        withDetail.detailStrength = 0.9;
        withDetail.detailCoverage = &coverage;
        const RgbImage superResolved = finish(merged, withDetail, nullptr);
        dump(single, "leaves_single");
        dump(plain, "leaves_merged");
        dump(superResolved, "leaves_super_resolved");
        const double scene = checker(ideal);
        qInfo("checkerboard: ideal %.3f, single frame %.3f, merged %.3f, super-resolved %.3f",
              scene, checker(single), checker(plain), checker(superResolved));
        // Unclamped MHC measured 1.53x the scene here.
        QVERIFY(checker(single) < 1.3 * scene);
        QVERIFY(checker(plain) < 1.3 * scene);
        QVERIFY(checker(superResolved) <= 1.02 * checker(plain));
    }

    void sharpnessSeesShakeThroughNoise()
    {
        // A noisy, mostly flat frame (a wall) with one detailed area: the
        // shaken copy must score lower, although noise dominates the frame.
        const int width = 1024, height = 768;
        std::mt19937 random(17);
        std::normal_distribution<float> noise(0.0f, 0.03f);
        std::vector<float> sharp(std::size_t(width) * height), shaken(sharp.size());
        auto detail = [](int x, int y) {
            return (x / 3 + y / 5) % 2 ? 0.45f : 0.25f;
        };
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const bool inDetail = x >= 400 && x < 600 && y >= 300 && y < 460;
                float blurred = 0;
                for (int k = -3; k <= 3; ++k) blurred += inDetail ? detail(x + k, y) : 0.3f;
                const std::size_t i = std::size_t(y) * width + x;
                sharp[i] = (inDetail ? detail(x, y) : 0.3f) + noise(random);
                shaken[i] = blurred / 7 + noise(random); // 7 px horizontal shake
            }
        const double a = sharpness(sharp, width, height), b = sharpness(shaken, width, height);
        qInfo("sharpness: sharp %.4f, shaken %.4f", a, b);
        QVERIFY(a > 1.1 * b);
    }

    void facesStayInNaturalRange()
    {
        // A face lit by a lamp in a dark room (the lift for the dark room
        // blew it out) and a backlit face against a bright sky (left dark):
        // with the face known, both land in a natural brightness range.
        const int w = 512, h = 384;
        auto render = [&](float background, float face, bool withFace, double targetGain) {
            Scene scene;
            scene.width = w;
            scene.height = h;
            scene.rgb.resize(std::size_t(w) * h * 3);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const bool inFace = x >= 200 && x < 312 && y >= 120 && y < 260;
                    const float v = inFace ? face : background;
                    float *p = &scene.rgb[(std::size_t(y) * w + x) * 3];
                    p[0] = v * 1.1f; p[1] = v; p[2] = v * 0.85f; // skin-ish warm grey
                }
            const Planes frame = toPlanes(mosaicFrame(scene, 3, 0, 0, 1.0, 2e-5, 1e-7, 21), 1.0);
            FinishOptions options = finishOptions(3);
            options.targetGain = targetGain;
            if (withFace) options.faces.push_back({{200.0f / w, 120.0f / h, 312.0f / w, 260.0f / h}});
            const RgbImage image = finish(frame, options, nullptr);
            double sum = 0;
            int n = 0;
            for (int y = 160; y < 220; ++y)
                for (int x = 230; x < 280; ++x, ++n) sum += image.rgb[(std::size_t(y) * w + x) * 3 + 1];
            return sum / n;
        };
        // The lamp-lit face is the brightest thing in the frame: the
        // highlight protection alone would map it close to white.
        const double lampWithout = render(0.01f, 0.25f, false, 3.2), lampWith = render(0.01f, 0.25f, true, 3.2);
        const double backlitWithout = render(0.8f, 0.012f, false, 1.0), backlitWith = render(0.8f, 0.012f, true, 1.0);
        qInfo("face (8-bit green): lamp-lit %.0f -> %.0f with face, backlit %.0f -> %.0f with face",
              lampWithout, lampWith, backlitWithout, backlitWith);
        QVERIFY(lampWith < lampWithout - 5 && lampWith <= 200);
        QVERIFY(backlitWith > backlitWithout + 10 && backlitWith >= 75); // 3 stops above the sky
    }

    void sunlitFaceKeepsColour()
    {
        // A small face half in sun in a dark forest (the fusion pyramid lets
        // the shadow lift spill onto it): its lit side must not clip, and its
        // skin is boosted less than the same colour elsewhere.
        const int w = 512, h = 384;
        Scene scene;
        scene.width = w;
        scene.height = h;
        scene.rgb.resize(std::size_t(w) * h * 3);
        auto inFace = [](int x, int y) { return x >= 230 && x < 290 && y >= 150 && y < 230; };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float v = y < 60 ? 0.3f : 0.012f; // sky, forest
                if (inFace(x, y)) v = x < 260 ? 0.2f : 0.04f;
                if (x >= 60 && x < 120 && y >= 250 && y < 330) v = 0.06f; // skin-coloured patch, no face
                float *p = &scene.rgb[(std::size_t(y) * w + x) * 3];
                const bool warm = inFace(x, y) || (x >= 60 && x < 120 && y >= 250);
                p[0] = v * (warm ? 1.5f : 1.0f); p[1] = v; p[2] = v * (warm ? 0.75f : 1.0f);
            }
        const Planes frame = toPlanes(mosaicFrame(scene, 3, 0, 0, 1.0, 2e-5, 1e-7, 23), 1.0);
        auto render = [&](bool withFace, FinishStats *stats, bool fusion = true) {
            FinishOptions options = finishOptions(3);
            options.targetGain = 3.0;
            if (!fusion) options.localToneMapping = 0;
            if (withFace) options.faces.push_back({{230.0f / w, 150.0f / h, 290.0f / w, 230.0f / h}});
            return finish(frame, options, stats);
        };
        FinishStats plainStats, faceStats;
        const RgbImage plain = render(false, &plainStats), withFace = render(true, &faceStats);
        auto mean = [&](const RgbImage &image, int x0, int y0, int x1, int y1, int k) {
            double sum = 0;
            int n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x, ++n) sum += image.rgb[(std::size_t(y) * w + x) * 3 + k];
            return sum / n;
        };
        const double litPlain = mean(plain, 238, 170, 256, 210, 0), litFace = mean(withFace, 238, 170, 256, 210, 0);
        const double shadeFace = mean(withFace, 268, 170, 284, 210, 1);
        // Skin chroma (R - B) in the face's shaded half, at equal brightness
        // (no fusion): the face gets less of the boost; the patch outside
        // keeps all of it.
        const RgbImage flatPlain = render(false, nullptr, false), flatFace = render(true, nullptr, false);
        auto chroma = [&](const RgbImage &image, int x0, int y0, int x1, int y1) {
            return mean(image, x0, y0, x1, y1, 0) - mean(image, x0, y0, x1, y1, 2);
        };
        const double skinFace = chroma(flatFace, 268, 170, 284, 210) / chroma(flatPlain, 268, 170, 284, 210);
        const double skinPatch = chroma(flatFace, 70, 260, 110, 320) / chroma(flatPlain, 70, 260, 110, 320);
        qInfo("sunlit face red: %.0f -> %.0f with face (gains %.2f/%.2f -> %.2f/%.2f); shade green %.0f; "
              "skin chroma x%.2f in face, x%.2f outside",
              litPlain, litFace, plainStats.shortGain, plainStats.longGain, faceStats.shortGain, faceStats.longGain,
              shadeFace, skinFace, skinPatch);
        QVERIFY(litPlain >= 250 && litFace < 245);
        QVERIFY(shadeFace > 40);
        QVERIFY(skinFace < 0.9 && std::abs(skinPatch - 1.0) < 0.01);
    }

    void toneEndsAddNoGrain()
    {
        // An indoor wall whose brightest tones stop short: the white-point
        // stretch brightens it, but its noise must not grow with the slope
        // (it moves the smooth layer only).
        const int w = 512, h = 384;
        Scene scene;
        scene.width = w;
        scene.height = h;
        scene.rgb.resize(std::size_t(w) * h * 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c) scene.rgb[(std::size_t(y) * w + x) * 3 + c] = 0.12f + 0.10f * x / w;
        const Planes frame = toPlanes(mosaicFrame(scene, 3, 0, 0, 1.0, 4e-4, 2e-6, 31), 1.0);
        FinishOptions plain = finishOptions(3);
        plain.targetGain = 2.0;
        plain.localToneMapping = 0;
        plain.sharpening = 0;
        FinishOptions stretched = plain;
        stretched.blackPoint = stretched.whitePoint = true;
        FinishStats stats;
        const RgbImage a = finish(frame, plain, nullptr), b = finish(frame, stretched, &stats);
        auto grain = [&](const RgbImage &image) {
            double sum = 0, sum2 = 0;
            long n = 0;
            for (int y = 16; y < h - 16; ++y)
                for (int x = 16; x < w - 16; ++x, ++n) {
                    auto g = [&](int xx, int yy) { return double(image.rgb[(std::size_t(yy) * w + xx) * 3 + 1]); };
                    const double l = 4 * g(x, y) - g(x - 1, y) - g(x + 1, y) - g(x, y - 1) - g(x, y + 1);
                    sum += l; sum2 += l * l;
                }
            return std::sqrt(sum2 / n - (sum / n) * (sum / n));
        };
        qInfo("tone ends: white scale %.2f, grain %.2f -> %.2f", stats.whiteScale, grain(a), grain(b));
        QVERIFY(stats.whiteScale > 1.05);
        QVERIFY(grain(b) <= 1.1 * grain(a));
    }

    void superResolutionAddsNoGrain()
    {
        // A flat wall in a noisy indoor burst (ISO ~1700 on the phone): the
        // super-resolved estimate has fewer samples per pixel than the merge,
        // so swapping its fine detail in unfiltered roughly doubled the grain
        // on a real capture. The swap may not add noise where there is no
        // detail to gain.
        const int factor = 4;
        Scene wall;
        wall.width = 1024;
        wall.height = 768;
        wall.rgb.assign(std::size_t(wall.width) * wall.height * 3, 0.0f);
        for (std::size_t i = 0; i < wall.rgb.size(); i += 3) {
            wall.rgb[i] = 0.36f; wall.rgb[i + 1] = 0.42f; wall.rgb[i + 2] = 0.40f;
        }
        const double s = 1.2e-3, o = 2e-5;
        const double shifts[][2] = {{0, 0}, {0.5, 0.25}, {1.25, -0.5}, {-0.75, 0.75}, {0.25, -1.25}, {1.5, 1.0},
                                    {-0.5, -0.75}, {0.75, 1.5}};
        const int count = 8;
        std::vector<BayerFrame> frames;
        for (int i = 0; i < count; ++i)
            frames.push_back(mosaicFromFine(wall, factor, shifts[i][0], shifts[i][1], 0.5, s, o, 1100 + i));
        const int width = frames[0].width, height = frames[0].height;
        std::vector<const BayerFrame *> pointers;
        for (const BayerFrame &f : frames) pointers.push_back(&f);
        const std::vector<double> gains(count, 1.0);
        const std::vector<float> reference = greenImage(frames[0], 1.0);
        std::vector<Alignment> alignments;
        for (int i = 1; i < count; ++i) alignments.push_back(align(reference, greenImage(frames[i], 1.0), width / 2, height / 2));
        NoiseModel noise;
        QVERIFY(noiseFromProfile({s, o}, &noise));
        const Planes merged = merge(pointers, gains, alignments, noise, MergeOptions(), nullptr);
        FinishOptions options;
        options.cfa = 0;
        options.wbGains = {{2.0, 1.0, 1.0, 1.0 / 0.7}};
        options.targetGain = 2.0;
        options.relativeNoise = 0.03;
        auto grain = [&](const RgbImage &image) {
            double sum = 0, sum2 = 0;
            long n = 0;
            for (int y = 16; y < image.height - 16; ++y)
                for (int x = 16; x < image.width - 16; ++x, ++n) {
                    auto g = [&](int xx, int yy) { return double(image.rgb[(std::size_t(yy) * image.width + xx) * 3 + 1]); };
                    const double l = 4 * g(x, y) - g(x - 1, y) - g(x + 1, y) - g(x, y - 1) - g(x, y + 1);
                    sum += l;
                    sum2 += l * l;
                }
            return std::sqrt(std::max(0.0, sum2 / n - (sum / n) * (sum / n)));
        };
        const RgbImage plain = finish(merged, options, nullptr);
        std::vector<float> coverage;
        std::vector<float> detail = superResolveGreen(pointers, gains, alignments, merged, noise, nullptr, &coverage);
        FinishOptions withDetail = options;
        withDetail.detailGreen = &detail;
        withDetail.detailCoverage = &coverage;
        withDetail.detailStrength = 0.7;
        const RgbImage superResolved = finish(merged, withDetail, nullptr);
        qInfo("flat wall grain: merged %.2f, super-resolved %.2f", grain(plain), grain(superResolved));
        QVERIFY(grain(superResolved) <= 1.05 * grain(plain));
    }

    void smallHighlightInDarkScene()
    {
        // Night room: dim textured background, a lamp-lit face that is NOT
        // clipped in the RAW. Local tone mapping must not blow it out.
        const int w = 768, h = 1024;
        Scene scene;
        scene.width = w;
        scene.height = h;
        scene.rgb.resize(std::size_t(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float *p = &scene.rgb[(std::size_t(y) * w + x) * 3];
                const float t = 0.6f + 0.4f * valueNoise(x / 24.0f, y / 24.0f, 3);
                p[0] = 0.025f * t; p[1] = 0.02f * t; p[2] = 0.015f * t;
                const float dx = (x - w / 2) / 110.0f, dy = (y - h * 0.4f) / 140.0f;
                if (dx * dx + dy * dy < 1) {
                    // Lamp-lit: bright centre falling off towards the edges.
                    const float skin = (0.85f + 0.15f * valueNoise(x / 6.0f, y / 6.0f, 9))
                            * (1.0f - 0.6f * (dx * dx + dy * dy));
                    p[0] = 0.55f * skin; p[1] = 0.38f * skin; p[2] = 0.30f * skin;
                }
            }
        }
        const double s = 3e-3, o = 2e-4;
        const Planes frame = toPlanes(mosaicFrame(scene, 0, 0, 0, 1.0, s, o, 77), 1.0);
        NoiseModel noise;
        QVERIFY(noiseFromProfile({s, o}, &noise));
        const Planes denoised = merge({&frame}, {}, noise, MergeOptions(), nullptr);
        FinishOptions global = finishOptions(0);
        global.localToneMapping = 0;
        FinishOptions local = finishOptions(0);
        FinishStats stats;
        const RgbImage a = finish(denoised, global, nullptr), b = finish(denoised, local, &stats);
        dump(a, "night_face_global");
        dump(b, "night_face_local");
        auto face = [&](const RgbImage &image, int c) {
            double sum = 0; int n = 0;
            for (int y = int(h * 0.4) - 40; y < int(h * 0.4) + 40; ++y)
                for (int x = w / 2 - 40; x < w / 2 + 40; ++x) { sum += image.rgb[(std::size_t(y) * w + x) * 3 + c]; ++n; }
            return sum / n;
        };
        qInfo("night face: global R %.0f G %.0f, local R %.0f G %.0f (gains %.2f/%.2f key %.4f)",
              face(a, 0), face(a, 1), face(b, 0), face(b, 1), stats.shortGain, stats.longGain, stats.keyLevel);
        // The face must keep tonal separation from white.
        QVERIFY2(face(b, 0) < 245 && face(b, 1) < 235, "face blown out by local tone mapping");

        // Overexposed so red and green clip on the face while blue survives
        // (what the sensor did in a real night capture): the rebuilt face
        // keeps skin colour and texture instead of a flat white patch.
        const Planes hot = toPlanes(mosaicFrame(scene, 0, 0, 0, 4.0, s, o, 78), 1.0);
        const RgbImage rebuilt = finish(merge({&hot}, {}, noise, MergeOptions(), nullptr), local, nullptr);
        dump(rebuilt, "night_face_clipped_rebuilt");
        auto spread = [&](const RgbImage &image, int c) {
            double sum = 0, sum2 = 0; int n = 0;
            for (int y = int(h * 0.4) - 40; y < int(h * 0.4) + 40; ++y)
                for (int x = w / 2 - 40; x < w / 2 + 40; ++x) {
                    const double v = image.rgb[(std::size_t(y) * w + x) * 3 + c];
                    sum += v; sum2 += v * v; ++n;
                }
            return std::sqrt(std::max(0.0, sum2 / n - (sum / n) * (sum / n)));
        };
        qInfo("clipped face rebuilt: R %.0f G %.0f B %.0f, G spread %.1f",
              face(rebuilt, 0), face(rebuilt, 1), face(rebuilt, 2), spread(rebuilt, 1));
        QVERIFY2(face(rebuilt, 0) - face(rebuilt, 2) > 25, "rebuilt face lost its skin colour");
        QVERIFY2(spread(rebuilt, 1) > 3, "rebuilt face has no texture");
    }

    void endToEndBurstVisual()
    {
        // Low light: a 4x underexposed noisy burst of a real photo, with hand shake.
        const Scene scene = detailScene(1024, 768);
        QVERIFY(scene.width);
        const double s = 3e-3, o = 3e-4, exposure = 0.18;
        const int count = 8;
        std::vector<Planes> frames;
        for (int i = 0; i < count; ++i)
            frames.push_back(toPlanes(mosaicFrame(scene, 0, i * 6 - 20, (i % 3) * 8 - 8, exposure, s, o, 200 + i), 1.0));
        QElapsedTimer timer;
        timer.start();
        const std::vector<float> refGray = grayImage(frames[0]);
        std::vector<Alignment> alignments;
        for (int i = 1; i < count; ++i)
            alignments.push_back(align(refGray, grayImage(frames[i]), frames[0].width, frames[0].height));
        const qint64 alignMs = timer.restart();
        NoiseModel noise = estimateNoise(frames[0]);
        std::vector<const Planes *> pointers;
        for (const Planes &f : frames) pointers.push_back(&f);
        MergeStats mergeStats;
        const Planes merged = merge(pointers, alignments, noise, MergeOptions(), &mergeStats);
        const qint64 mergeMs = timer.restart();
        FinishOptions options = finishOptions(0);
        options.targetGain = 1.0 / exposure;
        options.relativeNoise = std::sqrt(noise.meanVariance(exposure * 0.18) / count) / (exposure * 0.18);
        const RgbImage image = finish(merged, options, nullptr);
        const qint64 finishMs = timer.elapsed();
        qInfo("1024x768 x%d: align %lld ms, merge %lld ms, finish %lld ms, rejection %.3f, relative noise %.3f",
              count, alignMs, mergeMs, finishMs, mergeStats.rejection, options.relativeNoise);
        dump(image, "lowlight_burst");
        FinishOptions single = options;
        single.relativeNoise *= std::sqrt(double(count));
        dump(finish(merge({&frames[0]}, {}, noise, MergeOptions(), nullptr), single, nullptr), "lowlight_single_enhanced");
        // What the current renderer would produce: no denoise, no tone mapping.
        FinishOptions legacy = finishOptions(0);
        legacy.localToneMapping = 0;
        legacy.sharpening = 0;
        legacy.chromaDenoise = 0;
        legacy.contrast = 0;
        legacy.saturation = 1;
        legacy.targetGain = 1.0 / exposure;
        Planes brightened = frames[0];
        brightened.clip = float(1.0 / exposure);
        for (auto &plane : brightened.phase) for (float &v : plane) v /= float(exposure);
        dump(finish(brightened, legacy, nullptr), "lowlight_single_plain");
        const Planes clean = toPlanes(mosaicFrame(scene, 0, -20, -8, exposure, 0, 0, 1), 1.0);
        FinishOptions cleanOptions = finishOptions(0);
        cleanOptions.targetGain = 1.0 / exposure;
        dump(finish(clean, cleanOptions, nullptr), "lowlight_ground_truth");
        QCOMPARE(image.width, 1024);
    }
};

QTEST_GUILESS_MAIN(RawPipelineTest)
#include "tst_rawpipeline.moc"
