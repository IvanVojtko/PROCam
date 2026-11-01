// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "rawpipelinemath.h"
#include "raisr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace RawPipeline {
namespace {

const double Pi = 3.14159265358979323846;

inline int clampIndex(int value, int size)
{
    return value < 0 ? 0 : value >= size ? size - 1 : value;
}

// Reflect about the edge pixel; keeps the parity of mosaic coordinates.
inline int reflect(int p, int size)
{
    if (p < 0) p = -p;
    if (p >= size) p = 2 * size - 2 - p;
    return clampIndex(p, size);
}

const double Luma[3] = {0.2126, 0.7152, 0.0722};

// sRGB OETF/EOTF through tables; inputs are clamped to [0, 1].
struct Curve {
    static const int Size = 4096;
    float encode[Size + 1];
    float decode[Size + 1];
    Curve()
    {
        for (int i = 0; i <= Size; ++i) {
            encode[i] = float(srgbEncode(double(i) / Size));
            decode[i] = float(srgbDecode(double(i) / Size));
        }
    }
    static float lookup(const float *table, float x)
    {
        if (!(x > 0.0f)) return table[0];
        if (x >= 1.0f) return table[Size];
        const float position = x * Size;
        const int index = int(position);
        const float fraction = position - index;
        return table[index] + (table[index + 1] - table[index]) * fraction;
    }
    float toGamma(float x) const { return lookup(encode, x); }
    float toLinear(float x) const { return lookup(decode, x); }
};

const Curve &curve()
{
    static const Curve instance;
    return instance;
}

// --- Image pyramids ----------------------------------------------------------

struct Image {
    int width = 0, height = 0;
    std::vector<float> data;
    Image() {}
    Image(int w, int h) : width(w), height(h), data(std::size_t(w) * h) {}
    float at(int x, int y) const { return data[std::size_t(clampIndex(y, height)) * width + clampIndex(x, width)]; }
};

Image boxDown(const Image &source, int factor)
{
    Image out(std::max(1, source.width / factor), std::max(1, source.height / factor));
    const float scale = 1.0f / float(factor * factor);
    parallelFor(0, out.height, [&](int y) {
        for (int x = 0; x < out.width; ++x) {
            float sum = 0.0f;
            for (int dy = 0; dy < factor; ++dy) {
                const float *row = &source.data[std::size_t(std::min(y * factor + dy, source.height - 1)) * source.width];
                for (int dx = 0; dx < factor; ++dx) sum += row[std::min(x * factor + dx, source.width - 1)];
            }
            out.data[std::size_t(y) * out.width + x] = sum * scale;
        }
    });
    return out;
}

// Burt-Adelson REDUCE: 5-tap binomial blur, then decimate. `sample(x, y)`
// supplies source values with clamped coordinates, so level 0 can be a
// function of another image instead of a stored buffer.
template <typename Sample>
Image reduce(int width, int height, Sample sample)
{
    static const float taps[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};
    Image out((width + 1) / 2, (height + 1) / 2);
    parallelFor(0, out.height, [&](int y) {
        for (int x = 0; x < out.width; ++x) {
            float sum = 0.0f;
            for (int j = 0; j < 5; ++j) {
                const int sy = clampIndex(2 * y + j - 2, height);
                float row = 0.0f;
                for (int i = 0; i < 5; ++i) row += taps[i] * sample(clampIndex(2 * x + i - 2, width), sy);
                sum += taps[j] * row;
            }
            out.data[std::size_t(y) * out.width + x] = sum;
        }
    });
    return out;
}

// EXPAND by bilinear interpolation; position x maps to coarse x / 2, the
// inverse of the decimation in reduce().
inline float expandAt(const Image &coarse, int x, int y)
{
    const int cx = x >> 1, cy = y >> 1;
    const float fx = (x & 1) ? 0.5f : 0.0f, fy = (y & 1) ? 0.5f : 0.0f;
    const float a = coarse.at(cx, cy), b = coarse.at(cx + 1, cy);
    const float c = coarse.at(cx, cy + 1), d = coarse.at(cx + 1, cy + 1);
    return (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;
}

// --- FFT ---------------------------------------------------------------------

struct Fft16 {
    float cosTable16[10], sinTable16[10]; // W16^m for m = n2 * k1 <= 9
    Fft16()
    {
        for (int m = 0; m < 10; ++m) {
            cosTable16[m] = float(std::cos(2 * Pi * m / 16));
            sinTable16[m] = float(std::sin(2 * Pi * m / 16));
        }
    }
    // 16-point DFT as 4x4 (two radix-4 stages with constant twiddles).
    void run(float *re, float *im, int stride, bool inverse) const
    {
        const float sign = inverse ? 1.0f : -1.0f;
        float yr[4][4], yi[4][4];
        // Stage 1: 4-point DFTs over n1 of x[4 n1 + n2], times W16^(n2 k1).
        for (int n2 = 0; n2 < 4; ++n2) {
            const float x0r = re[n2 * stride], x0i = im[n2 * stride];
            const float x1r = re[(n2 + 4) * stride], x1i = im[(n2 + 4) * stride];
            const float x2r = re[(n2 + 8) * stride], x2i = im[(n2 + 8) * stride];
            const float x3r = re[(n2 + 12) * stride], x3i = im[(n2 + 12) * stride];
            const float a0r = x0r + x2r, a0i = x0i + x2i, a1r = x0r - x2r, a1i = x0i - x2i;
            const float b0r = x1r + x3r, b0i = x1i + x3i, b1r = x1r - x3r, b1i = x1i - x3i;
            // X1 = a1 + sign*i*b1, X3 = a1 - sign*i*b1.
            float outr[4] = {a0r + b0r, a1r - sign * b1i, a0r - b0r, a1r + sign * b1i};
            float outi[4] = {a0i + b0i, a1i + sign * b1r, a0i - b0i, a1i - sign * b1r};
            for (int k1 = 0; k1 < 4; ++k1) {
                const int m = n2 * k1;
                const float wr = cosTable16[m], wi = sign * sinTable16[m];
                yr[n2][k1] = outr[k1] * wr - outi[k1] * wi;
                yi[n2][k1] = outr[k1] * wi + outi[k1] * wr;
            }
        }
        // Stage 2: 4-point DFTs over n2 give X[k1 + 4 k2].
        for (int k1 = 0; k1 < 4; ++k1) {
            const float a0r = yr[0][k1] + yr[2][k1], a0i = yi[0][k1] + yi[2][k1];
            const float a1r = yr[0][k1] - yr[2][k1], a1i = yi[0][k1] - yi[2][k1];
            const float b0r = yr[1][k1] + yr[3][k1], b0i = yi[1][k1] + yi[3][k1];
            const float b1r = yr[1][k1] - yr[3][k1], b1i = yi[1][k1] - yi[3][k1];
            re[k1 * stride] = a0r + b0r;
            im[k1 * stride] = a0i + b0i;
            re[(k1 + 4) * stride] = a1r - sign * b1i;
            im[(k1 + 4) * stride] = a1i + sign * b1r;
            re[(k1 + 8) * stride] = a0r - b0r;
            im[(k1 + 8) * stride] = a0i - b0i;
            re[(k1 + 12) * stride] = a1r + sign * b1i;
            im[(k1 + 12) * stride] = a1i - sign * b1r;
        }
    }
};

const Fft16 &fft()
{
    static const Fft16 instance;
    return instance;
}

// --- Merge tiles -------------------------------------------------------------

struct TileTables {
    float window[MergeTileSize];
    // Radial frequency in [0, 1] for spatial noise shaping (0 at DC).
    float radius[MergeTileSize * MergeTileSize];
    TileTables()
    {
        for (int i = 0; i < MergeTileSize; ++i)
            window[i] = float(0.5 - 0.5 * std::cos(2 * Pi * (i + 0.5) / MergeTileSize));
        for (int ky = 0; ky < MergeTileSize; ++ky) {
            for (int kx = 0; kx < MergeTileSize; ++kx) {
                const double fx = std::min(kx, MergeTileSize - kx), fy = std::min(ky, MergeTileSize - ky);
                radius[ky * MergeTileSize + kx] = float(std::sqrt(fx * fx + fy * fy) / (8 * std::sqrt(2.0)));
            }
        }
    }
};

const TileTables &tileTables()
{
    static const TileTables instance;
    return instance;
}

void loadTile(const std::vector<float> &plane, int width, int height, int originX, int originY,
              float *tile)
{
    const bool inside = originX >= 0 && originY >= 0 && originX + MergeTileSize <= width
            && originY + MergeTileSize <= height;
    for (int y = 0; y < MergeTileSize; ++y) {
        float *out = tile + y * MergeTileSize;
        if (inside) {
            const float *row = &plane[std::size_t(originY + y) * width + originX];
            std::copy(row, row + MergeTileSize, out);
        } else {
            const float *row = &plane[std::size_t(reflect(originY + y, height)) * width];
            for (int x = 0; x < MergeTileSize; ++x) out[x] = row[reflect(originX + x, width)];
        }
    }
}

void forward2d(float *re, float *im)
{
    const Fft16 &f = fft();
    for (int y = 0; y < 16; ++y) f.run(re + y * 16, im + y * 16, 1, false);
    for (int x = 0; x < 16; ++x) f.run(re + x, im + x, 16, false);
}

} // namespace

// --- Basic helpers -------------------------------------------------------------

double srgbEncode(double linear)
{
    linear = std::max(0.0, std::min(1.0, linear));
    return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
}

double srgbDecode(double encoded)
{
    encoded = std::max(0.0, std::min(1.0, encoded));
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

int phaseChannel(int cfa, int phase)
{
    static const int channels[4][4] = {{0, 1, 1, 2}, {1, 0, 2, 1}, {1, 2, 0, 1}, {2, 1, 1, 0}};
    return channels[cfa & 3][phase & 3];
}

int mergeTileCount(int planeSize)
{
    return (planeSize + MergeTileStride - 1) / MergeTileStride + 1;
}

double NoiseModel::variance(int phase, double signal) const
{
    return std::max(1e-12, scale[phase] * std::max(0.0, signal) + offset[phase]);
}

double NoiseModel::meanVariance(double signal) const
{
    double sum = 0;
    for (int p = 0; p < 4; ++p) sum += variance(p, signal);
    return sum / 4;
}

void fft16x16(float *re, float *im, bool inverse)
{
    const Fft16 &f = fft();
    for (int y = 0; y < 16; ++y) f.run(re + y * 16, im + y * 16, 1, inverse);
    for (int x = 0; x < 16; ++x) f.run(re + x, im + x, 16, inverse);
}

Planes toPlanes(const BayerFrame &frame, double exposureGain)
{
    Planes planes;
    planes.width = frame.width / 2;
    planes.height = frame.height / 2;
    planes.clip = float(exposureGain);
    float scale[4];
    for (int k = 0; k < 4; ++k) {
        planes.phase[k].resize(std::size_t(planes.width) * planes.height);
        scale[k] = float(exposureGain / std::max(1.0, frame.white - frame.black[k]));
    }
    parallelFor(0, planes.height, [&](int y) {
        for (int k = 0; k < 4; ++k) {
            const uint16_t *row = &frame.pixels[std::size_t(2 * y + (k >> 1)) * frame.width + (k & 1)];
            float *out = &planes.phase[k][std::size_t(y) * planes.width];
            const float black = float(frame.black[k]);
            for (int x = 0; x < planes.width; ++x) out[x] = (float(row[2 * x]) - black) * scale[k];
        }
    });
    return planes;
}

std::vector<float> grayImage(const Planes &planes)
{
    std::vector<float> gray(std::size_t(planes.width) * planes.height);
    parallelFor(0, planes.height, [&](int y) {
        const std::size_t base = std::size_t(y) * planes.width;
        for (int x = 0; x < planes.width; ++x) {
            const std::size_t i = base + x;
            gray[i] = 0.25f * (planes.phase[0][i] + planes.phase[1][i] + planes.phase[2][i] + planes.phase[3][i]);
        }
    });
    return gray;
}

double sharpness(const std::vector<float> &gray, int width, int height)
{
    // Gradient energy of the most detailed tenth of the frame: shake blurs
    // edges and texture, while flat areas (walls, sky) only carry noise,
    // which at high ISO swamped a whole-frame average (real 8-frame burst:
    // 0.4% spread over the frame, 4% on the detailed rack).
    Image image;
    image.width = width;
    image.height = height;
    image.data = gray;
    const Image small = boxDown(image, 2);
    const int tile = 32;
    const int tilesX = std::max(1, (small.width - 1) / tile), tilesY = std::max(1, (small.height - 1) / tile);
    std::vector<double> energy(std::size_t(tilesX) * tilesY, 0.0);
    for (int y = 1; y < small.height; ++y) {
        const int ty = std::min(tilesY - 1, (y - 1) / tile);
        for (int x = 1; x < small.width; ++x) {
            const float c = small.data[std::size_t(y) * small.width + x];
            energy[std::size_t(ty) * tilesX + std::min(tilesX - 1, (x - 1) / tile)]
                    += std::abs(c - small.data[std::size_t(y) * small.width + x - 1])
                    + std::abs(c - small.data[std::size_t(y - 1) * small.width + x]);
        }
    }
    const std::size_t top = std::max<std::size_t>(1, energy.size() / 10);
    std::nth_element(energy.begin(), energy.begin() + (energy.size() - top), energy.end());
    double sum = 0;
    for (std::size_t i = energy.size() - top; i < energy.size(); ++i) sum += energy[i];
    return sum / (double(top) * tile * tile);
}

// --- Alignment -----------------------------------------------------------------

namespace {

struct LevelSpec {
    int tile, radius;
    bool l1;
    bool subpixel;
};

struct Field {
    int tilesX = 0, tilesY = 0, stride = 0, tile = 0;
    std::vector<TileOffset> offsets;
    const TileOffset &at(int tx, int ty) const
    {
        return offsets[std::size_t(clampIndex(ty, tilesY)) * tilesX + clampIndex(tx, tilesX)];
    }
};

double tileCost(const Image &ref, const Image &alt, int ox, int oy, int tile, int dx, int dy,
                bool l1, double limit)
{
    const bool inside = ox >= 0 && oy >= 0 && ox + tile <= ref.width && oy + tile <= ref.height
            && ox + dx >= 0 && oy + dy >= 0 && ox + dx + tile <= alt.width && oy + dy + tile <= alt.height;
    double cost = 0;
    for (int y = 0; y < tile; ++y) {
        if (inside) {
            const float *r = &ref.data[std::size_t(oy + y) * ref.width + ox];
            const float *a = &alt.data[std::size_t(oy + y + dy) * alt.width + ox + dx];
            float row = 0.0f;
            if (l1) {
                for (int x = 0; x < tile; ++x) row += std::abs(r[x] - a[x]);
            } else {
                for (int x = 0; x < tile; ++x) {
                    const float d = r[x] - a[x];
                    row += d * d;
                }
            }
            cost += row;
        } else {
            for (int x = 0; x < tile; ++x) {
                const float d = ref.at(ox + x, oy + y) - alt.at(ox + x + dx, oy + y + dy);
                cost += l1 ? std::abs(d) : d * d;
            }
        }
        if (cost >= limit) return cost; // cannot beat the current best
    }
    return cost;
}

// Lucas-Kanade refinement of a whole-pixel tile offset: solves for the
// fractional shift from the reference gradients, resampling the alternate
// bilinearly, a few iterations.
void refineSubpixel(const Image &ref, const Image &alt, int ox, int oy, int tile, TileOffset *offset)
{
    // Reference values and gradients once (tile <= 16).
    float value[256], gradX[256], gradY[256];
    double xx = 0, xy = 0, yy = 0;
    for (int y = 0; y < tile; ++y) {
        for (int x = 0; x < tile; ++x) {
            const int i = y * tile + x;
            value[i] = ref.at(ox + x, oy + y);
            gradX[i] = 0.5f * (ref.at(ox + x + 1, oy + y) - ref.at(ox + x - 1, oy + y));
            gradY[i] = 0.5f * (ref.at(ox + x, oy + y + 1) - ref.at(ox + x, oy + y - 1));
            xx += double(gradX[i]) * gradX[i];
            xy += double(gradX[i]) * gradY[i];
            yy += double(gradY[i]) * gradY[i];
        }
    }
    const double det = xx * yy - xy * xy;
    if (!(det > 1e-12) || det < 1e-4 * (xx + yy) * (xx + yy)) return; // flat or a single edge
    float fx = 0, fy = 0;
    for (int iteration = 0; iteration < 3; ++iteration) {
        double bx = 0, by = 0;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        const float ax = fx - ix, ay = fy - iy;
        const int sx0 = ox + offset->dx + ix, sy0 = oy + offset->dy + iy;
        const bool inside = sx0 >= 0 && sy0 >= 0 && sx0 + tile + 1 < alt.width && sy0 + tile + 1 < alt.height;
        for (int y = 0; y < tile; ++y) {
            const float *top = inside ? &alt.data[std::size_t(sy0 + y) * alt.width + sx0] : nullptr;
            const float *bottom = inside ? top + alt.width : nullptr;
            for (int x = 0; x < tile; ++x) {
                float a, b, c, d;
                if (inside) {
                    a = top[x]; b = top[x + 1]; c = bottom[x]; d = bottom[x + 1];
                } else {
                    a = alt.at(sx0 + x, sy0 + y); b = alt.at(sx0 + x + 1, sy0 + y);
                    c = alt.at(sx0 + x, sy0 + y + 1); d = alt.at(sx0 + x + 1, sy0 + y + 1);
                }
                const int i = y * tile + x;
                const float error = (a + (b - a) * ax) * (1 - ay) + (c + (d - c) * ax) * ay - value[i];
                bx += double(gradX[i]) * error;
                by += double(gradY[i]) * error;
            }
        }
        // alt(x + f) ~ ref(x): f -= H^-1 * sum(g * error).
        fx -= float((yy * bx - xy * by) / det);
        fy -= float((xx * by - xy * bx) / det);
        fx = std::max(-1.0f, std::min(1.0f, fx));
        fy = std::max(-1.0f, std::min(1.0f, fy));
    }
    offset->fx = fx;
    offset->fy = fy;
    // Smaller eigenvalue of the structure tensor, per pixel.
    const double trace = xx + yy, gap = std::sqrt(std::max(0.0, (xx - yy) * (xx - yy) + 4 * xy * xy));
    offset->confidence = float(0.5 * (trace - gap) / (tile * tile));
}

// Robust weighted least-squares affine fit of the refined tile offsets.
void fitMotionModel(Alignment *alignment)
{
    struct Sample { double x, y, dx, dy, w; };
    std::vector<Sample> samples;
    for (int ty = 0; ty < alignment->tilesY; ++ty)
        for (int tx = 0; tx < alignment->tilesX; ++tx) {
            const TileOffset &o = alignment->at(tx, ty);
            if (o.confidence <= 0) continue;
            samples.push_back({double(tx * MergeTileStride), double(ty * MergeTileStride),
                               o.dx + o.fx, double(o.dy + o.fy), double(o.confidence)});
        }
    if (samples.size() < 12) return;
    double a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    // A few reweighting rounds: tiles far from the fit (moving objects,
    // repetitive texture) drop out.
    for (int round = 0; round < 4; ++round) {
        double m[3][3] = {{0}}, va[3] = {0}, vb[3] = {0};
        for (const Sample &s : samples) {
            double w = s.w;
            if (round > 0) {
                const double ex = a[0] + a[1] * s.x + a[2] * s.y - s.dx, ey = b[0] + b[1] * s.x + b[2] * s.y - s.dy;
                const double e2 = ex * ex + ey * ey, limit = round == 1 ? 1.0 : 0.25; // plane pixels squared
                if (e2 > limit) continue;
                w /= 1 + e2 / 0.01;
            }
            const double f[3] = {1, s.x, s.y};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) m[i][j] += w * f[i] * f[j];
                va[i] += w * f[i] * s.dx;
                vb[i] += w * f[i] * s.dy;
            }
        }
        // Solve the 3x3 normal equations (Cramer's rule).
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (!(std::abs(det) > 1e-12)) return;
        auto solve = [&](const double *v, double *out) {
            for (int c = 0; c < 3; ++c) {
                double n[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) n[i][j] = j == c ? v[i] : m[i][j];
                out[c] = (n[0][0] * (n[1][1] * n[2][2] - n[1][2] * n[2][1])
                          - n[0][1] * (n[1][0] * n[2][2] - n[1][2] * n[2][0])
                          + n[0][2] * (n[1][0] * n[2][1] - n[1][1] * n[2][0])) / det;
            }
        };
        solve(va, a);
        solve(vb, b);
    }
    for (int i = 0; i < 3; ++i) {
        alignment->a[i] = a[i];
        alignment->b[i] = b[i];
    }
    alignment->hasModel = true;
}

Field alignLevel(const Image &ref, const Image &alt, const LevelSpec &spec, const Field *coarse,
                 int factor)
{
    Field field;
    field.tile = spec.tile;
    field.stride = spec.tile / 2;
    field.tilesX = (ref.width + field.stride - 1) / field.stride + 1;
    field.tilesY = (ref.height + field.stride - 1) / field.stride + 1;
    field.offsets.resize(std::size_t(field.tilesX) * field.tilesY);
    parallelFor(0, field.tilesY, [&](int ty) {
        for (int tx = 0; tx < field.tilesX; ++tx) {
            const int ox = tx * field.stride - field.stride, oy = ty * field.stride - field.stride;
            TileOffset guess;
            if (coarse) {
                // Coarse tile whose centre is nearest to this tile's centre.
                const double cx = (ox + field.tile * 0.5) / factor, cy = (oy + field.tile * 0.5) / factor;
                const int ctx = int(std::lround((cx - coarse->tile * 0.5 + coarse->stride) / coarse->stride));
                const int cty = int(std::lround((cy - coarse->tile * 0.5 + coarse->stride) / coarse->stride));
                const TileOffset &c = coarse->at(ctx, cty);
                guess.dx = c.dx * factor;
                guess.dy = c.dy * factor;
                guess.fx = guess.fy = 0;
            }
            TileOffset best = guess;
            double bestCost = tileCost(ref, alt, ox, oy, field.tile, guess.dx, guess.dy, spec.l1, 1e300);
            for (int dy = -spec.radius; dy <= spec.radius; ++dy) {
                for (int dx = -spec.radius; dx <= spec.radius; ++dx) {
                    if (!dx && !dy) continue;
                    const double cost = tileCost(ref, alt, ox, oy, field.tile, guess.dx + dx, guess.dy + dy,
                                                 spec.l1, bestCost);
                    if (cost < bestCost) {
                        bestCost = cost;
                        best.dx = guess.dx + dx;
                        best.dy = guess.dy + dy;
                    }
                }
            }
            if (spec.subpixel) refineSubpixel(ref, alt, ox, oy, field.tile, &best);
            field.offsets[std::size_t(ty) * field.tilesX + tx] = best;
        }
    });
    return field;
}

} // namespace

