// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "raisr.h"
#include "rawpipelinemath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>

namespace Raisr {

namespace {

const double Pi = 3.14159265358979323846;
const char Magic[8] = {'R', 'A', 'I', 'S', 'R', 'F', '1', '\0'};

inline int clampIndex(int i, int n) { return i < 0 ? 0 : (i >= n ? n - 1 : i); }

// Separable Gaussian, sigma 1.5 (radius 4): the 9x9 neighbourhood the
// structure tensor describes.
std::vector<float> blurWindow(const std::vector<float> &source, int width, int height)
{
    const int radius = 4;
    float kernel[2 * radius + 1], sum = 0;
    for (int i = -radius; i <= radius; ++i) sum += kernel[i + radius] = float(std::exp(-i * i / (2 * 1.5 * 1.5)));
    for (float &k : kernel) k /= sum;
    std::vector<float> rows(source.size()), out(source.size());
    RawPipeline::parallelFor(0, height, [&](int y) {
        const float *in = &source[std::size_t(y) * width];
        for (int x = 0; x < width; ++x) {
            float v = 0;
            for (int i = -radius; i <= radius; ++i) v += kernel[i + radius] * in[clampIndex(x + i, width)];
            rows[std::size_t(y) * width + x] = v;
        }
    });
    RawPipeline::parallelFor(0, height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            float v = 0;
            for (int i = -radius; i <= radius; ++i)
                v += kernel[i + radius] * rows[std::size_t(clampIndex(y + i, height)) * width + x];
            out[std::size_t(y) * width + x] = v;
        }
    });
    return out;
}

inline int quantise(float value, const float *thresholds, int count)
{
    int bucket = 0;
    while (bucket < count && value >= thresholds[bucket]) ++bucket;
    return bucket;
}

// Local structure: dominant gradient angle in [0, pi), strength and
// coherence of the Gaussian-weighted structure tensor.
struct Structure {
    std::vector<float> angle, strength, coherence;
};

Structure structure(const Image &image)
{
    const int width = image.width, height = image.height;
    const std::size_t pixels = std::size_t(width) * height;
    std::vector<float> xx(pixels), xy(pixels), yy(pixels);
    RawPipeline::parallelFor(0, height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const float gx = 0.5f * (image.at(x + 1, y) - image.at(x - 1, y));
            const float gy = 0.5f * (image.at(x, y + 1) - image.at(x, y - 1));
            const std::size_t i = std::size_t(y) * width + x;
            xx[i] = gx * gx;
            xy[i] = gx * gy;
            yy[i] = gy * gy;
        }
    });
    xx = blurWindow(xx, width, height);
    xy = blurWindow(xy, width, height);
    yy = blurWindow(yy, width, height);
    Structure s;
    s.angle.resize(pixels);
    s.strength.resize(pixels);
    s.coherence.resize(pixels);
    RawPipeline::parallelFor(0, height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = std::size_t(y) * width + x;
            const double a = xx[i], b = xy[i], d = yy[i];
            const double root = std::sqrt((a - d) * (a - d) + 4 * b * b);
            const double l1 = std::max(0.0, 0.5 * (a + d + root)), l2 = std::max(0.0, 0.5 * (a + d - root));
            double theta = 0.5 * std::atan2(2 * b, a - d);
            if (theta < 0) theta += Pi;
            const double s1 = std::sqrt(l1), s2 = std::sqrt(l2);
            s.angle[i] = float(theta);
            s.strength[i] = float(s1);
            s.coherence[i] = float(s1 + s2 > 1e-12 ? (s1 - s2) / (s1 + s2) : 0.0);
        }
    });
    return s;
}

inline int bucketOf(const Structure &s, std::size_t i, const Filters &filters)
{
    const int angle = std::min(Angles - 1, int(s.angle[i] * (Angles / Pi)));
    const int strength = quantise(s.strength[i], filters.strengthThresholds.data(), Strengths - 1);
    const int coherence = quantise(s.coherence[i], filters.coherenceThresholds.data(), Coherences - 1);
    return (angle * Strengths + strength) * Coherences + coherence;
}

