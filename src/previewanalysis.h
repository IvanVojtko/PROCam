// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef PREVIEWANALYSIS_H
#define PREVIEWANALYSIS_H

// Viewfinder measurements for the burst plan, from 8-bit luma (the Y plane,
// or the green channel of RGB): how fast the image moves (hand shake), how
// much of it moves on its own (subjects), and how bright the main face is.
// Header-only so the host tests can run it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace PreviewAnalysis {

// Samples at data[y * stride + x * pixelStride].
struct LumaView {
    const uint8_t *data = nullptr;
    int width = 0, height = 0, stride = 0, pixelStride = 1;
    int at(int x, int y) const { return data[std::size_t(y) * stride + std::size_t(x) * pixelStride]; }
};

namespace detail {

// Profile with its slow trend removed (lighting gradients, exposure) and
// normalised, so a shift is found from structure only.
inline std::vector<float> normalised(std::vector<float> p)
{
    const int n = int(p.size()), r = 16;
    std::vector<float> prefix(n + 1, 0.0f);
    for (int i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + p[i];
    std::vector<float> out(n);
    double sum2 = 0;
    for (int i = 0; i < n; ++i) {
        const int a = std::max(0, i - r), b = std::min(n, i + r + 1);
        out[i] = p[i] - (prefix[b] - prefix[a]) / (b - a);
        sum2 += double(out[i]) * out[i];
    }
    const float scale = float(1.0 / std::sqrt(std::max(1e-6, sum2 / std::max(1, n))));
    for (float &v : out) v *= scale;
    return out;
}

// Shift s (sub-pixel) for which current[i] best matches previous[i - s]:
// the content moved by s. 0 when the profiles carry no structure.
inline double profileShift(const std::vector<float> &previous, const std::vector<float> &current, int maxShift)
{
    const int n = int(current.size());
    if (n != int(previous.size()) || n < 4 * maxShift) return 0.0;
    std::vector<double> cost(2 * maxShift + 1);
    for (int s = -maxShift; s <= maxShift; ++s) {
        double sum = 0;
        for (int i = maxShift; i < n - maxShift; ++i) sum += std::abs(current[i] - previous[i - s]);
        cost[s + maxShift] = sum / (n - 2 * maxShift);
    }
    const int best = int(std::min_element(cost.begin(), cost.end()) - cost.begin());
    double shift = best - maxShift;
    if (best > 0 && best < 2 * maxShift) {
        const double a = cost[best - 1], b = cost[best], c = cost[best + 1];
        const double denominator = a - 2 * b + c;
        if (denominator > 1e-9) shift += 0.5 * (a - c) / denominator;
    }
    return shift;
}

} // namespace detail

class MotionMeter
{
public:
    void reset()
    {
        m_columns.clear();
        m_rows.clear();
        m_grid.clear();
        m_ms = -1;
        m_motion = -1.0;
        m_subject = 0.0;
    }

    // Frame widths per second; -1 until two frames were compared.
    double motion() const { return m_motion; }
    // Share of the frame (0..1) that changed beyond the global shift.
    double subjectMotion() const { return m_subject; }