Alignment align(const std::vector<float> &reference, const std::vector<float> &alternate, int width,
                int height)
{
    // Pyramid factors 2, 4, 4 as in HDR+: about +-128 plane pixels of search.
    static const int factors[3] = {2, 4, 4};
    static const LevelSpec specs[4] = {{16, 1, true, true}, {8, 4, false, false}, {8, 4, false, false}, {8, 4, false, false}};
    Image refLevels[4], altLevels[4];
    refLevels[0].width = altLevels[0].width = width;
    refLevels[0].height = altLevels[0].height = height;
    refLevels[0].data = reference;
    altLevels[0].data = alternate;
    int levels = 1;
    for (int l = 1; l < 4; ++l) {
        if (refLevels[l - 1].width / factors[l - 1] < 16 || refLevels[l - 1].height / factors[l - 1] < 16) break;
        refLevels[l] = boxDown(refLevels[l - 1], factors[l - 1]);
        altLevels[l] = boxDown(altLevels[l - 1], factors[l - 1]);
        levels = l + 1;
    }
    Field field;
    for (int l = levels - 1; l >= 0; --l) {
        const bool hasCoarse = l < levels - 1;
        field = alignLevel(refLevels[l], altLevels[l], specs[l], hasCoarse ? &field : nullptr,
                           hasCoarse ? factors[l] : 1);
    }
    Alignment alignment;
    alignment.tilesX = field.tilesX;
    alignment.tilesY = field.tilesY;
    alignment.offsets = field.offsets;
    fitMotionModel(&alignment);
    return alignment;
}

// --- Noise -----------------------------------------------------------------------

bool noiseFromProfile(const std::vector<double> &profile, NoiseModel *model)
{
    if (profile.size() != 2 && profile.size() < 8) return false;
    for (int p = 0; p < 4; ++p) {
        const int index = profile.size() == 2 ? 0 : 2 * p;
        const double s = profile[index], o = profile[index + 1];
        if (!std::isfinite(s) || !std::isfinite(o) || s <= 0 || s > 1 || o > 1 || o < -s) return false;
        model->scale[p] = s;
        model->offset[p] = std::max(1e-10, o);
    }
    return true;
}

NoiseModel estimateNoise(const Planes &planes)
{
    // Residual against the 4-neighbour mean has variance 1.25 * sigma^2 for
    // white noise. Texture only adds variance, so a low percentile of 16x16
    // block variances per brightness bin tracks the noise floor.
    const int block = 16, bins = 24;
    const double minSignal = 1e-3, maxSignal = 0.9;
    NoiseModel model;
    for (int p = 0; p < 4; ++p) {
        const std::vector<float> &plane = planes.phase[p];
        const int bx = (planes.width - 2) / block, by = (planes.height - 2) / block;
        std::vector<std::vector<double>> binned(bins);
        std::vector<double> binSignal(bins, 0.0);
        std::vector<std::vector<std::pair<double, double>>> perRow(std::max(0, by));
        parallelFor(0, by, [&](int j) {
            for (int i = 0; i < bx; ++i) {
                double mean = 0, residual = 0;
                bool clipped = false;
                for (int y = 1 + j * block; y < 1 + (j + 1) * block; ++y) {
                    const float *row = &plane[std::size_t(y) * planes.width];
                    const float *up = row - planes.width, *down = row + planes.width;
                    for (int x = 1 + i * block; x < 1 + (i + 1) * block; ++x) {
                        const float v = row[x];
                        clipped = clipped || v >= planes.clip * 0.95f;
                        const float r = v - 0.25f * (row[x - 1] + row[x + 1] + up[x] + down[x]);
                        mean += v;
                        residual += double(r) * r;
                    }
                }
                if (clipped) continue;
                mean /= block * block;
                residual /= block * block * 1.25;
                if (mean > minSignal * planes.clip && mean < maxSignal * planes.clip)
                    perRow[j].push_back(std::make_pair(mean, residual));
            }
        });
        for (const auto &row : perRow) {
            for (const auto &sample : row) {
                const double t = std::log(sample.first / (minSignal * planes.clip))
                        / std::log(maxSignal / minSignal);
                const int bin = std::max(0, std::min(bins - 1, int(t * bins)));
                binned[bin].push_back(sample.second);
                binSignal[bin] += sample.first;
            }
        }
        // Weighted least squares of variance = s * mean + o over the bins,
        // weights 1/variance^2 so dark and bright bins count alike.
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        int used = 0;
        for (int b = 0; b < bins; ++b) {
            std::vector<double> &values = binned[b];
            if (values.size() < 8) continue;
            const double meanSignal = binSignal[b] / values.size();
            const std::size_t k = values.size() / 5;
            std::nth_element(values.begin(), values.begin() + k, values.end());
            // A 256-sample variance has ~9% spread; its 20th percentile sits
            // about 8% below the true value.
            const double v = values[k] / 0.92;
            if (!(v > 0)) continue;
            const double w = 1.0 / (v * v);
            sw += w;
            sx += w * meanSignal;
            sy += w * v;
            sxx += w * meanSignal * meanSignal;
            sxy += w * meanSignal * v;
            ++used;
        }
        double s = 0, o = 0;
        const double det = sw * sxx - sx * sx;
        if (used >= 2 && det > 0) {
            s = (sw * sxy - sx * sy) / det;
            o = (sy - s * sx) / sw;
        }
        if (!(s > 0) || !std::isfinite(s)) {
            // Too little flat area: assume pure shot noise at the bin average.
            s = used ? std::max(1e-7, sy / std::max(sx, 1e-12)) : 1e-4;
            o = 1e-8;
        }
        model.scale[p] = s;
        model.offset[p] = std::max(1e-10, o);
    }
    return model;
}

// --- Merge -------------------------------------------------------------------------

namespace {

// Loads the 16x16 tiles of all four phase planes of a mosaic in one pass,
// normalised like toPlanes().
void loadMosaicTiles(const BayerFrame &frame, const float *black, const float *scale, int originX,
                     int originY, float (*tiles)[256])
{
    const int width = frame.width / 2, height = frame.height / 2;
    const bool insideX = originX >= 0 && originX + MergeTileSize <= width;
    for (int y = 0; y < MergeTileSize; ++y) {
        const int py = reflect(originY + y, height);
        for (int half = 0; half < 2; ++half) {
            const uint16_t *row = &frame.pixels[std::size_t(2 * py + half) * frame.width];
            float *even = tiles[2 * half] + y * MergeTileSize, *odd = tiles[2 * half + 1] + y * MergeTileSize;
            const float blackEven = black[2 * half], blackOdd = black[2 * half + 1];
            const float scaleEven = scale[2 * half], scaleOdd = scale[2 * half + 1];
            for (int x = 0; x < MergeTileSize; ++x) {
                const int px = insideX ? originX + x : reflect(originX + x, width);
                even[x] = (float(row[2 * px]) - blackEven) * scaleEven;
                odd[x] = (float(row[2 * px + 1]) - blackOdd) * scaleOdd;
            }
        }
    }
}

// Mirror index (-k) of each 16x16 DFT coefficient.
struct MirrorTable {
    int index[256];
    MirrorTable()
    {
        for (int ky = 0; ky < 16; ++ky)
            for (int kx = 0; kx < 16; ++kx) index[ky * 16 + kx] = ((16 - ky) & 15) * 16 + ((16 - kx) & 15);
    }
};

const MirrorTable &mirror()
{
    static const MirrorTable instance;
    return instance;
}

// Spectra of two real tiles from one complex transform of a + ib.
void forwardPair(const float *a, const float *b, float *ar, float *ai, float *br, float *bi)
{
    float zr[256], zi[256];
    std::copy(a, a + 256, zr);
    std::copy(b, b + 256, zi);
    forward2d(zr, zi);
    const int *m = mirror().index;
    for (int k = 0; k < 256; ++k) {
        const int j = m[k];
        ar[k] = 0.5f * (zr[k] + zr[j]);
        ai[k] = 0.5f * (zi[k] - zi[j]);
        br[k] = 0.5f * (zi[k] + zi[j]);
        bi[k] = -0.5f * (zr[k] - zr[j]);
    }
}

// Inverse of two Hermitian spectra from one complex transform; outputs are
// scaled by 256 (the unnormalised DFT).
void inversePair(const float *ar, const float *ai, const float *br, const float *bi, float *a, float *b)
{
    float xr[256], xi[256];
    for (int k = 0; k < 256; ++k) {
        // conj(A + iB): the forward transform then inverts.
        xr[k] = ar[k] - bi[k];
        xi[k] = -(ai[k] + br[k]);
    }
    forward2d(xr, xi);
    for (int k = 0; k < 256; ++k) {
        a[k] = xr[k];
        b[k] = -xi[k];
    }
}

// Shared merge over any frame storage: load(z, x, y, tiles) fills the four
// phase tiles of frame z (0 is the reference). Two real planes share each
// complex FFT.
template <typename Load>
Planes mergeFrames(int width, int height, float clip, int frameCount, Load load,
                   const std::vector<Alignment> &alignments, const NoiseModel &noise,
                   const MergeOptions &options, MergeStats *stats)
{
    const int tilesX = mergeTileCount(width), tilesY = mergeTileCount(height);
    Planes out;
    out.width = width;
    out.height = height;
    out.clip = clip;
    for (int p = 0; p < 4; ++p) out.phase[p].assign(std::size_t(width) * height, 0.0f);

    const TileTables &tables = tileTables();
    const float temporal = float(std::max(0.0, options.temporalStrength));
    const float spatial = float(std::max(0.0, options.spatialStrength));
    const float floor = float(std::max(0.0, std::min(1.0, options.spatialFloor)));
    const float pilot = float(std::max(0.0, options.spatialPilot));
    std::vector<double> rowRejection(tilesY, 0.0);

    // Tile rows two apart never overlap, so each pass can run in parallel.
    for (int pass = 0; pass < 2; ++pass) {
        parallelFor(0, (tilesY + 1 - pass) / 2, [&](int index) {
            const int ty = 2 * index + pass;
            float tile[4][256], rr[4][256], ri[4][256], ar[4][256], ai[4][256], mr[4][256], mi[4][256];
            double rejection = 0;
            for (int tx = 0; tx < tilesX; ++tx) {
                const int ox = tx * MergeTileStride - MergeTileStride;
                const int oy = ty * MergeTileStride - MergeTileStride;
                load(0, ox, oy, tile);
                float sigma2[4];
                for (int p = 0; p < 4; ++p) {
                    double mean = 0;
                    for (int i = 0; i < 256; ++i) mean += tile[p][i];
                    // Variance of an unnormalised 256-point DFT coefficient.
                    sigma2[p] = float(noise.variance(p, mean / 256) * 256.0);
                }
                forwardPair(tile[0], tile[1], rr[0], ri[0], rr[1], ri[1]);
                forwardPair(tile[2], tile[3], rr[2], ri[2], rr[3], ri[3]);
                for (int p = 0; p < 4; ++p) {
                    std::copy(rr[p], rr[p] + 256, mr[p]);
                    std::copy(ri[p], ri[p] + 256, mi[p]);
                }
                for (int z = 1; z < frameCount; ++z) {
                    const TileOffset offset = z - 1 < int(alignments.size())
                            && alignments[z - 1].tilesX == tilesX && alignments[z - 1].tilesY == tilesY
                            ? alignments[z - 1].at(tx, ty) : TileOffset();
                    load(z, ox + offset.dx, oy + offset.dy, tile);
                    forwardPair(tile[0], tile[1], ar[0], ai[0], ar[1], ai[1]);
                    forwardPair(tile[2], tile[3], ar[2], ai[2], ar[3], ai[3]);
                    for (int p = 0; p < 4; ++p) {
                        const float threshold = temporal * sigma2[p];
                        double weightSum = 0;
                        for (int i = 0; i < 256; ++i) {
                            const float dr = rr[p][i] - ar[p][i], di = ri[p][i] - ai[p][i];
                            const float d2 = dr * dr + di * di;
                            const float a = d2 / (d2 + threshold + 1e-30f);
                            mr[p][i] += ar[p][i] + a * dr;
                            mi[p][i] += ai[p][i] + a * di;
                            weightSum += a;
                        }
                        rejection += weightSum / 256;
                    }
                }
                const float inverseCount = 1.0f / frameCount;
                for (int p = 0; p < 4; ++p) {
                    const float merged2 = sigma2[p] / frameCount;
                    for (int i = 0; i < 256; ++i) {
                        float re = mr[p][i] * inverseCount, im = mi[p][i] * inverseCount;
                        if (spatial > 0 && i) {
                            const float m2 = re * re + im * im;
                            const float shaping = floor + (1.0f - floor) * tables.radius[i];
                            const float noise = spatial * shaping * merged2;
                            float power = m2;
                            if (pilot > 0) {
                                const float first = m2 / (m2 + pilot * shaping * merged2 + 1e-30f);
                                power = m2 * first * first;
                            }
                            const float shrink = power / (power + noise + 1e-30f);
                            re *= shrink;
                            im *= shrink;
                        }
                        mr[p][i] = re;
                        mi[p][i] = im;
                    }
                }
                inversePair(mr[0], mi[0], mr[1], mi[1], tile[0], tile[1]);
                inversePair(mr[2], mi[2], mr[3], mi[3], tile[2], tile[3]);
                for (int p = 0; p < 4; ++p) {
                    std::vector<float> &plane = out.phase[p];
                    for (int y = 0; y < MergeTileSize; ++y) {
                        const int py = oy + y;
                        if (py < 0 || py >= height) continue;
                        const float wy = tables.window[y] / 256.0f;
                        float *row = &plane[std::size_t(py) * width];
                        for (int x = 0; x < MergeTileSize; ++x) {
                            const int px = ox + x;
                            if (px < 0 || px >= width) continue;
                            row[px] += tile[p][y * 16 + x] * wy * tables.window[x];
                        }
                    }
                }
            }
            rowRejection[ty] = rejection;
        });
    }
    if (stats) {
        double total = 0;
        for (double r : rowRejection) total += r;
        const double count = double(tilesX) * tilesY * 4 * std::max(1, frameCount - 1);
        stats->rejection = frameCount > 1 ? total / count : 0;
    }
    return out;
}

} // namespace