inline void patchAt(const Image &image, int x, int y, double *patch)
{
    const int r = Patch / 2;
    int k = 0;
    if (x >= r && y >= r && x + r < image.width && y + r < image.height) {
        for (int j = -r; j <= r; ++j) {
            const float *row = &image.data[std::size_t(y + j) * image.width + x - r];
            for (int i = 0; i < Patch; ++i) patch[k++] = row[i];
        }
        return;
    }
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i) patch[k++] = image.at(x + i, y + j);
}

// Cholesky solve of the symmetric positive definite system A w = b (n x n,
// row-major, overwritten). False if A is not positive definite.
bool choleskySolve(std::vector<double> &a, std::vector<double> &b, int n)
{
    for (int j = 0; j < n; ++j) {
        double diagonal = a[std::size_t(j) * n + j];
        for (int k = 0; k < j; ++k) diagonal -= a[std::size_t(j) * n + k] * a[std::size_t(j) * n + k];
        if (diagonal <= 0) return false;
        const double l = std::sqrt(diagonal);
        a[std::size_t(j) * n + j] = l;
        for (int i = j + 1; i < n; ++i) {
            double v = a[std::size_t(i) * n + j];
            for (int k = 0; k < j; ++k) v -= a[std::size_t(i) * n + k] * a[std::size_t(j) * n + k];
            a[std::size_t(i) * n + j] = v / l;
        }
    }
    for (int i = 0; i < n; ++i) {
        double v = b[i];
        for (int k = 0; k < i; ++k) v -= a[std::size_t(i) * n + k] * b[k];
        b[i] = v / a[std::size_t(i) * n + i];
    }
    for (int i = n - 1; i >= 0; --i) {
        double v = b[i];
        for (int k = i + 1; k < n; ++k) v -= a[std::size_t(k) * n + i] * b[k];
        b[i] = v / a[std::size_t(i) * n + i];
    }
    return true;
}

} // namespace

float Image::at(int x, int y) const
{
    return data[std::size_t(clampIndex(y, height)) * width + clampIndex(x, width)];
}

Image resize(const Image &source, int width, int height)
{
    Image out;
    out.width = width;
    out.height = height;
    out.data.resize(std::size_t(width) * height);
    if (source.width <= 0 || source.height <= 0) return out;
    const double sx = double(source.width) / width, sy = double(source.height) / height;
    // Downscaling averages a box of source pixels (no aliasing); upscaling
    // interpolates.
    if (sx > 1.0 || sy > 1.0) {
        RawPipeline::parallelFor(0, height, [&](int y) {
            const double y0 = y * sy, y1 = (y + 1) * sy;
            for (int x = 0; x < width; ++x) {
                const double x0 = x * sx, x1 = (x + 1) * sx;
                double sum = 0, weight = 0;
                for (int j = int(y0); j < int(std::ceil(y1)) && j < source.height; ++j) {
                    const double wy = std::min(y1, j + 1.0) - std::max(y0, double(j));
                    for (int i = int(x0); i < int(std::ceil(x1)) && i < source.width; ++i) {
                        const double w = wy * (std::min(x1, i + 1.0) - std::max(x0, double(i)));
                        sum += w * source.data[std::size_t(j) * source.width + i];
                        weight += w;
                    }
                }
                out.data[std::size_t(y) * width + x] = float(weight > 0 ? sum / weight : 0);
            }
        });
        return out;
    }
    RawPipeline::parallelFor(0, height, [&](int y) {
        const double v = (y + 0.5) * sy - 0.5;
        const int y0 = int(std::floor(v));
        const float ay = float(v - y0);
        for (int x = 0; x < width; ++x) {
            const double u = (x + 0.5) * sx - 0.5;
            const int x0 = int(std::floor(u));
            const float ax = float(u - x0);
            const float a = source.at(x0, y0), b = source.at(x0 + 1, y0);
            const float c = source.at(x0, y0 + 1), d = source.at(x0 + 1, y0 + 1);
            out.data[std::size_t(y) * width + x] = (a + (b - a) * ax) * (1 - ay) + (c + (d - c) * ax) * ay;
        }
    });
    return out;
}