    void update(const LumaView &luma, long long ms)
    {
        if (!luma.data || luma.width < 64 || luma.height < 48) return;
        std::vector<float> columns(luma.width, 0.0f), rows(luma.height, 0.0f);
        const int step = 4;
        for (int y = 0; y < luma.height; y += step)
            for (int x = 0; x < luma.width; ++x) columns[x] += float(luma.at(x, y));
        for (int y = 0; y < luma.height; ++y) {
            float sum = 0;
            for (int x = 0; x < luma.width; x += step) sum += float(luma.at(x, y));
            rows[y] = sum;
        }
        columns = detail::normalised(columns);
        rows = detail::normalised(rows);
        // Coarse grid of block means for subject motion.
        const int gx = 32, gy = 24;
        std::vector<float> grid(gx * gy, 0.0f);
        std::vector<int> count(gx * gy, 0);
        for (int y = 0; y < luma.height; y += step)
            for (int x = 0; x < luma.width; x += step) {
                const int i = (y * gy / luma.height) * gx + x * gx / luma.width;
                grid[i] += float(luma.at(x, y));
                ++count[i];
            }
        for (int i = 0; i < gx * gy; ++i) grid[i] /= std::max(1, count[i]);

        const long long dt = m_ms >= 0 ? ms - m_ms : -1;
        if (dt >= 5 && dt <= 400 && int(m_columns.size()) == luma.width && int(m_rows.size()) == luma.height) {
            const int maxShift = std::max(4, luma.width / 40);
            const double sx = detail::profileShift(m_columns, columns, maxShift);
            const double sy = detail::profileShift(m_rows, rows, maxShift);
            const double speed = std::hypot(sx, sy) / luma.width / (dt / 1000.0);
            // Subject motion: blocks that changed after removing the shift
            // and any exposure change.
            // The previous grid sampled at the exact (sub-cell) shift.
            const double cx = sx * gx / luma.width, cy = sy * gy / luma.height;
            double meanNow = 0, meanBefore = 0;
            for (int i = 0; i < gx * gy; ++i) { meanNow += grid[i]; meanBefore += m_grid[i]; }
            const double exposure = meanBefore > 0 ? meanNow / meanBefore : 1.0;
            int moving = 0, compared = 0;
            for (int y = 1; y < gy - 1; ++y)
                for (int x = 1; x < gx - 1; ++x) {
                    const double px = x - cx, py = y - cy;
                    const int ix = int(std::floor(px)), iy = int(std::floor(py));
                    if (ix < 0 || ix + 1 >= gx || iy < 0 || iy + 1 >= gy) continue;
                    const double ax = px - ix, ay = py - iy;
                    const float *g = &m_grid[iy * gx + ix];
                    const double before = exposure * ((g[0] * (1 - ax) + g[1] * ax) * (1 - ay)
                                                      + (g[gx] * (1 - ax) + g[gx + 1] * ax) * ay);
                    const double now = grid[y * gx + x];
                    ++compared;
                    if (std::abs(now - before) > std::max(8.0, 0.12 * before)) ++moving;
                }
            const double subject = compared ? double(moving) / compared : 0.0;
            const double alpha = 1.0 - std::exp(-double(dt) / 250.0);
            m_motion = m_motion < 0 ? speed : m_motion + alpha * (speed - m_motion);
            m_subject += alpha * (subject - m_subject);
        }
        m_columns.swap(columns);
        m_rows.swap(rows);
        m_grid.swap(grid);
        m_ms = ms;
    }

private:
    std::vector<float> m_columns, m_rows, m_grid;
    long long m_ms = -1;
    double m_motion = -1.0, m_subject = 0.0;
};

struct FaceExposure {
    int count = 0;
    double luma = -1.0;    // mean of the largest face's centre, 0..1
    double clipped = 0.0;  // share of it at 250 or above
};

// faces: (left, top, right, bottom), normalised on the frame.
inline FaceExposure faceExposure(const LumaView &luma, const std::vector<std::array<float, 4>> &faces)
{
    FaceExposure result;
    result.count = int(faces.size());
    double bestArea = 0;
    for (const std::array<float, 4> &face : faces) {
        int x0 = std::max(0, std::min(luma.width, int(face[0] * luma.width)));
        int x1 = std::max(0, std::min(luma.width, int(face[2] * luma.width)));
        int y0 = std::max(0, std::min(luma.height, int(face[1] * luma.height)));
        int y1 = std::max(0, std::min(luma.height, int(face[3] * luma.height)));
        const double area = double(x1 - x0) * (y1 - y0);
        if (area < 16 || area <= bestArea) continue;
        // The centre: the box also holds hair and background.
        const int insetX = (x1 - x0) / 5, insetY = (y1 - y0) / 5;
        x0 += insetX; x1 -= insetX; y0 += insetY; y1 -= insetY;
        const int step = std::max(1, int(std::sqrt(area / 2000)));
        double sum = 0;
        long samples = 0, clipped = 0;
        for (int y = y0; y < y1; y += step)
            for (int x = x0; x < x1; x += step, ++samples) {
                const int v = luma.at(x, y);
                sum += v;
                clipped += v >= 250;
            }
        if (!samples) continue;
        bestArea = area;
        result.luma = sum / samples / 255.0;
        result.clipped = double(clipped) / samples;
    }
    return result;
}

// A selfie against bright light: the camera's face detection usually misses
// a face that is nearly black in the viewfinder. When the centre of the
// frame (where a selfie's face is) is very dark while the rest holds bright
// light, the centre is taken as the subject: (left, top, right, bottom)
// normalised, or false.
inline bool backlitSubject(const LumaView &luma, std::array<float, 4> *subject)
{
    if (!luma.data || luma.width < 32 || luma.height < 32) return false;
    int histogram[256] = {0};
    long total = 0;
    double centre = 0;
    long centreCount = 0;
    const int step = std::max(1, std::min(luma.width, luma.height) / 120);
    for (int y = 0; y < luma.height; y += step)
        for (int x = 0; x < luma.width; x += step) {
            const int v = luma.at(x, y);
            ++histogram[v];
            ++total;
            if (x > luma.width * 3 / 10 && x < luma.width * 7 / 10
                    && y > luma.height * 3 / 10 && y < luma.height * 7 / 10) {
                centre += v;
                ++centreCount;
            }
        }
    if (!centreCount) return false;
    centre /= centreCount;
    long cumulative = 0;
    int p95 = 255;
    for (int v = 0; v < 256; ++v) {
        cumulative += histogram[v];
        if (cumulative >= total * 95 / 100) { p95 = v; break; }
    }
    if (centre > 0.15 * 255 || p95 < 0.7 * 255 || p95 - centre < 0.5 * 255) return false;
    *subject = {{0.3f, 0.3f, 0.7f, 0.7f}};
    return true;
}

} // namespace PreviewAnalysis

#endif // PREVIEWANALYSIS_H