Planes merge(const std::vector<const Planes *> &frames, const std::vector<Alignment> &alignments,
             const NoiseModel &noise, const MergeOptions &options, MergeStats *stats)
{
    const Planes &reference = *frames.front();
    return mergeFrames(reference.width, reference.height, reference.clip, int(frames.size()),
                       [&](int z, int x, int y, float (*tiles)[256]) {
                           for (int p = 0; p < 4; ++p)
                               loadTile(frames[z]->phase[p], reference.width, reference.height, x, y, tiles[p]);
                       },
                       alignments, noise, options, stats);
}

Planes merge(const std::vector<const BayerFrame *> &frames, const std::vector<double> &exposureGains,
             const std::vector<Alignment> &alignments, const NoiseModel &noise,
             const MergeOptions &options, MergeStats *stats)
{
    const BayerFrame &reference = *frames.front();
    std::vector<std::array<float, 4>> black(frames.size()), scale(frames.size());
    for (std::size_t z = 0; z < frames.size(); ++z) {
        const double gain = z < exposureGains.size() ? exposureGains[z] : 1.0;
        for (int p = 0; p < 4; ++p) {
            black[z][p] = float(frames[z]->black[p]);
            scale[z][p] = float(gain / std::max(1.0, frames[z]->white - frames[z]->black[p]));
        }
    }
    const float clip = float(exposureGains.empty() ? 1.0 : exposureGains.front());
    return mergeFrames(reference.width / 2, reference.height / 2, clip, int(frames.size()),
                       [&](int z, int x, int y, float (*tiles)[256]) {
                           loadMosaicTiles(*frames[z], black[z].data(), scale[z].data(), x, y, tiles);
                       },
                       alignments, noise, options, stats);
}

std::vector<float> greenImage(const BayerFrame &frame, double exposureGain)
{
    const int width = frame.width / 2, height = frame.height / 2;
    std::vector<float> green(std::size_t(width) * height);
    int phases[2], count = 0;
    for (int p = 0; p < 4 && count < 2; ++p)
        if (phaseChannel(frame.cfa, p) == 1) phases[count++] = p;
    float scale[2], black[2];
    for (int k = 0; k < 2; ++k) {
        black[k] = float(frame.black[phases[k]]);
        scale[k] = float(0.5 * exposureGain / std::max(1.0, frame.white - frame.black[phases[k]]));
    }
    parallelFor(0, height, [&](int y) {
        const uint16_t *a = &frame.pixels[std::size_t(2 * y + (phases[0] >> 1)) * frame.width + (phases[0] & 1)];
        const uint16_t *b = &frame.pixels[std::size_t(2 * y + (phases[1] >> 1)) * frame.width + (phases[1] & 1)];
        for (int x = 0; x < width; ++x)
            green[std::size_t(y) * width + x] = (float(a[2 * x]) - black[0]) * scale[0] + (float(b[2 * x]) - black[1]) * scale[1];
    });
    return green;
}

std::vector<float> grayImage(const BayerFrame &frame, double exposureGain)
{
    const int width = frame.width / 2, height = frame.height / 2;
    std::vector<float> gray(std::size_t(width) * height);
    float scale[4];
    for (int p = 0; p < 4; ++p) scale[p] = float(0.25 * exposureGain / std::max(1.0, frame.white - frame.black[p]));
    parallelFor(0, height, [&](int y) {
        const uint16_t *top = &frame.pixels[std::size_t(2 * y) * frame.width];
        const uint16_t *bottom = top + frame.width;
        for (int x = 0; x < width; ++x) {
            gray[std::size_t(y) * width + x] = (float(top[2 * x]) - float(frame.black[0])) * scale[0]
                    + (float(top[2 * x + 1]) - float(frame.black[1])) * scale[1]
                    + (float(bottom[2 * x]) - float(frame.black[2])) * scale[2]
                    + (float(bottom[2 * x + 1]) - float(frame.black[3])) * scale[3];
        }
    });
    return gray;
}

BurstPlan planBurst(int64_t meteredExposureNs, int meteredIso, double clippedFraction,
                    int64_t minExposureNs, int64_t maxExposureNs, int minIso, int maxIso,
                    int maxFrames, bool equalExposure, bool night, const BurstScene &scene)
{
    BurstPlan plan;
    if (meteredExposureNs <= 0 || meteredIso <= 0 || maxFrames < 2 || minIso <= 0 || maxIso < minIso
            || minExposureNs <= 0 || maxExposureNs < minExposureNs)
        return plan;
    // More headroom the more of the metered frame is already clipped.
    // YUV frames (tone curve applied, 8 bit) take less: only when the
    // preview clips, and the tone mapping lifts the rest back.
    double under = equalExposure
            ? (clippedFraction < 0.002 ? 0.0 : clippedFraction < 0.02 ? 0.7
               : clippedFraction < 0.08 ? 1.2 : 1.7)
            : clippedFraction < 0.002 ? 0.5 : clippedFraction < 0.02 ? 1.0
            : clippedFraction < 0.08 ? 1.7 : 2.5;
    // A face in hard light: keep its highlights (a blown forehead cannot be
    // recovered, least of all from YUV frames, which are tone mapped).
    if (scene.faceLuma >= 0) {
        if (scene.faceClipped > 0.02) {
            under = std::max(under, equalExposure ? 1.2 : 1.7);
        } else if (scene.faceLuma > 0.8) {
            under = std::max(under, equalExposure ? 0.7 : 1.0);
        } else if (equalExposure) {
            // Tone-mapped YUV frames cannot lift a face captured dark (it
            // turns noisy and too red): the face sets the exposure, at
            // about 0.3 in the viewfinder, up to 2 EV above the meter for a
            // face lit from behind; the bright background may clip.
            under = std::min(under, std::max(-2.0, 2.2 * std::log2(std::max(0.03, scene.faceLuma) / 0.3)));
        } else if (scene.faceLuma < 0.25) {
            // RAW keeps the highlights in its headroom and only gives up
            // most of its usual underexposure for a dark face.
            under = std::min(under, 0.5);
        }
    }
    const double metered = double(meteredExposureNs) * meteredIso;
    const double wanted = metered * std::pow(2.0, (night ? 2.0 : 0.0) - under);
    // Short frames first: alignment fixes shake between frames, not blur
    // within one, and merging recovers the noise of the higher gain (8
    // frames: about 2.5x). 1/60 s while that stays at ISO 800 or below,
    // 1/30 s up to ISO 3200 (a real indoor burst at 1/15 s was visibly
    // shaken), and the 1/15 s handheld limit only in dim light. Night mode
    // asks the user to hold still and goes to 1/5 s before raising the gain.
    double limit = 1e9 / 60;
    if (wanted / limit > 800) limit = 1e9 / 30;
    if (wanted / limit > 3200) limit = 1e9 / 15;
    if (night) limit = 1e9 / 5;
    if (scene.motion >= 0) {
        // Blur budget in frame widths (1.5 px of a 4096 px frame), against
        // the measured motion; a floor so a quiet reading never trusts more
        // than about a tripod's steadiness.
        const double budget = (night ? 2.5 : 1.5) / 4096;
        const double speed = std::max(scene.motion, night ? 0.0015 : 0.004);
        double seconds = budget / speed;
        // Moving subjects: short frames whatever the hand does.
        if (scene.subjectMotion > 0.05) seconds = std::min(seconds, night ? 1.0 / 15 : 1.0 / 60);
        seconds = std::max(night ? 1.0 / 15 : 1.0 / 250, std::min(night ? 1.0 / 3 : 1.0 / 15, seconds));
        limit = 1e9 * seconds;
    }
    double time = std::min(limit, wanted / minIso);
    time = std::max(double(minExposureNs), std::min(double(maxExposureNs), time));
    const int iso = int(std::max<double>(minIso, std::min<double>(maxIso, std::round(wanted / time))));
    plan.exposureNs = int64_t(std::llround(time));
    plan.iso = iso;
    const double captured = double(plan.exposureNs) * iso;
    plan.targetGain = std::max(0.25, std::min(16.0, metered / captured));
    plan.underexposureEv = std::log2(metered / captured);
    plan.frames = night ? maxFrames
                        : std::max(2, std::min(maxFrames, 4 + int(std::lround(under)) + (iso >= 400 ? 2 : 0)
                                                          + (iso >= 1600 ? 2 : 0)));
    return plan;
}

// --- Super-resolution -----------------------------------------------------------------

namespace {

std::vector<float> gaussianBlur(const std::vector<float> &source, int width, int height, double sigma);

// Offset field of one frame in sensor pixels at plane position (u, v),
// bilinear between tile centres (tile tx is centred on plane x = 8 tx).
inline void tileOffsetAt(const Alignment &alignment, float u, float v, float *dx, float *dy)
{
    if (alignment.hasModel) {
        *dx = float(2.0 * (alignment.a[0] + alignment.a[1] * u + alignment.a[2] * v));
        *dy = float(2.0 * (alignment.b[0] + alignment.b[1] * u + alignment.b[2] * v));
        return;
    }
    const float gx = std::max(0.0f, std::min(float(alignment.tilesX - 1), u / MergeTileStride));
    const float gy = std::max(0.0f, std::min(float(alignment.tilesY - 1), v / MergeTileStride));
    const int x0 = std::min(alignment.tilesX - 2, int(gx)), y0 = std::min(alignment.tilesY - 2, int(gy));
    const float ax = gx - x0, ay = gy - y0;
    auto value = [&](int tx, int ty, bool horizontal) {
        const TileOffset &o = alignment.at(tx, ty);
        return horizontal ? o.dx + o.fx : o.dy + o.fy;
    };
    for (int axis = 0; axis < 2; ++axis) {
        const bool horizontal = axis == 0;
        const float a = value(x0, y0, horizontal), b = value(x0 + 1, y0, horizontal);
        const float c = value(x0, y0 + 1, horizontal), d = value(x0 + 1, y0 + 1, horizontal);
        const float plane = (a + (b - a) * ax) * (1 - ay) + (c + (d - c) * ax) * ay;
        (horizontal ? *dx : *dy) = 2.0f * plane; // plane pixels are two sensor pixels
    }
}

std::vector<float> boxMean3(const std::vector<float> &source, int width, int height)
{
    std::vector<float> out(source.size());
    parallelFor(0, height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            float sum = 0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                    sum += source[std::size_t(clampIndex(y + j, height)) * width + clampIndex(x + i, width)];
            out[std::size_t(y) * width + x] = sum / 9;
        }
    });
    return out;
}

} // namespace