std::vector<int> hashImage(const Image &image, const Filters &filters, std::vector<float> *strength)
{
    const Structure s = structure(image);
    std::vector<int> buckets(s.angle.size());
    for (std::size_t i = 0; i < buckets.size(); ++i) buckets[i] = bucketOf(s, i, filters);
    if (strength) *strength = s.strength;
    return buckets;
}

Image upscale(const Image &source, int width, int height, const Filters &filters)
{
    Image coarse = resize(source, width, height);
    if (!filters.valid()) return coarse;
    const std::vector<int> buckets = hashImage(coarse, filters);
    Image out = coarse;
    RawPipeline::parallelFor(0, height, [&](int y) {
        double patch[Taps];
        for (int x = 0; x < width; ++x) {
            const std::size_t i = std::size_t(y) * width + x;
            patchAt(coarse, x, y, patch);
            const float *w = &filters.weights[std::size_t(buckets[i]) * Taps];
            double v = 0;
            for (int k = 0; k < Taps; ++k) v += w[k] * patch[k];
            // No overshoot beyond the 3x3 neighbourhood: sharp edges without
            // halos or ringing.
            const int c = Taps / 2;
            float lo = float(patch[c]), hi = lo;
            for (int j = -1; j <= 1; ++j)
                for (int k = -1; k <= 1; ++k) {
                    const float p = float(patch[c + j * Patch + k]);
                    lo = std::min(lo, p);
                    hi = std::max(hi, p);
                }
            out.data[i] = std::max(lo, std::min(hi, float(v)));
        }
    });
    return out;
}

bool load(const std::string &path, Filters *filters, std::string *error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    char magic[8];
    int32_t shape[4];
    Filters loaded;
    in.read(magic, sizeof magic);
    in.read(reinterpret_cast<char *>(shape), sizeof shape);
    in.read(reinterpret_cast<char *>(loaded.strengthThresholds.data()), sizeof(float) * loaded.strengthThresholds.size());
    in.read(reinterpret_cast<char *>(loaded.coherenceThresholds.data()), sizeof(float) * loaded.coherenceThresholds.size());
    if (!in || std::memcmp(magic, Magic, sizeof Magic) != 0 || shape[0] != Angles || shape[1] != Strengths
            || shape[2] != Coherences || shape[3] != Patch) {
        if (error) *error = "unsupported filter file " + path;
        return false;
    }
    loaded.weights.resize(std::size_t(Buckets) * Taps);
    in.read(reinterpret_cast<char *>(loaded.weights.data()), std::streamsize(sizeof(float) * loaded.weights.size()));
    if (!in) {
        if (error) *error = "truncated filter file " + path;
        return false;
    }
    *filters = std::move(loaded);
    return true;
}