std::vector<float> superResolveGreen(const std::vector<const BayerFrame *> &frames,
                                     const std::vector<double> &exposureGains,
                                     const std::vector<Alignment> &alignments, const Planes &guide,
                                     const NoiseModel &noise, const ZoomRegion *region,
                                     std::vector<float> *coverage)
{
    const BayerFrame &reference = *frames.front();
    const int width = reference.width, height = reference.height;
    const int planeWidth = width / 2, planeHeight = height / 2;
    const int frameCount = int(frames.size());
    int greenPhases[2], count = 0;
    for (int p = 0; p < 4 && count < 2; ++p)
        if (phaseChannel(reference.cfa, p) == 1) greenPhases[count++] = p;

    // Robustness per alternate frame on the plane grid: local green of the
    // aligned frame against the guide, tolerant of noise and of texture
    // (residual misalignment), then a 3x3 minimum to stay conservative.
    std::vector<float> guideGreen(std::size_t(planeWidth) * planeHeight);
    for (std::size_t i = 0; i < guideGreen.size(); ++i)
        guideGreen[i] = 0.5f * (guide.phase[greenPhases[0]][i] + guide.phase[greenPhases[1]][i]);
    const std::vector<float> guideMean = boxMean3(guideGreen, planeWidth, planeHeight);
    std::vector<float> guideVariance(guideGreen.size());
    {
        std::vector<float> squared(guideGreen.size());
        for (std::size_t i = 0; i < squared.size(); ++i) squared[i] = guideGreen[i] * guideGreen[i];
        squared = boxMean3(squared, planeWidth, planeHeight);
        for (std::size_t i = 0; i < squared.size(); ++i)
            guideVariance[i] = std::max(0.0f, squared[i] - guideMean[i] * guideMean[i]);
    }
    std::vector<std::vector<float>> robustness(frameCount);
    for (int z = 1; z < frameCount; ++z) {
        const Alignment &alignment = alignments[z - 1];
        const std::vector<float> green = greenImage(*frames[z], exposureGains[z]);
        std::vector<float> aligned(green.size());
        parallelFor(0, planeHeight, [&](int y) {
            for (int x = 0; x < planeWidth; ++x) {
                float dx, dy;
                tileOffsetAt(alignment, float(x), float(y), &dx, &dy);
                const int sx = clampIndex(x + int(std::lround(dx / 2)), planeWidth);
                const int sy = clampIndex(y + int(std::lround(dy / 2)), planeHeight);
                aligned[std::size_t(y) * planeWidth + x] = green[std::size_t(sy) * planeWidth + sx];
            }
        });
        const std::vector<float> alignedMean = boxMean3(aligned, planeWidth, planeHeight);
        std::vector<float> weight(green.size());
        parallelFor(0, planeHeight, [&](int y) {
            for (int x = 0; x < planeWidth; ++x) {
                const std::size_t i = std::size_t(y) * planeWidth + x;
                // Two green samples averaged over 3x3, in both images.
                const double noiseVariance = noise.variance(greenPhases[0], guideMean[i]) / 9.0;
                const double tolerance = noiseVariance + 0.1 * guideVariance[i];
                const double d = alignedMean[i] - guideMean[i];
                weight[i] = float(std::exp(-d * d / (2 * 6.25 * tolerance + 1e-12)));
            }
        });
        robustness[z].resize(weight.size());
        parallelFor(0, planeHeight, [&](int y) {
            for (int x = 0; x < planeWidth; ++x) {
                float minimum = 1.0f;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i)
                        minimum = std::min(minimum, weight[std::size_t(clampIndex(y + j, planeHeight)) * planeWidth
                                                           + clampIndex(x + i, planeWidth)]);
                robustness[z][std::size_t(y) * planeWidth + x] = minimum;
            }
        });
    }

    // Narrow Gaussian splat (sigma 0.3 sensor pixels; wider kernels blur
    // away the detail being recovered) over the nearest output pixels.
    // Output pixel centres map to sensor positions (o + 0.5) / scale - 0.5 + origin.
    ZoomRegion window;
    window.width = width;
    window.height = height;
    if (region && region->active()) window = *region;
    const float scale = float(window.scale);
    const int outWidth = window.outputWidth(), outHeight = window.outputHeight();
    const float sigma = 0.3f;
    float kernel[65];
    for (int i = 0; i <= 64; ++i) {
        const float t = 1.5f * i / 64;
        kernel[i] = std::exp(-t * t / (2 * sigma * sigma));
    }
    // t in sensor pixels.
    auto gauss = [&](float t) { t = std::abs(t); return t >= 1.5f ? 0.0f : kernel[int(t * (64 / 1.5f) + 0.5f)]; };
    auto toOutput = [&](float sensor, int origin) { return (sensor - origin + 0.5f) * scale - 0.5f; };
    auto toSensor = [&](float output, int origin) { return (output + 0.5f) / scale - 0.5f + origin; };

    std::vector<float> numerator(std::size_t(outWidth) * outHeight, 0.0f), denominator(numerator.size(), 0.0f);
    const int cellsX = (outWidth + 1) / 2, cellsY = (outHeight + 1) / 2;
    if (coverage) coverage->assign(std::size_t(cellsX) * cellsY, 0.0f);
    // Clipped reference samples per cell: they are skipped, so the cell's
    // estimate leans on its darker neighbours and would dull bright detail
    // (white text, lamps). The swap fades out around them.
    std::vector<float> clipped(coverage ? coverage->size() : 0, 0.0f);
    const int band = 32; // even: a band owns whole rows of coverage cells
    parallelFor(0, (outHeight + band - 1) / band, [&](int bandIndex) {
        const int y0 = bandIndex * band, y1 = std::min(outHeight, y0 + band);
        // Sensor rows of this band (before motion).
        const float sensorTop = toSensor(y0 - scale, window.y), sensorBottom = toSensor(y1 + scale, window.y);
        for (int z = 0; z < frameCount; ++z) {
            const BayerFrame &frame = *frames[z];
            const double gain = z < int(exposureGains.size()) ? exposureGains[z] : 1.0;
            float black[2], scaleValue[2];
            uint16_t clipLevel[2];
            for (int k = 0; k < 2; ++k) {
                const int p = greenPhases[k];
                black[k] = float(frame.black[p]);
                scaleValue[k] = float(gain / std::max(1.0, frame.white - frame.black[p]));
                clipLevel[k] = uint16_t(std::min(65535.0, frame.black[p] + 0.97 * (frame.white - frame.black[p])));
            }
            // Source rows and columns that can land in this band.
            float minDy = 0, maxDy = 0, minDx = 0, maxDx = 0;
            if (z > 0) {
                const Alignment &alignment = alignments[z - 1];
                const int t0 = std::max(0, int(sensorTop / 2) / MergeTileStride - 1);
                const int t1 = std::min(alignment.tilesY - 1, int(sensorBottom / 2) / MergeTileStride + 2);
                minDy = minDx = 1e9f;
                maxDy = maxDx = -1e9f;
                for (int ty = t0; ty <= t1; ++ty)
                    for (int tx = 0; tx < alignment.tilesX; ++tx) {
                        float ox, oy;
                        tileOffsetAt(alignment, float(tx * MergeTileStride), float(ty * MergeTileStride), &ox, &oy);
                        minDy = std::min(minDy, oy);
                        maxDy = std::max(maxDy, oy);
                        minDx = std::min(minDx, ox);
                        maxDx = std::max(maxDx, ox);
                    }
            }
            const int s0 = std::max(0, int(std::floor(sensorTop + minDy)) - 2);
            const int s1 = std::min(height - 1, int(std::ceil(sensorBottom + maxDy)) + 2);
            const int c0 = std::max(0, int(std::floor(window.x - 2 + minDx)));
            const int c1 = std::min(width - 1, int(std::ceil(window.x + window.width + 2 + maxDx)));
            for (int ys = s0; ys <= s1; ++ys) {
                const uint16_t *row = &frame.pixels[std::size_t(ys) * width];
                for (int k = 0; k < 2; ++k) {
                    const int p = greenPhases[k];
                    if ((ys & 1) != (p >> 1)) continue;
                    for (int xs = (c0 & ~1) + (p & 1); xs <= c1; xs += 2) {
                        const uint16_t raw = row[xs];
                        if (raw >= clipLevel[k]) {
                            if (coverage && z == 0) {
                                const float cx = toOutput(float(xs), window.x), cy = toOutput(float(ys), window.y);
                                if (cy >= y0 && cy < y1 && cx >= 0 && cx < outWidth)
                                    clipped[std::size_t(int(cy) / 2) * cellsX + int(cx) / 2] += 1.0f;
                            }
                            continue;
                        }
                        float dx = 0, dy = 0, r = 1.0f;
                        if (z > 0) {
                            tileOffsetAt(alignments[z - 1], xs * 0.5f, ys * 0.5f, &dx, &dy);
                        }
                        const float mx = xs - dx, my = ys - dy; // reference sensor position
                        const float ox = toOutput(mx, window.x), oy = toOutput(my, window.y);
                        if (oy < y0 - 1.5f * scale || oy >= y1 + 1.5f * scale
                                || ox < -1.5f * scale || ox >= outWidth + 1.5f * scale) continue;
                        if (z > 0) {
                            const int rx = clampIndex(int(mx * 0.5f), planeWidth), ry = clampIndex(int(my * 0.5f), planeHeight);
                            r = robustness[z][std::size_t(ry) * planeWidth + rx];
                            if (r < 1e-3f) continue;
                        }
                        if (coverage && z > 0 && oy >= y0 && oy < y1 && ox >= 0 && ox < outWidth)
                            (*coverage)[std::size_t(int(oy) / 2) * cellsX + int(ox) / 2] += r;
                        const float value = (float(raw) - black[k]) * scaleValue[k];
                        // sigma 0.3: beyond one sensor pixel the weight is
                        // negligible, so the nearest pixels carry it all.
                        const int px0 = int(std::ceil(ox - scale)), py0 = std::max(y0, int(std::ceil(oy - scale)));
                        const int px1 = int(std::floor(ox + scale)), py1 = std::min(y1 - 1, int(std::floor(oy + scale)));
                        for (int py = py0; py <= py1; ++py) {
                            const float wy = gauss((py - oy) / scale) * r;
                            if (wy <= 0) continue;
                            float *num = &numerator[std::size_t(py) * outWidth], *den = &denominator[std::size_t(py) * outWidth];
                            for (int px = std::max(0, px0); px <= std::min(outWidth - 1, px1); ++px) {
                                const float w = wy * gauss((px - ox) / scale);
                                num[px] += w * value;
                                den[px] += w;
                            }
                        }
                    }
                }
            }
        }
    });
    for (std::size_t i = 0; i < numerator.size(); ++i)
        numerator[i] = denominator[i] > 1e-4f ? numerator[i] / denominator[i] : std::numeric_limits<float>::quiet_NaN();
    if (coverage) {
        // In frames: each one lands 0.5 green samples per sensor pixel, and
        // a cell spans (2 / scale)^2 sensor pixels. Smoothed so the swap
        // fades instead of switching per cell.
        *coverage = gaussianBlur(*coverage, cellsX, cellsY, 1.5);
        clipped = gaussianBlur(clipped, cellsX, cellsY, 2.0);
        const float perFrame = 0.5f * (2 / scale) * (2 / scale);
        for (std::size_t i = 0; i < coverage->size(); ++i) {
            const float clippedShare = clipped[i] / perFrame;
            (*coverage)[i] = (*coverage)[i] / perFrame * std::max(0.0f, 1.0f - 4.0f * clippedShare);
        }
    }
    return numerator;
}

// --- Lens shading -------------------------------------------------------------------

bool ShadingGrid::valid() const
{
    if (rows < 2 || cols < 2) return false;
    for (const std::vector<float> &p : phase)
        if (int(p.size()) != rows * cols) return false;
    return true;
}

float ShadingGrid::gain(int p, double x, double y) const
{
    const double gx = std::max(0.0, std::min(1.0, x)) * (cols - 1);
    const double gy = std::max(0.0, std::min(1.0, y)) * (rows - 1);
    const int x0 = std::min(cols - 2, int(gx)), y0 = std::min(rows - 2, int(gy));
    const float fx = float(gx - x0), fy = float(gy - y0);
    const std::vector<float> &g = phase[p];
    const float a = g[y0 * cols + x0], b = g[y0 * cols + x0 + 1];
    const float c = g[(y0 + 1) * cols + x0], d = g[(y0 + 1) * cols + x0 + 1];
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

// --- Demosaic -------------------------------------------------------------------------

std::vector<float> demosaic(const std::vector<float> &mosaic, int width, int height, int cfa)
{
    // Malvar, He and Cutler 2004: bilinear interpolation corrected by the
    // same-position Laplacian of the known channel. The correction is kept
    // within the range of the samples interpolated from: at hard edges,
    // above all clipped ones (a bright sky behind branches), it otherwise
    // overshoots into alternating dark and coloured dots along the edge.
    auto within = [](float value, float a, float b, float c, float d) {
        return std::max(std::min(std::min(a, b), std::min(c, d)), std::min(std::max(std::max(a, b), std::max(c, d)), value));
    };
    std::vector<float> rgb(std::size_t(width) * height * 3);
    parallelFor(0, height, [&](int y) {
        const bool border = y < 2 || y >= height - 2;
        auto v = [&](int xx, int yy) -> float {
            if (border || xx < 0 || xx >= width)
                return mosaic[std::size_t(reflect(yy, height)) * width + reflect(xx, width)];
            return mosaic[std::size_t(yy) * width + xx];
        };
        float *out = &rgb[std::size_t(y) * width * 3];
        for (int x = 0; x < width; ++x) {
            const int phase = (y & 1) * 2 + (x & 1);
            const int channel = phaseChannel(cfa, phase);
            const float c = v(x, y);
            const float n = v(x, y - 1), s = v(x, y + 1), w = v(x - 1, y), e = v(x + 1, y);
            const float nn = v(x, y - 2), ss = v(x, y + 2), ww = v(x - 2, y), ee = v(x + 2, y);
            const float nw = v(x - 1, y - 1), ne = v(x + 1, y - 1), sw = v(x - 1, y + 1), se = v(x + 1, y + 1);
            float r, g, b;
            if (channel == 1) {
                g = c;
                // Green with red/blue horizontal neighbours or vertical ones.
                const float horizontal = within((5 * c + 4 * (w + e) - (nw + ne + sw + se) - (ww + ee) + 0.5f * (nn + ss)) / 8,
                                                w, e, w, e);
                const float vertical = within((5 * c + 4 * (n + s) - (nw + ne + sw + se) - (nn + ss) + 0.5f * (ww + ee)) / 8,
                                              n, s, n, s);
                const int rowNeighbour = phaseChannel(cfa, (y & 1) * 2 + ((x + 1) & 1));
                if (rowNeighbour == 0) {
                    r = horizontal;
                    b = vertical;
                } else {
                    b = horizontal;
                    r = vertical;
                }
            } else {
                g = within((4 * c + 2 * (n + s + w + e) - (nn + ss + ww + ee)) / 8, n, s, w, e);
                const float diagonal = within((6 * c + 2 * (nw + ne + sw + se) - 1.5f * (nn + ss + ww + ee)) / 8,
                                              nw, ne, sw, se);
                if (channel == 0) {
                    r = c;
                    b = diagonal;
                } else {
                    b = c;
                    r = diagonal;
                }
            }
            // Signed: clamping noise at zero here would bias the noisiest
            // channel upwards and tint the shadows.
            out[x * 3] = r;
            out[x * 3 + 1] = g;
            out[x * 3 + 2] = b;
        }
    });
    return rgb;
}

// --- Tone mapping ------------------------------------------------------------------------

std::vector<float> fuseExposures(const std::vector<float> &luma, int width, int height,
                                 double shortGain, double longGain)
{
    // Mertens exposure fusion of two synthetic exposures of the same linear
    // luma: the short one keeps highlights, the long one lifts shadows. The
    // full-resolution level is evaluated on the fly to save memory.
    const Curve &c = curve();
    const float gs = float(shortGain), gl = float(longGain);
    struct Exposure {
        float s, l, w; // gamma-encoded short/long exposure, weight of short
    };
    // Weights depend only on the gamma-encoded exposures, so tabulate them.
    static const int Steps = 4096;
    std::vector<float> wellExposed(Steps + 1), takeover(Steps + 1), longWeight(Steps + 1);
    for (int i = 0; i <= Steps; ++i) {
        const float e = float(i) / Steps, d = e - 0.5f;
        wellExposed[i] = std::exp(-d * d * 12.5f);
        // The short exposure only takes over where the long one approaches
        // white, so shadows and mid tones keep the metered brightness.
        takeover[i] = 0.15f + 0.85f * std::min(1.0f, std::max(0.0f, (e - 0.5f) / 0.45f));
        // A clipped long exposure carries no information.
        longWeight[i] = wellExposed[i] * std::min(1.0f, std::max(0.0f, (1.0f - e) * 25.0f));
    }
    auto exposure = [&](float y) {
        Exposure e;
        e.s = c.toGamma(y * gs);
        e.l = c.toGamma(y * gl);
        const int is = int(e.s * Steps + 0.5f), il = int(e.l * Steps + 0.5f);
        const float ws = wellExposed[is] * takeover[il] + 1e-4f;
        const float wl = longWeight[il] + 1e-4f;
        e.w = ws / (ws + wl);
        return e;
    };
    auto lumaAt = [&](int x, int y) { return luma[std::size_t(y) * width + x]; };

    int levels = 1;
    while ((width >> levels) >= 16 && (height >> levels) >= 16 && levels < 10) ++levels;
    if (levels < 2) {
        std::vector<float> fused(luma.size());
        for (std::size_t i = 0; i < luma.size(); ++i) {
            const Exposure e = exposure(luma[i]);
            fused[i] = e.l + e.w * (e.s - e.l);
        }
        return fused;
    }

    // Gaussian pyramids from level 1 for the short, long and weight images.
    std::vector<Image> gS(levels), gL(levels), gW(levels);
    {
        // Separable first REDUCE of all three images: each pixel's exposure
        // is evaluated once per row instead of 25 times per image.
        static const float taps[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};
        const int outWidth = (width + 1) / 2, outHeight = (height + 1) / 2;
        std::vector<float> rowS(std::size_t(outWidth) * height), rowL(rowS.size()), rowW(rowS.size());
        parallelFor(0, height, [&](int y) {
            std::vector<Exposure> line(width);
            for (int x = 0; x < width; ++x) line[x] = exposure(lumaAt(x, y));
            for (int ox = 0; ox < outWidth; ++ox) {
                float vs = 0, vl = 0, vw = 0;
                for (int i = 0; i < 5; ++i) {
                    const Exposure &e = line[clampIndex(2 * ox + i - 2, width)];
                    vs += taps[i] * e.s;
                    vl += taps[i] * e.l;
                    vw += taps[i] * e.w;
                }
                const std::size_t o = std::size_t(y) * outWidth + ox;
                rowS[o] = vs;
                rowL[o] = vl;
                rowW[o] = vw;
            }
        });
        gS[1] = Image(outWidth, outHeight);
        gL[1] = Image(outWidth, outHeight);
        gW[1] = Image(outWidth, outHeight);
        parallelFor(0, outHeight, [&](int oy) {
            for (int ox = 0; ox < outWidth; ++ox) {
                float vs = 0, vl = 0, vw = 0;
                for (int j = 0; j < 5; ++j) {
                    const std::size_t o = std::size_t(clampIndex(2 * oy + j - 2, height)) * outWidth + ox;
                    vs += taps[j] * rowS[o];
                    vl += taps[j] * rowL[o];
                    vw += taps[j] * rowW[o];
                }
                const std::size_t i = std::size_t(oy) * outWidth + ox;
                gS[1].data[i] = vs;
                gL[1].data[i] = vl;
                gW[1].data[i] = vw;
            }
        });
    }
    for (int l = 2; l < levels; ++l) {
        gS[l] = reduce(gS[l - 1].width, gS[l - 1].height, [&](int x, int y) { return gS[l - 1].data[std::size_t(y) * gS[l - 1].width + x]; });
        gL[l] = reduce(gL[l - 1].width, gL[l - 1].height, [&](int x, int y) { return gL[l - 1].data[std::size_t(y) * gL[l - 1].width + x]; });
        gW[l] = reduce(gW[l - 1].width, gW[l - 1].height, [&](int x, int y) { return gW[l - 1].data[std::size_t(y) * gW[l - 1].width + x]; });
    }
    // Blend Laplacian levels with the weight pyramid and collapse, coarse to fine.
    Image result = gL[levels - 1];
    {
        const Image &s = gS[levels - 1], &w = gW[levels - 1];
        for (std::size_t i = 0; i < result.data.size(); ++i)
            result.data[i] += w.data[i] * (s.data[i] - result.data[i]);
    }
    for (int l = levels - 2; l >= 1; --l) {
        Image next(gS[l].width, gS[l].height);
        const Image &s = gS[l], &lo = gL[l], &w = gW[l];
        const Image &s1 = gS[l + 1], &l1 = gL[l + 1];
        parallelFor(0, next.height, [&](int y) {
            for (int x = 0; x < next.width; ++x) {
                const std::size_t i = std::size_t(y) * next.width + x;
                const float detailS = s.data[i] - expandAt(s1, x, y);
                const float detailL = lo.data[i] - expandAt(l1, x, y);
                next.data[i] = expandAt(result, x, y) + detailL + w.data[i] * (detailS - detailL);
            }
        });
        result = std::move(next);
        gS[l + 1] = Image();
        gL[l + 1] = Image();
        gW[l + 1] = Image();
    }
    std::vector<float> fused(std::size_t(width) * height);
    parallelFor(0, height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = std::size_t(y) * width + x;
            const Exposure e = exposure(luma[i]);
            const float detailS = e.s - expandAt(gS[1], x, y);
            const float detailL = e.l - expandAt(gL[1], x, y);
            fused[i] = std::max(0.0f, std::min(1.0f, expandAt(result, x, y) + detailL + e.w * (detailS - detailL)));
        }
    });
    return fused;
}

namespace {

// Separable Gaussian blur of a single-channel image.
std::vector<float> gaussianBlur(const std::vector<float> &source, int width, int height, double sigma)
{
    const int radius = std::max(1, int(std::ceil(sigma * 2.5)));
    std::vector<float> kernel(2 * radius + 1);
    double sum = 0;
    for (int i = -radius; i <= radius; ++i) sum += kernel[i + radius] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (float &k : kernel) k = float(k / sum);
    std::vector<float> temp(source.size()), out(source.size());
    parallelFor(0, height, [&](int y) {
        const float *row = &source[std::size_t(y) * width];
        float *dst = &temp[std::size_t(y) * width];
        for (int x = 0; x < width; ++x) {
            float v = 0;
            for (int i = -radius; i <= radius; ++i) v += kernel[i + radius] * row[reflect(x + i, width)];
            dst[x] = v;
        }
    });
    parallelFor(0, height, [&](int y) {
        float *dst = &out[std::size_t(y) * width];
        for (int x = 0; x < width; ++x) dst[x] = 0;
        for (int i = -radius; i <= radius; ++i) {
            const float k = kernel[i + radius];
            const float *row = &temp[std::size_t(reflect(y + i, height)) * width];
            for (int x = 0; x < width; ++x) dst[x] += k * row[x];
        }
    });
    return out;
}

// Edge-preserving chroma smoothing: a separable cross bilateral whose range
// kernel is scaled to the chroma noise measured at each brightness, so colour
// edges well above the noise (red against green, often equal in luma) survive.
void chromaBilateral(std::vector<float> *cr, std::vector<float> *cb, const std::vector<float> &guide,
                     int width, int height, double sigma)
{
    const int bins = 20;
    auto binOf = [](float y) {
        const float t = (std::log2(std::max(y, 1e-5f)) + 16.0f) / 17.0f; // 2^-16 .. 2^1
        return std::max(0, std::min(bins - 1, int(t * bins)));
    };
    std::vector<std::vector<float>> residuals(bins);
    for (int y = 1; y < height - 1; y += 2) {
        for (int x = 1; x < width - 1; x += 2) {
            const std::size_t i = std::size_t(y) * width + x;
            const float rr = (*cr)[i] - 0.25f * ((*cr)[i - 1] + (*cr)[i + 1] + (*cr)[i - width] + (*cr)[i + width]);
            const float rb = (*cb)[i] - 0.25f * ((*cb)[i - 1] + (*cb)[i + 1] + (*cb)[i - width] + (*cb)[i + width]);
            residuals[binOf(guide[i])].push_back(std::sqrt(0.5f * (rr * rr + rb * rb)));
        }
    }
    // Median absolute residual -> noise sigma (robust against sparse edges).
    std::vector<float> noise(bins, -1.0f);
    for (int b = 0; b < bins; ++b) {
        std::vector<float> &values = residuals[b];
        if (values.size() < 32) continue;
        std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
        noise[b] = values[values.size() / 2] * 1.4826f / std::sqrt(1.25f) * 1.2f;
    }
    float last = -1.0f;
    for (int b = 0; b < bins; ++b) if (noise[b] >= 0) { last = noise[b]; break; }
    for (int b = 0; b < bins; ++b) {
        if (noise[b] < 0) noise[b] = last;
        // A large fully clipped area (window, sky) has exactly zero chroma
        // residual; a zero sigma would make every weight 0 * inf = NaN.
        noise[b] = std::max(noise[b], 1e-6f);
        last = noise[b];
    }
    // exp(-t) table for t in [0, 8].
    float table[257];
    for (int i = 0; i <= 256; ++i) table[i] = float(std::exp(-i / 32.0));
    const int radius = std::max(1, int(std::ceil(sigma * 2.5)));
    std::vector<float> spatial(2 * radius + 1);
    for (int i = -radius; i <= radius; ++i) spatial[i + radius] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    // Range sigma 2.5x the local noise: noise-level differences merge, edges do not.
    std::vector<float> range(std::size_t(width) * height);
    for (std::size_t i = 0; i < range.size(); ++i) {
        const float s = 2.5f * noise[binOf(guide[i])];
        range[i] = 1.0f / (2.0f * s * s);
    }
    std::vector<float> outR(cr->size()), outB(cb->size());
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<float> &inR = *cr, &inB = *cb;
        parallelFor(0, height, [&](int y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(y) * width + x;
                const float r0 = inR[i], b0 = inB[i], k = range[i];
                float sumW = 0, sumR = 0, sumB = 0;
                for (int t = -radius; t <= radius; ++t) {
                    const std::size_t j = pass == 0 ? std::size_t(y) * width + reflect(x + t, width)
                                                    : std::size_t(reflect(y + t, height)) * width + x;
                    const float dr = inR[j] - r0, db = inB[j] - b0;
                    const float e = (dr * dr + db * db) * k * 32.0f;
                    const float w = spatial[t + radius] * (e < 256.0f ? table[int(e)] : 0.0f);
                    sumW += w;
                    sumR += w * inR[j];
                    sumB += w * inB[j];
                }
                outR[i] = sumW > 0 ? sumR / sumW : r0;
                outB[i] = sumW > 0 ? sumB / sumW : b0;
            }
        });
        cr->swap(outR);
        cb->swap(outB);
    }
}

// Rebuilds sensor-clipped channels of partly clipped 2x2 quads from the
// channels that survived, using the colour ratios of nearby bright, unclipped
// pixels (skin next to a blown forehead, sky next to a bright cloud). A
// coarse grid of channel means is filled by diffusion where it has no data.
class HighlightRatios
{
public:
    HighlightRatios(const Planes &planes, int cfa) : m_planes(planes), m_threshold(planes.clip * 0.97f)
    {
        for (int p = 0; p < 4; ++p) m_channel[p] = phaseChannel(cfa, p);
        m_cols = std::max(1, (planes.width + Cell - 1) / Cell);
        m_rows = std::max(1, (planes.height + Cell - 1) / Cell);
        std::vector<double> sums(std::size_t(m_cols) * m_rows * 4, 0.0);
        std::vector<int> clippedQuads(std::size_t(m_cols) * m_rows, 0), fullQuads(clippedQuads.size(), 0);
        bool anyClipped = false;
        for (int y = 0; y < planes.height; ++y) {
            for (int x = 0; x < planes.width; ++x) {
                float rgb[3];
                int clippedCount;
                quad(x, y, rgb, &clippedCount, nullptr);
                anyClipped = anyClipped || clippedCount > 0;
                const std::size_t cellIndex = std::size_t(y / Cell) * m_cols + x / Cell;
                if (clippedCount) ++clippedQuads[cellIndex];
                if (clippedCount == 3) ++fullQuads[cellIndex];
                const float peak = std::max(rgb[0], std::max(rgb[1], rgb[2]));
                // Bright, unclipped quads (the same object just below the
                // clip level, such as skin around a blown forehead) carry
                // the colour of the highlights.
                if (clippedCount || peak < planes.clip * 0.5f) continue;
                double *cell = &sums[(std::size_t(y / Cell) * m_cols + x / Cell) * 4];
                for (int c = 0; c < 3; ++c) cell[c] += rgb[c];
                cell[3] += 1;
            }
        }
        m_active = anyClipped;
        if (!m_active) return;
        m_means.assign(std::size_t(m_cols) * m_rows * 3, 0.0f);
        std::vector<bool> known(std::size_t(m_cols) * m_rows, false);
        bool anyKnown = false;
        for (std::size_t i = 0; i < known.size(); ++i) {
            if (sums[i * 4 + 3] >= 4) {
                for (int c = 0; c < 3; ++c) m_means[i * 3 + c] = float(sums[i * 4 + c] / sums[i * 4 + 3]);
                known[i] = anyKnown = true;
            }
        }
        if (!anyKnown) {
            m_active = false;
            return;
        }
        // Fill cells without data from their neighbours, remembering how far
        // each is from real samples.
        m_distance.assign(known.size(), 0);
        std::vector<bool> filled = known;
        for (int iteration = 0; iteration < m_cols + m_rows; ++iteration) {
            bool changed = false;
            std::vector<bool> next = filled;
            for (int r = 0; r < m_rows; ++r) {
                for (int c = 0; c < m_cols; ++c) {
                    const std::size_t i = std::size_t(r) * m_cols + c;
                    if (filled[i]) continue;
                    float sum[3] = {0, 0, 0};
                    int count = 0;
                    for (int dr = -1; dr <= 1; ++dr) {
                        for (int dc = -1; dc <= 1; ++dc) {
                            const int rr = r + dr, cc = c + dc;
                            if (rr < 0 || cc < 0 || rr >= m_rows || cc >= m_cols) continue;
                            const std::size_t j = std::size_t(rr) * m_cols + cc;
                            if (!filled[j]) continue;
                            for (int k = 0; k < 3; ++k) sum[k] += m_means[j * 3 + k];
                            ++count;
                        }
                    }
                    if (!count) continue;
                    for (int k = 0; k < 3; ++k) m_means[i * 3 + k] = sum[k] / count;
                    m_distance[i] = iteration + 1;
                    next[i] = changed = true;
                }
            }
            filled.swap(next);
            if (!changed) break;
        }
        // Trust per cell: near real colour samples, and not where most of
        // the highlight is saturated in every channel (a blown sky or lamp
        // has no colour to recover; mixing rebuilt and white quads there
        // only produces speckle).
        m_trust.assign(known.size(), 0.0f);
        for (std::size_t i = 0; i < known.size(); ++i) {
            const float near = std::max(0.0f, std::min(1.0f, (4.0f - m_distance[i]) / 3.0f));
            const float full = clippedQuads[i] ? float(fullQuads[i]) / clippedQuads[i] : 0.0f;
            m_trust[i] = near * std::max(0.0f, std::min(1.0f, (0.3f - full) / 0.2f));
        }
    }

    // For a quad with sensor-clipped channels, writes rebuilt phase values
    // to `rebuilt` and returns how much to trust them (0 when nothing is
    // clipped, everything is clipped, or no real colour sample is near).
    float reconstruct(int x, int y, const float *values, float *rebuilt, bool *anyClipped) const
    {
        // Defined on every path: callers blend with trust 0 too.
        for (int p = 0; p < 4; ++p) rebuilt[p] = values[p];
        float rgb[3];
        int clippedCount;
        bool clipped[3];
        quad(x, y, rgb, &clippedCount, clipped);
        *anyClipped = clippedCount > 0;
        // Only with a single surviving channel: two survivors already fix the
        // colour well (the brightness lift handles it), and borrowing the
        // colour of a differently tinted neighbour (blue sky around a white
        // cloud) would only tint it.
        if (!m_active || clippedCount != 2) return 0.0f;
        const float trust = sample(m_trust, 1, x, y, 0);
        if (trust <= 0) return 0.0f;
        // Least noisy surviving channel: green, then the brighter of red/blue.
        const int reference = !clipped[1] ? 1 : (!clipped[0] && (clipped[2] || rgb[0] >= rgb[2]) ? 0 : 2);
        float mean[3];
        meansAt(x, y, mean);
        if (!(mean[reference] > 0)) return 0.0f;
        float channel[3];
        float consistency = 1.0f;
        for (int c = 0; c < 3; ++c) {
            if (!clipped[c]) {
                channel[c] = rgb[c];
                continue;
            }
            const float estimate = rgb[reference] * mean[c] / mean[reference];
            // A clipped channel is truly at least what was recorded; an
            // estimate below that means the nearby colour does not fit here.
            consistency = std::min(consistency, std::max(0.0f, std::min(1.0f, (estimate / rgb[c] - 0.9f) / 0.2f)));
            channel[c] = std::max(rgb[c], estimate);
        }
        for (int p = 0; p < 4; ++p) {
            const int c = m_channel[p];
            rebuilt[p] = clipped[c] ? std::max(values[p], channel[c]) : values[p];
        }
        return trust * consistency;
    }

private:
    static const int Cell = 32;

    void quad(int x, int y, float *rgb, int *clippedCount, bool *clippedOut) const
    {
        const std::size_t i = std::size_t(y) * m_planes.width + x;
        float sum[3] = {0, 0, 0};
        int count[3] = {0, 0, 0};
        bool clipped[3] = {false, false, false};
        for (int p = 0; p < 4; ++p) {
            const float v = m_planes.phase[p][i];
            const int c = m_channel[p];
            sum[c] += v;
            ++count[c];
            clipped[c] = clipped[c] || v >= m_threshold;
        }
        *clippedCount = 0;
        for (int c = 0; c < 3; ++c) {
            rgb[c] = count[c] ? sum[c] / count[c] : 0.0f;
            *clippedCount += clipped[c];
            if (clippedOut) clippedOut[c] = clipped[c];
        }
    }

    // Bilinear sample of a per-cell grid at quad (x, y).
    float sample(const std::vector<float> &grid, int stride, int x, int y, int offset) const
    {
        const float gx = std::max(0.0f, (x + 0.5f) / Cell - 0.5f), gy = std::max(0.0f, (y + 0.5f) / Cell - 0.5f);
        const int x0 = std::min(m_cols - 1, int(gx)), y0 = std::min(m_rows - 1, int(gy));
        const int x1 = std::min(m_cols - 1, x0 + 1), y1 = std::min(m_rows - 1, y0 + 1);
        const float fx = std::min(1.0f, gx - x0), fy = std::min(1.0f, gy - y0);
        const float a = grid[(std::size_t(y0) * m_cols + x0) * stride + offset];
        const float b = grid[(std::size_t(y0) * m_cols + x1) * stride + offset];
        const float d = grid[(std::size_t(y1) * m_cols + x0) * stride + offset];
        const float e = grid[(std::size_t(y1) * m_cols + x1) * stride + offset];
        return (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
    }

    void meansAt(int x, int y, float *mean) const
    {
        for (int c = 0; c < 3; ++c) mean[c] = sample(m_means, 3, x, y, c);
    }

    const Planes &m_planes;
    float m_threshold;
    int m_channel[4];
    int m_cols = 1, m_rows = 1;
    bool m_active = false;
    std::vector<float> m_means;
    std::vector<int> m_distance;
    std::vector<float> m_trust;
};

double percentile(std::vector<float> values, double fraction)
{
    if (values.empty()) return 0;
    const std::size_t k = std::min(values.size() - 1, std::size_t(fraction * values.size()));
    std::nth_element(values.begin(), values.begin() + k, values.end());
    return values[k];
}

} // namespace

namespace {

// Crop of interleaved camera RGB upscaled to the zoom's output size:
// bilinear colour, with the brightness refined by the RAISR filters in a
// square-root domain (where they were trained, close to display gamma).
std::vector<float> upscaleCrop(const std::vector<float> &rgb, int width, const ZoomRegion &zoom,
                               const Raisr::Filters *filters)
{
    const int outWidth = zoom.outputWidth(), outHeight = zoom.outputHeight();
    Raisr::Image channel[3], tone;
    for (Raisr::Image *image : {&channel[0], &channel[1], &channel[2], &tone}) {
        image->width = zoom.width;
        image->height = zoom.height;
        image->data.resize(std::size_t(zoom.width) * zoom.height);
    }
    parallelFor(0, zoom.height, [&](int y) {
        for (int x = 0; x < zoom.width; ++x) {
            const float *p = &rgb[(std::size_t(zoom.y + y) * width + zoom.x + x) * 3];
            const std::size_t i = std::size_t(y) * zoom.width + x;
            for (int k = 0; k < 3; ++k) channel[k].data[i] = p[k];
            const float luma = 0.25f * (p[0] + 2 * p[1] + p[2]);
            tone.data[i] = std::copysign(std::sqrt(std::abs(luma)), luma);
        }
    });
    Raisr::Image up[3];
    for (int k = 0; k < 3; ++k) up[k] = Raisr::resize(channel[k], outWidth, outHeight);
    std::vector<float> out(std::size_t(outWidth) * outHeight * 3);
    const bool learned = filters && filters->valid();
    Raisr::Image coarse, sharp;
    if (learned) {
        coarse = Raisr::resize(tone, outWidth, outHeight);
        sharp = Raisr::upscale(tone, outWidth, outHeight, *filters);
    }
    // Below 2x the filters (trained for 2x) would oversharpen.
    const float strength = float(std::max(0.0, std::min(1.0, zoom.scale - 1.0)));
    parallelFor(0, outHeight, [&](int y) {
        for (int x = 0; x < outWidth; ++x) {
            const std::size_t i = std::size_t(y) * outWidth + x;
            float delta = 0;
            if (learned) {
                const float s = sharp.data[i], c = coarse.data[i];
                delta = strength * (s * std::abs(s) - c * std::abs(c));
            }
            for (int k = 0; k < 3; ++k) out[i * 3 + k] = up[k].data[i] + delta;
        }
    });
    return out;
}

}

// Black and white points of an output, from its display luma (0..1):
// the darkest 0.2% go down to blackTarget when the image has no real blacks
// (shadows only, fading out by mid-grey); the brightest 0.5% are stretched
// towards 0.9 when they stop short, in the upper tones only.
struct ToneEnds {
    float blackPoint = 0, whiteScale = 1, whiteReference = 1, low = 0, high = 0;
    static constexpr float blackTarget = 0.02f;

    float operator()(float v) const
    {
        if (blackPoint > 0) {
            if (v < blackPoint) {
                v *= blackTarget / blackPoint;
            } else if (v < 0.5f) {
                const float w = (0.5f - v) / (0.5f - blackPoint);
                v -= (blackPoint - blackTarget) * w * w;
            }
        }
        if (whiteScale > 1) {
            float t = std::max(0.0f, std::min(1.0f, (v - 0.4f) / std::max(0.05f, whiteReference - 0.4f)));
            t = t * t * (3 - 2 * t);
            v += (v * whiteScale - v) * t;
            if (v > 0.94f) v = 0.94f + 0.06f * (1 - std::exp(-(v - 0.94f) / 0.06f));
        }
        return v;
    }
};