bool save(const std::string &path, const Filters &filters, std::string *error)
{
    if (!filters.valid()) {
        if (error) *error = "no filters to save";
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    const int32_t shape[4] = {Angles, Strengths, Coherences, Patch};
    out.write(Magic, sizeof Magic);
    out.write(reinterpret_cast<const char *>(shape), sizeof shape);
    out.write(reinterpret_cast<const char *>(filters.strengthThresholds.data()), sizeof(float) * filters.strengthThresholds.size());
    out.write(reinterpret_cast<const char *>(filters.coherenceThresholds.data()), sizeof(float) * filters.coherenceThresholds.size());
    out.write(reinterpret_cast<const char *>(filters.weights.data()), std::streamsize(sizeof(float) * filters.weights.size()));
    if (!out) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

namespace {

Image degrade(const Image &high, double factor)
{
    const int lowWidth = std::max(1, int(std::lround(high.width / factor)));
    const int lowHeight = std::max(1, int(std::lround(high.height / factor)));
    return resize(resize(high, lowWidth, lowHeight), high.width, high.height);
}

}

void Trainer::calibrate(const std::vector<Image> &images)
{
    std::vector<float> samples;
    for (const Image &image : images) {
        const Structure s = structure(degrade(image, m_factor));
        for (std::size_t i = 0; i < s.strength.size(); i += 7) samples.push_back(s.strength[i]);
    }
    if (samples.empty()) return;
    for (int k = 0; k < Strengths - 1; ++k) {
        const std::size_t n = samples.size() * std::size_t(k + 1) / Strengths;
        std::nth_element(samples.begin(), samples.begin() + n, samples.end());
        m_hash.strengthThresholds[k] = samples[n];
    }
}

void Trainer::add(const Image &high, int sampleStride)
{
    const Image coarse = degrade(high, m_factor);
    const Structure s = structure(coarse);
    const int margin = Patch / 2 + int(std::ceil(m_factor));
    const int rows = std::max(0, (high.height - 2 * margin + sampleStride - 1) / sampleStride);
    std::mutex mutex;
    // Per-band accumulators merged under the lock (an 81x81 normal matrix
    // per bucket is too large to keep per row).
    const int bands = 16;
    RawPipeline::parallelFor(0, bands, [&](int band) {
        std::vector<double> normal(std::size_t(Buckets) * Taps * Taps, 0.0), target(std::size_t(Buckets) * Taps, 0.0);
        std::vector<double> count(Buckets, 0.0);
        double patch[Taps];
        for (int r = band; r < rows; r += bands) {
            const int y = margin + r * sampleStride;
            for (int x = margin + (r % sampleStride); x < high.width - margin; x += sampleStride) {
                const std::size_t i = std::size_t(y) * high.width + x;
                const int bucket = bucketOf(s, i, m_hash);
                patchAt(coarse, x, y, patch);
                double *a = &normal[std::size_t(bucket) * Taps * Taps];
                for (int p = 0; p < Taps; ++p) {
                    const double v = patch[p];
                    double *row = a + std::size_t(p) * Taps;
                    for (int q = p; q < Taps; ++q) row[q] += v * patch[q];
                }
                const double t = high.data[i];
                double *b = &target[std::size_t(bucket) * Taps];
                for (int p = 0; p < Taps; ++p) b[p] += t * patch[p];
                count[bucket] += 1;
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t k = 0; k < normal.size(); ++k) m_normal[k] += normal[k];
        for (std::size_t k = 0; k < target.size(); ++k) m_target[k] += target[k];
        for (int k = 0; k < Buckets; ++k) m_count[k] += count[k];
    });
}

Filters Trainer::solve(double regularisation) const
{
    Filters filters = m_hash;
    filters.weights.assign(std::size_t(Buckets) * Taps, 0.0f);
    for (int bucket = 0; bucket < Buckets; ++bucket) {
        float *w = &filters.weights[std::size_t(bucket) * Taps];
        w[Taps / 2] = 1.0f; // identity (plain bilinear) unless enough samples
        if (m_count[bucket] < 4 * Taps) continue;
        std::vector<double> a(m_normal.begin() + std::ptrdiff_t(bucket) * Taps * Taps,
                              m_normal.begin() + std::ptrdiff_t(bucket + 1) * Taps * Taps);
        std::vector<double> b(m_target.begin() + std::ptrdiff_t(bucket) * Taps,
                              m_target.begin() + std::ptrdiff_t(bucket + 1) * Taps);
        double trace = 0;
        for (int p = 0; p < Taps; ++p) {
            for (int q = 0; q < p; ++q) a[std::size_t(p) * Taps + q] = a[std::size_t(q) * Taps + p];
            trace += a[std::size_t(p) * Taps + p];
        }
        // Ridge towards zero, scaled to the data, keeps rare buckets tame.
        const double ridge = regularisation * trace / Taps + 1e-12;
        for (int p = 0; p < Taps; ++p) a[std::size_t(p) * Taps + p] += ridge;
        if (!choleskySolve(a, b, Taps)) continue;
        for (int p = 0; p < Taps; ++p) w[p] = float(b[p]);
    }
    return filters;
}

}