static ToneEnds measureToneEnds(std::vector<float> &sample, bool black, bool white)
{
    ToneEnds ends;
    if (sample.empty()) return ends;
    ends.low = float(percentile(sample, 0.002));
    ends.high = float(percentile(sample, 0.995));
    if (black && ends.low > 0.03f && ends.low < 0.25f) ends.blackPoint = ends.low;
    if (white && ends.high > 0.45f && ends.high < 0.86f) {
        ends.whiteScale = std::min(1.12f, 0.9f / ends.high);
        ends.whiteReference = ends.high;
    }
    return ends;
}

// Rows (landscape) or columns (portrait) of an image inside the centred
// share `kept` of its short side, as a [first, last) range.
static void keptRange(int width, int height, double kept, int *first, int *last)
{
    const int shortSide = width >= height ? height : width;
    *first = int(shortSide * (1 - std::max(0.1, std::min(1.0, kept))) / 2);
    *last = shortSide - *first;
}

// Clarity: local contrast at a medium scale (5 px) in the mid-tones of the
// display luma `y`, never in deep shadows or highlights; apply(i, delta)
// receives the change for pixel i (y itself is updated). Noise-aware: most
// of an image is flat, so the median deviation from the 5 px blur measures
// the noise at that scale; only what exceeds 1.5x that is boosted (flat
// walls otherwise got a third grainier).
template <typename Apply>
static void applyClarity(std::vector<float> &y, int width, int height, float amount, Apply apply)
{
    const std::vector<float> wide = gaussianBlur(y, width, height, 5.0);
    std::vector<float> deviation;
    deviation.reserve(y.size() / 7 + 1);
    for (std::size_t i = 0; i < y.size(); i += 7)
        if (y[i] > 0.1f && y[i] < 0.9f) deviation.push_back(std::abs(y[i] - wide[i]));
    const float floor = deviation.empty() ? 0.0f : 1.5f * 1.4826f * float(percentile(deviation, 0.5));
    parallelFor(0, height, [&](int row) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = std::size_t(row) * width + x;
            const float f = y[i];
            float lowSide = std::max(0.0f, std::min(1.0f, (f - 0.04f) / 0.12f));
            float highSide = std::max(0.0f, std::min(1.0f, (0.95f - f) / 0.15f));
            lowSide = lowSide * lowSide * (3 - 2 * lowSide);
            highSide = highSide * highSide * (3 - 2 * highSide);
            const float d = f - wide[i];
            const float excess = std::max(0.0f, std::abs(d) - floor);
            const float delta = std::max(-0.06f, std::min(0.06f, amount * lowSide * highSide * (d < 0 ? -excess : excess)));
            y[i] = f + delta;
            apply(i, delta);
        }
    });
}

// Mean linear level of the central part of the largest face (a face box also
// holds hair and background) in an image whose pixel (x, y) shows sensor
// position origin + (x, y) * step; -1 without a face of at least 0.3% of the
// image.
static double largestFaceLevel(const std::vector<float> &luma, int width, int height, const FaceRects &faces,
                               double originX, double originY, double step, int sensorWidth, int sensorHeight)
{
    double level = -1, bestArea = 0;
    for (const std::array<float, 4> &face : faces) {
        auto toX = [&](float v) { return std::max(0, std::min(width, int((v * sensorWidth - originX) / step))); };
        auto toY = [&](float v) { return std::max(0, std::min(height, int((v * sensorHeight - originY) / step))); };
        int x0 = toX(face[0]), x1 = toX(face[2]), y0 = toY(face[1]), y1 = toY(face[3]);
        const double area = double(x1 - x0) * (y1 - y0);
        if (area < 0.003 * width * height || area <= bestArea) continue;
        const int insetX = (x1 - x0) / 5, insetY = (y1 - y0) / 5;
        x0 += insetX; x1 -= insetX; y0 += insetY; y1 -= insetY;
        const int stride = std::max(1, int(std::sqrt(area / 4000)));
        double sum = 0;
        long count = 0;
        for (int y = y0; y < y1; y += stride)
            for (int x = x0; x < x1; x += stride, ++count) sum += std::max(0.0f, luma[std::size_t(y) * width + x]);
        if (count == 0) continue;
        level = sum / count;
        bestArea = area;
    }
    return level;
}

// Where a face should land after tone mapping (linear): from a darker skin
// in shade to a lighter one in good light (sRGB about 100 to 187).
static const double FaceLow = 0.13, FaceHigh = 0.5;

// fuseExposures() for one flat area of linear level y (gamma-encoded result),
// with the same weights; the pyramid only matters at edges.
static double fusedLevel(double y, double shortGain, double longGain)
{
    const Curve &c = curve();
    const double s = c.toGamma(float(std::min(1.0, y * shortGain)));
    const double l = c.toGamma(float(std::min(1.0, y * longGain)));
    auto wellExposed = [](double e) { return std::exp(-(e - 0.5) * (e - 0.5) * 12.5); };
    const double takeover = 0.15 + 0.85 * std::min(1.0, std::max(0.0, (l - 0.5) / 0.45));
    const double ws = wellExposed(s) * takeover + 1e-4;
    const double wl = wellExposed(l) * std::min(1.0, std::max(0.0, (1.0 - l) * 25.0)) + 1e-4;
    return l + ws / (ws + wl) * (s - l);
}

// Short and long gains closest to the given ones (in exposure stops; moving
// the short one, which sets the highlights, counts double) that put a face
// of linear level `face` within FaceLow..FaceHigh after fusion.
static void fitFace(double face, double *shortGain, double *longGain, double maxLong)
{
    const Curve &c = curve();
    const double low = c.toGamma(float(FaceLow)), high = c.toGamma(float(FaceHigh));
    const double s0 = *shortGain, l0 = *longGain;
    double best = 1e9;
    for (int i = 0; i <= 12; ++i) {
        const double s = s0 * std::pow(2.0, -0.25 * i); // up to 3 stops darker
        for (int j = 0; j <= 32; ++j) {
            const double l = std::max(s, std::min(maxLong, s * std::pow(2.0, 0.125 * j)));
            const double f = fusedLevel(face, s, l);
            const double miss = f < low ? low - f : f > high ? f - high : 0.0;
            const double cost = 100 * miss + std::abs(std::log2(l / l0)) + 2 * std::abs(std::log2(s / s0));
            if (cost < best) {
                best = cost;
                *shortGain = s;
                *longGain = l;
            }
        }
    }
}

// Super-resolved detail is swapped in only where it exceeds this many times
// the measured difference noise (see the detail swap in finish()).
static const float DetailCoring = 2.0f;

RgbImage finish(const Planes &merged, const FinishOptions &options, FinishStats *stats)
{
    const int sensorWidth = merged.width * 2, sensorHeight = merged.height * 2;
    int width = sensorWidth, height = sensorHeight;
    const Curve &c = curve();
    RgbImage output;
    output.width = width;
    output.height = height;
    if (width < 4 || height < 4) return output;

    auto stageStart = std::chrono::steady_clock::now();
    auto lap = [&](int stage) {
        const auto now = std::chrono::steady_clock::now();
        if (stats) stats->stageMs[stage] = std::chrono::duration<double, std::milli>(now - stageStart).count();
        stageStart = now;
    };

    // 1. Highlight reconstruction, white balance, lens shading, re-mosaic.
    double gains[4];
    double minGain = 1e9;
    for (int p = 0; p < 4; ++p) {
        const int channel = phaseChannel(options.cfa, p);
        gains[p] = channel == 0 ? options.wbGains[0] : channel == 2 ? options.wbGains[3]
                : (p >> 1) ? options.wbGains[2] : options.wbGains[1];
        if (!(gains[p] > 0) || !std::isfinite(gains[p])) gains[p] = 1;
        minGain = std::min(minGain, gains[p]);
    }
    for (double &g : gains) g /= minGain;
    const float clip = merged.clip;
    const HighlightRatios ratios(merged, options.cfa);
    const bool shading = options.shading && options.shading->valid();
    std::vector<float> mosaic(std::size_t(width) * height);
    // How much of each quad's colour is guessed (0 measured, 1 unknown).
    std::vector<float> guess(std::size_t(merged.width) * merged.height, 0.0f);
    parallelFor(0, merged.height, [&](int y) {
        for (int x = 0; x < merged.width; ++x) {
            const std::size_t i = std::size_t(y) * merged.width + x;
            float values[4], rebuilt[4];
            for (int p = 0; p < 4; ++p) values[p] = merged.phase[p][i];
            bool clipped = false;
            const float trust = ratios.reconstruct(x, y, values, rebuilt, &clipped);
            // After white balance a clipped channel is estimated as the mean
            // of the surviving colour channels (at least what was recorded):
            // near-grey highlights such as clouds stay neutral instead of
            // turning magenta or green, and the result is continuous. With
            // nothing surviving, every channel takes the brightest level.
            // White balance and lens shading first, so a neutral fill stays
            // neutral (shading gains differ slightly per channel).
            float balancedValues[4];
            for (int p = 0; p < 4; ++p) {
                float v = values[p] * float(gains[p]);
                if (shading)
                    v *= options.shading->gain(p, double(2 * x + (p & 1)) / (width - 1),
                                               double(2 * y + (p >> 1)) / (height - 1));
                balancedValues[p] = v;
            }
            float channelSum[3] = {0, 0, 0}, maximum = 0;
            int channelCount[3] = {0, 0, 0};
            for (int p = 0; p < 4; ++p) {
                const float balanced = balancedValues[p];
                maximum = std::max(maximum, balanced);
                if (values[p] < clip * 0.97f) {
                    channelSum[phaseChannel(options.cfa, p)] += balanced;
                    ++channelCount[phaseChannel(options.cfa, p)];
                }
            }
            float lift = 0;
            int survivors = 0;
            for (int c = 0; c < 3; ++c) {
                if (!channelCount[c]) continue;
                lift += channelSum[c] / channelCount[c];
                ++survivors;
            }
            lift = survivors ? lift / survivors : maximum;
            if (clipped) {
                // One clipped channel: the survivors' mean is a fair estimate.
                // One survivor: as good as the rebuild. None: no colour.
                guess[i] = survivors >= 2 ? 0.25f : survivors == 1 ? 1.0f - trust : 1.0f;
            }
            for (int p = 0; p < 4; ++p) {
                const int mx = 2 * x + (p & 1), my = 2 * y + (p >> 1);
                float v = balancedValues[p];
                if (clipped && values[p] >= clip * 0.97f) {
                    const float base = std::max(v, lift);
                    // Colour rebuilt from nearby unclipped pixels, where trusted.
                    const float shade = values[p] > 0 ? v / (values[p] * float(gains[p])) : 1.0f;
                    v = base + trust * (std::max(base, rebuilt[p] * float(gains[p]) * shade) - base);
                }
                mosaic[std::size_t(my) * width + mx] = v;
            }
        }
    });

    lap(0);

    // 2. Demosaic and colour matrix to linear sRGB.
    std::vector<float> rgb = demosaic(mosaic, width, height, options.cfa);
    std::vector<float>().swap(mosaic);
    bool anyGuess = false;
    for (float g : guess) if (g > 0) { anyGuess = true; break; }
    if (anyGuess) guess = gaussianBlur(guess, merged.width, merged.height, 4.0);
    else std::vector<float>().swap(guess);
    int guessWidth = merged.width, guessHeight = merged.height;
    // Output pixel -> normalised sensor position (lens shading lookups).
    double originX = 0, originY = 0, pixelStep = 1;

    // Zoom: only the crop goes on, upscaled before the super-resolved detail
    // (which has the output resolution) is swapped in.
    const ZoomRegion &zoom = options.zoom;
    if (zoom.active() && zoom.x >= 0 && zoom.y >= 0 && zoom.x + zoom.width <= width
            && zoom.y + zoom.height <= height) {
        const int outWidth = zoom.outputWidth(), outHeight = zoom.outputHeight();
        rgb = upscaleCrop(rgb, width, zoom, options.upscaleFilters);
        if (!guess.empty()) {
            Raisr::Image crop;
            crop.width = zoom.width / 2;
            crop.height = zoom.height / 2;
            crop.data.resize(std::size_t(crop.width) * crop.height);
            for (int y = 0; y < crop.height; ++y)
                for (int x = 0; x < crop.width; ++x)
                    crop.data[std::size_t(y) * crop.width + x] = guess[std::size_t(zoom.y / 2 + y) * merged.width + zoom.x / 2 + x];
            guessWidth = (outWidth + 1) / 2;
            guessHeight = (outHeight + 1) / 2;
            guess = Raisr::resize(crop, guessWidth, guessHeight).data;
        }
        originX = zoom.x + 0.5 / zoom.scale - 0.5;
        originY = zoom.y + 0.5 / zoom.scale - 0.5;
        pixelStep = 1.0 / zoom.scale;
        width = outWidth;
        height = outHeight;
        output.width = width;
        output.height = height;
    }
    auto guessAt = [&](int x, int y) {
        return guess[std::size_t(std::min(guessHeight - 1, y >> 1)) * guessWidth + std::min(guessWidth - 1, x >> 1)];
    };

    // Super-resolution: fine detail of the super-resolved green (real samples
    // from the shifted frames) replaces the demosaicked fine detail; coarse
    // structure and colour stay from the robust merge. Not in highlights,
    // where colour was rebuilt.
    if (options.detailGreen && options.detailStrength > 0
            && options.detailGreen->size() == std::size_t(width) * height) {
        std::vector<float> &detail = *options.detailGreen;
        int greenPhases[2], count = 0;
        for (int p = 0; p < 4 && count < 2; ++p)
            if (phaseChannel(options.cfa, p) == 1) greenPhases[count++] = p;
        const float greenGain = float(0.5 * (gains[greenPhases[0]] + gains[greenPhases[1]]));
        std::vector<float> demosaicked(std::size_t(width) * height);
        parallelFor(0, height, [&](int y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(y) * width + x;
                demosaicked[i] = rgb[i * 3 + 1];
                float v = detail[i];
                if (!std::isfinite(v)) {
                    detail[i] = demosaicked[i]; // nothing landed here: no detail change
                    continue;
                }
                v *= greenGain;
                if (shading) {
                    const double u = std::max(0.0, std::min(1.0, (originX + x * pixelStep) / (sensorWidth - 1)));
                    const double w = std::max(0.0, std::min(1.0, (originY + y * pixelStep) / (sensorHeight - 1)));
                    v *= 0.5f * (options.shading->gain(greenPhases[0], u, w) + options.shading->gain(greenPhases[1], u, w));
                }
                detail[i] = v;
            }
        });
        const std::vector<float> detailLow = gaussianBlur(detail, width, height, 1.0);
        const std::vector<float> demosaickedLow = gaussianBlur(demosaicked, width, height, 1.0);
        const float strength = float(std::min(1.0, options.detailStrength));
        // Noise-aware swap: the super-resolved estimate rests on fewer
        // samples per pixel than the merge, so where the two fine details
        // differ by no more than noise (flat walls, sky) the difference is
        // grain, not detail. Per brightness band, the median absolute
        // difference measures that noise; only what exceeds a multiple of it
        // is swapped in (soft threshold), so real edges stay crisp.
        const int bands = 16;
        auto bandOf = [](float v) {
            const float t = (std::log2(std::max(v, 1.0f / 4096)) + 12.0f) / 13.0f;
            return std::max(0, std::min(bands - 1, int(t * bands)));
        };
        float coring[bands];
        {
            std::vector<std::vector<float>> samples(bands);
            for (int y = 0; y < height; y += 3)
                for (int x = (y / 3) % 3; x < width; x += 3) {
                    const std::size_t i = std::size_t(y) * width + x;
                    const float d = (detail[i] - detailLow[i]) - (demosaicked[i] - demosaickedLow[i]);
                    samples[bandOf(demosaickedLow[i])].push_back(std::abs(d));
                }
            float previous = 0;
            for (int b = 0; b < bands; ++b) {
                std::vector<float> &v = samples[b];
                float sigma = previous;
                if (v.size() >= 64) {
                    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                    sigma = 1.4826f * v[v.size() / 2];
                }
                coring[b] = DetailCoring * sigma;
                previous = sigma;
            }
        }
        const int coverageCells = (width + 1) / 2, coverageRows = (height + 1) / 2;
        const bool coverageOk = options.detailCoverage
                && options.detailCoverage->size() == std::size_t(coverageCells) * coverageRows;
        parallelFor(0, height, [&](int y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(y) * width + x;
                float weight = strength;
                if (!guess.empty())
                    weight *= 1.0f - std::min(1.0f, guessAt(x, y));
                if (coverageOk)
                    weight *= std::min(1.0f, 0.5f * (*options.detailCoverage)[
                            std::size_t(std::min(coverageRows - 1, y >> 1)) * coverageCells + std::min(coverageCells - 1, x >> 1)]);
                if (weight <= 0) continue;
                const float difference = (detail[i] - detailLow[i]) - (demosaicked[i] - demosaickedLow[i]);
                const float excess = std::abs(difference) - coring[bandOf(demosaickedLow[i])];
                if (excess <= 0) continue;
                const float delta = weight * (difference < 0 ? -excess : excess);
                for (int k = 0; k < 3; ++k) rgb[i * 3 + k] += delta;
            }
        });
    }
    const std::array<double, 9> &m = options.colorMatrix;
    const std::size_t pixels = std::size_t(width) * height;
    std::vector<float> luma(pixels);
    parallelFor(0, height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = std::size_t(y) * width + x;
            float *p = &rgb[i * 3];
            const float r = p[0], g = p[1], b = p[2];
            for (int k = 0; k < 3; ++k) p[k] = float(m[k * 3] * r + m[k * 3 + 1] * g + m[k * 3 + 2] * b);
            luma[i] = float(Luma[0] * p[0] + Luma[1] * p[1] + Luma[2] * p[2]);
            // Guessed highlight colour (blown cloud cores, lamps) fades to neutral.
            if (!guess.empty()) {
                const float t = std::min(1.0f, guessAt(x, y));
                if (t > 0) for (int k = 0; k < 3; ++k) p[k] = luma[i] + (p[k] - luma[i]) * (1 - t);
            }
        }
    });

    lap(1);

    // 3. Chroma denoise: blur colour differences at half resolution.
    if (options.chromaDenoise > 0) {
        const int hw = width / 2, hh = height / 2;
        std::vector<float> cr(std::size_t(hw) * hh), cb(std::size_t(hw) * hh), yh(std::size_t(hw) * hh);
        parallelFor(0, hh, [&](int y) {
            for (int x = 0; x < hw; ++x) {
                float sr = 0, sb = 0, sy = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const std::size_t i = std::size_t(2 * y + dy) * width + 2 * x + dx;
                        sr += rgb[i * 3] - luma[i];
                        sb += rgb[i * 3 + 2] - luma[i];
                        sy += luma[i];
                    }
                }
                cr[std::size_t(y) * hw + x] = sr * 0.25f;
                cb[std::size_t(y) * hw + x] = sb * 0.25f;
                yh[std::size_t(y) * hw + x] = sy * 0.25f;
            }
        });
        const double sigma = std::max(1.0, std::min(6.0, (1.0 + 30.0 * options.relativeNoise) * options.chromaDenoise));
        chromaBilateral(&cr, &cb, yh, hw, hh, sigma);
        Image crImage, cbImage;
        crImage.width = cbImage.width = hw;
        crImage.height = cbImage.height = hh;
        crImage.data.swap(cr);
        cbImage.data.swap(cb);
        parallelFor(0, height, [&](int y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(y) * width + x;
                // Half-resolution sample centres sit between full-resolution pixels.
                const float fx = std::max(0.0f, (x - 0.5f) * 0.5f), fy = std::max(0.0f, (y - 0.5f) * 0.5f);
                const int x0 = int(fx), y0 = int(fy);
                const float ax = fx - x0, ay = fy - y0;
                auto sample = [&](const Image &image) {
                    const float a = image.at(x0, y0), b = image.at(x0 + 1, y0);
                    const float cc = image.at(x0, y0 + 1), d = image.at(x0 + 1, y0 + 1);
                    return (a + (b - a) * ax) * (1 - ay) + (cc + (d - cc) * ax) * ay;
                };
                const float yv = luma[i];
                const float r = yv + sample(crImage);
                const float b = yv + sample(cbImage);
                const float g = float((yv - Luma[0] * r - Luma[2] * b) / Luma[1]);
                rgb[i * 3] = r;
                rgb[i * 3 + 1] = g;
                rgb[i * 3 + 2] = b;
            }
        });
    }

    lap(2);

    // 4. Exposure: the short gain brings highlights just below white without
    // exceeding the metered exposure; the long gain lifts the scene key.
    // Highlights clipped in every channel (a window onto the sky) carry no
    // detail to keep: they only set the short gain if nothing else is
    // bright, otherwise compressing them darkens a halo around them.
    std::vector<float> samples, highlights;
    samples.reserve(pixels / 16 + 1);
    highlights.reserve(pixels / 16 + 1);
    double logSum = 0;
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            const float v = luma[std::size_t(y) * width + x];
            samples.push_back(v);
            if (guess.empty() || guessAt(x, y) < 0.5f) highlights.push_back(v);
        }
    }
    const double highlight = percentile(highlights.size() >= samples.size() / 4 ? highlights : samples, 0.999);
    std::vector<float>().swap(highlights);
    const double targetGain = std::max(1e-3, options.targetGain);
    // With exposure fusion, down to half the captured exposure, so rebuilt
    // highlights above the sensor clip level are compressed rather than
    // clipped again (the long exposure keeps the mid tones).
    const double shortFloor = options.localToneMapping > 0 ? 0.5 : 1.0;
    double shortGain = std::max(shortFloor * std::min(1.0, targetGain),
                                std::min(targetGain, 0.92 / std::max(highlight, 1e-6)));
    // Scene key at the metered exposure (noise below black counts as black).
    for (float v : samples) logSum += std::log(std::max(0.0f, v) * targetGain + 1e-4);
    const double key = std::exp(logSum / std::max<std::size_t>(1, samples.size()));
    // The long exposure is at least the metered one (what a normal capture
    // would show), up to 3x brighter for dark scenes; half that when the
    // highlights had to be compressed by 1 EV or more, so contrasty scenes
    // (a window in a dim room) stay natural. Relative to the metered
    // exposure, so a single frame and an underexposed burst of the same
    // scene come out equally bright.
    // Night mode lifts further but aims lower, so a night still looks like
    // night (dusk), not day.
    const double keyLift = options.night ? std::max(1.25, std::min(8.0, 0.15 / std::max(key, 1e-6)))
                                         : std::max(1.25, std::min(3.0, 0.22 / std::max(key, 1e-6)));
    const double lift = std::max(1.0, keyLift * std::max(0.5, std::min(1.0, shortGain / targetGain)));
    double longGain = shortGain;
    if (options.localToneMapping > 0) {
        // At most 2 EV between the two exposures: beyond that the fusion
        // darkens a wide halo around bright areas. Highlights that would
        // need more are compressed only that far.
        const double maxRatio = 4.0;
        const double wanted = targetGain * lift;
        shortGain = std::max(shortGain, wanted / maxRatio);
        const double ratio = std::max(1.5, wanted / shortGain);
        longGain = shortGain * std::pow(ratio, std::min(1.0, options.localToneMapping));
    }
    // Faces: mid-tones follow the long exposure, so a face lands near
    // faceLevel * longGain. Keep it within FaceLow..FaceHigh: a face in hard
    // light is not blown by the shadow lift, a backlit one is lifted.
    const double faceLevel = options.faces.empty() ? -1.0
            : largestFaceLevel(luma, width, height, options.faces, originX, originY, pixelStep,
                               sensorWidth, sensorHeight);
    // A face may be lifted up to 3 stops above the highlight exposure, past
    // the scene-key limit: against a bright sky it is the subject.
    if (faceLevel > 1e-5 && options.localToneMapping > 0)
        fitFace(faceLevel, &shortGain, &longGain, std::max(longGain, shortGain * 8.0));
    std::vector<float>().swap(samples);
    if (stats) {
        stats->shortGain = shortGain;
        stats->longGain = longGain;
        stats->highlightLevel = highlight;
        stats->keyLevel = key;
        stats->faceLevel = faceLevel;
    }
    std::vector<float> fused;
    if (options.localToneMapping > 0) {
        fused = fuseExposures(luma, width, height, shortGain, longGain);
    } else {
        fused.resize(pixels);
        parallelFor(0, height, [&](int y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(y) * width + x;
                const float v = float(luma[i] * shortGain);
                // Soft shoulder above 0.8 instead of a hard clip.
                const float shoulder = v < 0.8f ? v : 0.8f + 0.2f * (1.0f - std::exp(-(v - 0.8f) / 0.2f));
                fused[i] = c.toGamma(shoulder);
            }
        });
    }

    lap(3);

    // 5. Apply the fused luma to colour, add contrast and saturation in the
    // display domain. `fused` becomes the output luma used for sharpening.
    const float contrast = float(std::max(0.0, std::min(1.0, options.contrast)));
    const float shadowChroma = options.blackNoise > 0 ? float(1.0 / (8.0 * options.blackNoise)) : 0.0f;
    const float saturation = float(std::max(0.0, options.saturation));
    auto contrastCurve = [contrast](float l) { return (1 - contrast) * l + contrast * l * l * (3 - 2 * l); };
    // Display colour (gamma) and its luma for pixel i, before contrast.
    auto display = [&](std::size_t i, float g[3]) {
        const float *p = &rgb[i * 3];
        const float target = c.toLinear(fused[i]);
        const float scale = target / std::max(luma[i], 1e-7f);
        // Colour within the noise floor is mostly noise: fade it out
        // (luma at or below zero has no colour at all).
        float chroma = shadowChroma > 0 ? std::max(0.0f, std::min(1.0f, luma[i] * shadowChroma)) : 1.0f;
        chroma = chroma * chroma * (3 - 2 * chroma);
        float lin[3];
        float maximum = 0;
        for (int k = 0; k < 3; ++k) {
            lin[k] = target + (p[k] * scale - target) * chroma;
            maximum = std::max(maximum, lin[k]);
        }
        // Desaturate towards the luma instead of clipping a channel.
        if (maximum > 1.0f && maximum > target) {
            const float t = std::max(0.0f, (1.0f - target) / (maximum - target));
            for (float &v : lin) v = target + (v - target) * t;
        }
        for (int k = 0; k < 3; ++k) g[k] = c.toGamma(lin[k]);
        return float(Luma[0] * g[0] + Luma[1] * g[1] + Luma[2] * g[2]);
    };
    // Black and white points from the output luma's extremes (sampled,
    // inside the part of the frame the photo keeps).
    ToneEnds toneEnds;
    if (options.blackPoint || options.whitePoint) {
        std::vector<float> sample;
        sample.reserve(pixels / 16 + 1);
        int first, last;
        keptRange(width, height, options.keptShortSide, &first, &last);
        const bool landscape = width >= height;
        for (int y = 0; y < height; y += 4)
            for (int x = (y / 4) % 4; x < width; x += 4) {
                const int s = landscape ? y : x;
                if (s < first || s >= last) continue;
                float g[3];
                sample.push_back(contrastCurve(display(std::size_t(y) * width + x, g)));
            }
        toneEnds = measureToneEnds(sample, options.blackPoint, options.whitePoint);
        if (stats) {
            stats->lowLevel = toneEnds.low;
            stats->highLevel = toneEnds.high;
        }
    }
    // The black and white points move the smooth layer only and the fine
    // detail is added back unchanged: stretching the bright tones would
    // otherwise amplify their noise too (indoor walls: half again grainier).
    std::vector<float> curvedLuma, smoothLuma;
    if (toneEnds.blackPoint > 0 || toneEnds.whiteScale > 1) {
        curvedLuma.resize(pixels);
        parallelFor(0, height, [&](int y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(y) * width + x;
                float g[3];
                curvedLuma[i] = contrastCurve(display(i, g));
            }
        });
        smoothLuma = gaussianBlur(curvedLuma, width, height, 2.0);
    }
    parallelFor(0, height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = std::size_t(y) * width + x;
            float *p = &rgb[i * 3];
            float g[3];
            const float l = display(i, g);
            const float curved = contrastCurve(l);
            const float lc = smoothLuma.empty() ? toneEnds(curved)
                                                : toneEnds(smoothLuma[i]) + (curved - smoothLuma[i]);
            // Darkened shadows keep their colour in proportion.
            const float chromaScale = lc < curved ? lc / std::max(curved, 1e-4f) : 1.0f;
            for (int k = 0; k < 3; ++k)
                p[k] = std::max(0.0f, std::min(1.0f, lc + saturation * chromaScale * (g[k] - l)));
            fused[i] = lc;
        }
    });
    std::vector<float>().swap(luma);
    std::vector<float>().swap(curvedLuma);
    std::vector<float>().swap(smoothLuma);
    if (stats) {
        stats->blackPoint = toneEnds.blackPoint;
        stats->whiteScale = toneEnds.whiteScale;
    }

    // Clarity: local contrast at a medium scale in the mid-tones (texture of
    // wood, fabric, foliage), less in noisy shots, never in deep shadows or
    // highlights.
    if (options.clarity > 0) {
        const float amount = float(options.clarity
                                   * std::max(0.3, std::min(1.0, 1.0 - std::max(0.0, options.relativeNoise) / 0.05)));
        applyClarity(fused, width, height, amount, [&](std::size_t i, float delta) {
            for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = std::max(0.0f, std::min(1.0f, rgb[i * 3 + k] + delta));
        });
    }

    lap(4);

    // 6. Luma sharpening with coring so noise is not amplified.
    const double noise = std::max(0.0, options.relativeNoise);
    const float amount = float(std::max(0.0, options.sharpening) * std::max(0.2, std::min(0.6, 0.6 - 2.0 * noise)));
    const float coring = float(0.004 + 0.12 * noise);
    std::vector<float> blurred = amount > 0 ? gaussianBlur(fused, width, height, 1.0) : std::vector<float>();
    output.rgb.resize(pixels * 3);
    parallelFor(0, height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = std::size_t(y) * width + x;
            float delta = 0;
            if (amount > 0) {
                const float d = fused[i] - blurred[i];
                const float magnitude = std::max(0.0f, std::abs(d) - coring);
                // Lifted shadows are mostly noise: sharpen them less.
                const float shadow = std::max(0.15f, std::min(1.0f, (fused[i] - 0.08f) / 0.25f));
                delta = amount * shadow * std::max(-0.08f, std::min(0.08f, d < 0 ? -magnitude : magnitude));
            }
            for (int k = 0; k < 3; ++k) {
                const float v = rgb[i * 3 + k] + delta;
                output.rgb[i * 3 + k] = uint8_t(std::max(0, std::min(255, int(v * 255.0f + 0.5f))));
            }
        }
    });
    lap(5);
    return output;
}

// --- YUV bursts ------------------------------------------------------------------------

namespace {

// Noise of planes whose signal hardly varies (chroma): a low percentile of
// 16x16 block variances of the 4-neighbour residual, as a constant model.
NoiseModel flatNoise(const Planes &planes)
{
    NoiseModel model;
    const int block = 16;
    for (int p = 0; p < 4; ++p) {
        const std::vector<float> &plane = planes.phase[p];
        std::vector<double> variances;
        for (int by = 1; by + block < planes.height; by += block) {
            for (int bx = 1; bx + block < planes.width; bx += block) {
                double sum = 0, sum2 = 0;
                for (int y = by; y < by + block; ++y) {
                    for (int x = bx; x < bx + block; ++x) {
                        const std::size_t i = std::size_t(y) * planes.width + x;
                        const double r = plane[i] - 0.25 * (plane[i - 1] + plane[i + 1]
                                                           + plane[i - planes.width] + plane[i + planes.width]);
                        sum += r;
                        sum2 += r * r;
                    }
                }
                const double n = block * block;
                variances.push_back(std::max(0.0, sum2 / n - (sum / n) * (sum / n)) / 1.25);
            }
        }
        if (variances.empty()) continue;
        const std::size_t k = variances.size() / 10;
        std::nth_element(variances.begin(), variances.begin() + k, variances.end());
        model.scale[p] = 0;
        model.offset[p] = std::max(1e-8, variances[k]);
    }
    return model;
}

}

RgbImage renderYuvBurst(const std::vector<const YuvFrame *> &frames, const YuvOptions &options,
                        YuvStats *stats)
{
    RgbImage output;
    if (frames.empty() || !frames[0]) return output;
    const YuvFrame &reference = *frames[0];
    const int width = reference.width & ~1, height = reference.height & ~1;
    const int hw = width / 2, hh = height / 2;
    const std::size_t lumaSize = std::size_t(reference.width) * reference.height;
    const std::size_t chromaSize = std::size_t((reference.width + 1) / 2) * ((reference.height + 1) / 2);
    const int chromaStride = (reference.width + 1) / 2;
    std::vector<const YuvFrame *> usable;
    for (const YuvFrame *frame : frames) {
        if (frame && frame->width == reference.width && frame->height == reference.height
                && frame->data.size() >= lumaSize + 2 * chromaSize)
            usable.push_back(frame);
    }
    if (usable.empty() || width < 32 || height < 32) return output;
    const int count = int(usable.size());
    auto started = std::chrono::steady_clock::now();
    auto lap = [&](int stage) {
        const auto now = std::chrono::steady_clock::now();
        if (stats) stats->stageMs[stage] = std::chrono::duration<double, std::milli>(now - started).count();
        started = now;
    };

    // The merge works at half resolution: luma as its four 2x2 polyphase
    // planes, chroma (centred on 0.5) with U and V each in two of the four
    // slots. Tiles are read straight from the 8-bit frames (float copies of
    // a whole burst would need gigabytes).
    auto lumaAt = [&](const YuvFrame &f, int px, int py, int p) {
        return f.data[std::size_t(2 * py + (p >> 1)) * f.width + 2 * px + (p & 1)] * (1.0f / 255);
    };
    auto chromaAt = [&](const YuvFrame &f, int px, int py, int p) {
        const std::size_t offset = lumaSize + ((p & 1) ? chromaSize : 0);
        return f.data[offset + std::size_t(py) * chromaStride + px] * (1.0f / 255);
    };
    auto planesOf = [&](const YuvFrame &f, bool lumaPlanes) {
        Planes planes;
        planes.width = hw;
        planes.height = hh;
        for (int p = 0; p < 4; ++p) {
            planes.phase[p].resize(std::size_t(hw) * hh);
            parallelFor(0, hh, [&](int py) {
                for (int px = 0; px < hw; ++px)
                    planes.phase[p][std::size_t(py) * hw + px] = lumaPlanes ? lumaAt(f, px, py, p) : chromaAt(f, px, py, p);
            });
        }
        return planes;
    };
    auto grayOf = [&](const YuvFrame &f) {
        std::vector<float> gray(std::size_t(hw) * hh);
        parallelFor(0, hh, [&](int py) {
            for (int px = 0; px < hw; ++px)
                gray[std::size_t(py) * hw + px] = 0.25f * (lumaAt(f, px, py, 0) + lumaAt(f, px, py, 1)
                                                           + lumaAt(f, px, py, 2) + lumaAt(f, px, py, 3));
        });
        return gray;
    };
    auto loader = [&](bool lumaPlanes) {
        return [&, lumaPlanes](int z, int originX, int originY, float (*tiles)[256]) {
            const YuvFrame &f = *usable[z];
            for (int ty = 0; ty < MergeTileSize; ++ty) {
                const int py = reflect(originY + ty, hh);
                for (int tx = 0; tx < MergeTileSize; ++tx) {
                    const int px = reflect(originX + tx, hw);
                    for (int p = 0; p < 4; ++p)
                        tiles[p][ty * MergeTileSize + tx] = lumaPlanes ? lumaAt(f, px, py, p) : chromaAt(f, px, py, p);
                }
            }
        };
    };

    const std::vector<float> referenceGray = grayOf(reference);
    std::vector<Alignment> alignments;
    for (int z = 1; z < count; ++z) alignments.push_back(align(referenceGray, grayOf(*usable[z]), hw, hh));
    lap(0);

    // Noise: luma from the brightness-dependent fit, chroma flat. Spatial
    // denoising follows the noise expected in the shadows after merging.
    NoiseModel lumaNoise;
    {
        const Planes planes = planesOf(reference, true);
        lumaNoise = estimateNoise(planes);
        if (!(lumaNoise.meanVariance(0.2) > 0)) lumaNoise = flatNoise(planes);
    }
    const NoiseModel chromaNoise = flatNoise(planesOf(reference, false));
    // Judged after the expected merge (some frames are rejected for motion)
    // and after the shadow lift of an underexposed burst (gamma domain).
    const double brightness = std::isfinite(options.brightness) ? std::max(1.0, std::min(4.0, options.brightness)) : 1.0;
    const double effectiveFrames = 1.0 + (count - 1) * 0.6;
    const double shadowSigma = std::sqrt(lumaNoise.meanVariance(0.15) / effectiveFrames)
            * std::pow(brightness, 1 / 2.2);
    const double severity = std::max(0.0, std::min(1.0, (shadowSigma - 0.003) / 0.012));
    MergeOptions lumaOptions;
    lumaOptions.spatialStrength = (1.0 + 1.5 * severity) * std::max(0.0, options.spatialDenoise);
    lumaOptions.spatialFloor = 0.5 * severity;
    lumaOptions.spatialPilot = 2.5 * severity;
    // Chroma noise is blotchy and carries little detail: always strong.
    MergeOptions chromaOptions = lumaOptions;
    chromaOptions.spatialStrength = 3.0 * std::max(0.0, options.spatialDenoise);
    chromaOptions.spatialFloor = 0.6;
    chromaOptions.spatialPilot = 2.5;
    MergeStats mergeStats;
    const Planes mergedLuma = mergeFrames(hw, hh, 1.0f, count, loader(true), alignments, lumaNoise,
                                          lumaOptions, &mergeStats);
    const Planes mergedChroma = mergeFrames(hw, hh, 1.0f, count, loader(false), alignments, chromaNoise,
                                            chromaOptions, nullptr);
    lap(1);

    // Reassemble full-resolution luma; chroma back to signed values with an
    // edge-aware clean-up guided by luma.
    int outWidth = width, outHeight = height;
    std::vector<float> y(std::size_t(width) * height);
    parallelFor(0, hh, [&](int py) {
        for (int px = 0; px < hw; ++px) {
            const std::size_t i = std::size_t(py) * hw + px;
            for (int p = 0; p < 4; ++p)
                y[std::size_t(2 * py + (p >> 1)) * width + 2 * px + (p & 1)] = mergedLuma.phase[p][i];
        }
    });
    std::vector<float> u(std::size_t(hw) * hh), v(u.size());
    for (std::size_t i = 0; i < u.size(); ++i) {
        u[i] = 0.5f * (mergedChroma.phase[0][i] + mergedChroma.phase[2][i]) - 0.5f;
        v[i] = 0.5f * (mergedChroma.phase[1][i] + mergedChroma.phase[3][i]) - 0.5f;
    }
    chromaBilateral(&u, &v, grayImage(mergedLuma), hw, hh, 2.0 + 4.0 * severity);

    // An underexposed burst (highlights kept) is brought back to the metered
    // brightness by exposure fusion on linear luma, as for RAW: the short
    // exposure keeps faces and skies, the long one lifts the room. Chroma
    // follows the luma change.
    std::vector<float> chromaGain;
    // The largest face, from the merged luma (zoomed renders skip it).
    double faceLevel = -1;
    if (!options.faces.empty() && !options.zoom.active()) {
        std::vector<float> faceLuma(y.size());
        const Curve &c = curve();
        for (std::size_t i = 0; i < y.size(); ++i) faceLuma[i] = c.toLinear(std::max(0.0f, std::min(1.0f, y[i])));
        faceLevel = largestFaceLevel(faceLuma, width, height, options.faces, 0, 0, 1, width, height);
    }
    if (stats) stats->faceLevel = faceLevel;
    const bool faceNeedsTone = faceLevel > 1e-5
            && (faceLevel * brightness > FaceHigh || faceLevel * brightness < FaceLow);
    if (brightness > 1.01 || options.night || faceNeedsTone) {
        const Curve &c = curve();
        std::vector<float> linear(y.size());
        parallelFor(0, height, [&](int row) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(row) * width + x;
                linear[i] = c.toLinear(std::max(0.0f, std::min(1.0f, y[i])));
            }
        });
        // The ISP's tone curve crushes the shadows of the darker capture
        // (device selfies: lower quartile 9-19 against 58 at the metered
        // exposure), so dark scenes get extra lift, as the RAW path does:
        // up to 2x the metered exposure, no more than the underexposure.
        double logSum = 0;
        long samples = 0;
        for (std::size_t i = 0; i < linear.size(); i += 16, ++samples)
            logSum += std::log(linear[i] * brightness + 1e-4);
        const double key = std::exp(logSum / std::max(1L, samples));
        const double lift = std::max(1.0, std::min(options.night ? 3.0 : std::min(2.0, brightness),
                                                   0.2 / std::max(key, 1e-6)));
        double longGain = brightness * lift;
        if (faceLevel > 1e-5) {
            // As for RAW, but the short exposure stays the captured one: YUV
            // cannot go darker than the ISP's frame usefully, nor recover a
            // face it already clipped (the burst plan gives faces headroom).
            double shortGain = 1.0;
            // Up to 3 stops above the captured frame, as for RAW.
            fitFace(faceLevel, &shortGain, &longGain, std::max(longGain, 8.0));
            longGain = std::max(1.0, longGain / shortGain);
        }
        const std::vector<float> fused = fuseExposures(linear, width, height, 1.0, longGain);
        chromaGain.resize(y.size());
        parallelFor(0, height, [&](int row) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = std::size_t(row) * width + x;
                // A gentle toe instead of crushed blacks.
                const float toe = std::max(0.0f, 1.0f - fused[i] / 0.12f);
                const float out = fused[i] + 0.025f * toe * toe;
                // Chroma follows the luma change, much less where it is lifted:
                // the ISP's colour in dark areas is already exaggerated (a
                // face captured dark came out 3x too red for its luma) and
                // partly noise.
                const float ratio = out / std::max(0.02f, y[i]);
                chromaGain[i] = std::max(0.5f, std::min(2.0f, std::pow(ratio, ratio > 1.0f ? 0.5f : 0.8f)));
                y[i] = out;
            }
        });
    }

    // Upsampled chroma at full resolution (bilinear, sample centres between pixels).
    Raisr::Image uImage, vImage;
    uImage.width = vImage.width = hw;
    uImage.height = vImage.height = hh;
    uImage.data.swap(u);
    vImage.data.swap(v);
    Raisr::Image uFull = Raisr::resize(uImage, width, height), vFull = Raisr::resize(vImage, width, height);
    if (!chromaGain.empty()) {
        for (std::size_t i = 0; i < chromaGain.size(); ++i) {
            uFull.data[i] *= chromaGain[i];
            vFull.data[i] *= chromaGain[i];
        }
        std::vector<float>().swap(chromaGain);
    }

    // Zoom: the centre crop, upscaled (learned filters on luma).
    const ZoomRegion &zoom = options.zoom;
    if (zoom.active() && zoom.x >= 0 && zoom.y >= 0 && zoom.x + zoom.width <= width
            && zoom.y + zoom.height <= height) {
        outWidth = zoom.outputWidth();
        outHeight = zoom.outputHeight();
        auto crop = [&](const float *source) {
            Raisr::Image image;
            image.width = zoom.width;
            image.height = zoom.height;
            image.data.resize(std::size_t(zoom.width) * zoom.height);
            for (int row = 0; row < zoom.height; ++row)
                std::copy(source + std::size_t(zoom.y + row) * width + zoom.x,
                          source + std::size_t(zoom.y + row) * width + zoom.x + zoom.width,
                          image.data.begin() + std::size_t(row) * zoom.width);
            return image;
        };
        const Raisr::Image yCrop = crop(y.data());
        const Raisr::Image yCoarse = Raisr::resize(yCrop, outWidth, outHeight);
        Raisr::Image ySharp = options.upscaleFilters && options.upscaleFilters->valid()
                ? Raisr::upscale(yCrop, outWidth, outHeight, *options.upscaleFilters) : yCoarse;
        const float strength = float(std::max(0.0, std::min(1.0, zoom.scale - 1.0)));
        for (std::size_t i = 0; i < ySharp.data.size(); ++i)
            ySharp.data[i] = yCoarse.data[i] + strength * (ySharp.data[i] - yCoarse.data[i]);
        y.swap(ySharp.data);
        uFull = Raisr::resize(crop(uFull.data.data()), outWidth, outHeight);
        vFull = Raisr::resize(crop(vFull.data.data()), outWidth, outHeight);
    }

    const double mergedSigma = std::sqrt(lumaNoise.meanVariance(0.3) / (1.0 + (count - 1) * (1.0 - mergeStats.rejection)));
    const double liftedSigma = mergedSigma * std::pow(brightness, 1 / 2.2);

    // Tone refinements, as for RAW: black and white points on the luma,
    // colour kept in proportion where shadows darken, a little more
    // saturation, and clarity in the mid-tones (less when noisy).
    if (options.blackPoint || options.whitePoint || options.saturation != 1.0) {
        ToneEnds ends;
        if (options.blackPoint || options.whitePoint) {
            std::vector<float> sample;
            int first, last;
            keptRange(outWidth, outHeight, options.keptShortSide, &first, &last);
            const bool landscape = outWidth >= outHeight;
            for (int row = 0; row < outHeight; row += 4)
                for (int x = (row / 4) % 4; x < outWidth; x += 4) {
                    const int s = landscape ? row : x;
                    if (s >= first && s < last) sample.push_back(y[std::size_t(row) * outWidth + x]);
                }
            ends = measureToneEnds(sample, options.blackPoint, options.whitePoint);
            if (stats) {
                stats->blackPoint = ends.blackPoint;
                stats->whiteScale = ends.whiteScale;
            }
        }
        const float saturation = float(std::max(0.0, options.saturation));
        // As for RAW: the tone ends move the smooth layer, detail unchanged.
        const std::vector<float> smooth = ends.blackPoint > 0 || ends.whiteScale > 1
                ? gaussianBlur(y, outWidth, outHeight, 2.0) : std::vector<float>();
        parallelFor(0, outHeight, [&](int row) {
            for (int x = 0; x < outWidth; ++x) {
                const std::size_t i = std::size_t(row) * outWidth + x;
                const float before = y[i];
                const float after = smooth.empty() ? ends(before) : ends(smooth[i]) + (before - smooth[i]);
                const float chroma = saturation * (after < before ? after / std::max(before, 1e-4f) : 1.0f);
                y[i] = after;
                uFull.data[i] *= chroma;
                vFull.data[i] *= chroma;
            }
        });
    }
    if (options.clarity > 0) {
        const float clarity = float(options.clarity * std::max(0.3, std::min(1.0, 1.0 - liftedSigma / 0.03)));
        applyClarity(y, outWidth, outHeight, clarity, [](std::size_t, float) {});
    }

    // Luma sharpening with coring above the merged noise, less in shadows.
    const float amount = float(std::max(0.0, options.sharpening) * std::max(0.2, std::min(0.55, 0.55 - 15.0 * liftedSigma)));
    const float coring = float(0.004 + 2.5 * liftedSigma);
    if (amount > 0) {
        const std::vector<float> blurred = gaussianBlur(y, outWidth, outHeight, 1.0);
        parallelFor(0, outHeight, [&](int row) {
            for (int x = 0; x < outWidth; ++x) {
                const std::size_t i = std::size_t(row) * outWidth + x;
                const float detail = y[i] - blurred[i];
                const float magnitude = std::abs(detail);
                const float cored = magnitude <= coring ? 0.0f : (magnitude - coring) * (detail < 0 ? -1.0f : 1.0f);
                const float shadow = std::max(0.3f, std::min(1.0f, (y[i] - 0.05f) / 0.2f));
                y[i] += amount * shadow * cored;
            }
        });
    }

    // White balance towards neutral: the ISP's own balance leaves casts
    // (front camera: a pink white ceiling, R/G 1.30 and B/G 0.96; under warm
    // lamps R/G up to 1.6). Near-neutral mid-tones are averaged in linear
    // light, weighted towards the greyest and leaving faces out (skin is not
    // a cast); red and blue are then moved 60% of the way to green, so a warm
    // room keeps some of its mood. Needs enough near-neutral surfaces: a
    // uniformly coloured subject is never "corrected".
    float channelGain[3] = {1.0f, 1.0f, 1.0f};
    {
        const Curve &c = curve();
        const bool faceMask = !options.faces.empty() && !zoom.active();
        auto inFace = [&](int x, int row) {
            for (const std::array<float, 4> &f : options.faces)
                if (x >= f[0] * outWidth && x < f[2] * outWidth && row >= f[1] * outHeight && row < f[3] * outHeight)
                    return true;
            return false;
        };
        double red = 0, green = 0, blue = 0, weightSum = 0;
        long considered = 0;
        for (int row = 0; row < outHeight; row += 3) {
            for (int x = (row / 3) % 3; x < outWidth; x += 3) {
                const std::size_t i = std::size_t(row) * outWidth + x;
                const float luma = y[i], cb = uFull.data[i], cr = vFull.data[i];
                if (luma < 0.2f || luma > 0.85f) continue;
                const float chroma = std::sqrt(cb * cb + cr * cr);
                if (faceMask && inFace(x, row)) continue;
                ++considered;
                // Wide enough that the grey surfaces under a strong lamp
                // (themselves orange) still count.
                const double w = 1.0 / (1.0 + std::pow(chroma / 0.12, 2.0));
                red += w * c.toLinear(std::max(0.0f, std::min(1.0f, luma + 1.402f * cr)));
                green += w * c.toLinear(std::max(0.0f, std::min(1.0f, luma - 0.344136f * cb - 0.714136f * cr)));
                blue += w * c.toLinear(std::max(0.0f, std::min(1.0f, luma + 1.772f * cb)));
                weightSum += w;
            }
        }
        if (considered > 200 && weightSum > 0.3 * considered && red > 0 && green > 0 && blue > 0) {
            auto gain = [](double ratio) {
                // A 3% deadband, then 60% of the way, at most 1.6x either way.
                if (ratio > 0.97 && ratio < 1.03) return 1.0f;
                return float(std::max(0.62, std::min(1.6, std::pow(1.0 / ratio, 0.6))));
            };
            channelGain[0] = gain(red / green);
            channelGain[2] = gain(blue / green);
        }
    }
    const bool castCorrection = channelGain[0] != 1.0f || channelGain[2] != 1.0f;

    // Full-range BT.601 (JFIF), as camera YUV_420_888 is delivered.
    output.width = outWidth;
    output.height = outHeight;
    output.rgb.resize(std::size_t(outWidth) * outHeight * 3);
    parallelFor(0, outHeight, [&](int row) {
        for (int x = 0; x < outWidth; ++x) {
            const std::size_t i = std::size_t(row) * outWidth + x;
            const float luma = y[i], cb = uFull.data[i], cr = vFull.data[i];
            float rgb[3] = {luma + 1.402f * cr, luma - 0.344136f * cb - 0.714136f * cr, luma + 1.772f * cb};
            if (castCorrection) {
                const Curve &c = curve();
                for (int k = 0; k < 3; k += 2)
                    if (channelGain[k] != 1.0f)
                        rgb[k] = c.toGamma(c.toLinear(std::max(0.0f, std::min(1.0f, rgb[k]))) * channelGain[k]);
            }
            for (int k = 0; k < 3; ++k)
                output.rgb[i * 3 + k] = uint8_t(std::max(0, std::min(255, int(rgb[k] * 255.0f + 0.5f))));
        }
    });
    lap(2);
    if (stats) {
        stats->rejection = mergeStats.rejection;
        stats->lumaNoise = std::sqrt(lumaNoise.meanVariance(0.3));
        stats->chromaNoise = std::sqrt(chromaNoise.meanVariance(0.5));
        stats->severity = severity;
    }
    return output;
}

}
