// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "scenescan.h"

#include "3rdparty/quirc/quirc.h"

#include <QLineF>
#include <QTextCodec>

#include <algorithm>
#include <cmath>
#include <memory>

namespace SceneScan {

namespace {

const double Pi = 3.14159265358979323846;

double dot(const QPointF &a, const QPointF &b) { return a.x() * b.x() + a.y() * b.y(); }
double length(const QPointF &a) { return std::hypot(a.x(), a.y()); }

// ---- QR codes ------------------------------------------------------------

// A local-mean threshold into quirc's buffer (0 dark, 255 light): quirc's
// own global (Otsu) threshold loses codes in uneven light, such as a lit
// poster in a dim room. A pixel is dark only clearly below its
// neighbourhood, so flat areas stay light instead of turning into speckle.
void binarize(const PreviewAnalysis::LumaView &luma, uint8_t *out)
{
    const int w = luma.width, h = luma.height, stride = w + 1;
    // Sums stay below 2^32 for frames up to 16 million pixels.
    std::vector<uint32_t> integral(std::size_t(stride) * (h + 1), 0);
    for (int y = 0; y < h; ++y) {
        uint32_t row = 0;
        const uint32_t *above = &integral[std::size_t(y) * stride + 1];
        uint32_t *line = &integral[std::size_t(y + 1) * stride + 1];
        for (int x = 0; x < w; ++x) {
            row += uint32_t(luma.at(x, y));
            line[x] = above[x] + row;
        }
    }
    // About a twelfth of the short side: several modules of a code that
    // fills the frame, while light changes slowly across it.
    const int r = std::max(8, std::min(w, h) / 12);
    for (int y = 0; y < h; ++y) {
        const int y0 = std::max(0, y - r), y1 = std::min(h, y + r + 1);
        const uint32_t *top = &integral[std::size_t(y0) * stride];
        const uint32_t *bottom = &integral[std::size_t(y1) * stride];
        uint8_t *line = out + std::size_t(y) * w;
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - r), x1 = std::min(w, x + r + 1);
            const uint32_t sum = bottom[x1] - bottom[x0] - top[x1] + top[x0];
            const int mean = int(sum / uint32_t((x1 - x0) * (y1 - y0)));
            line[x] = luma.at(x, y) < mean - std::max(4, mean / 12) ? 0 : 255;
        }
    }
}

QString payloadText(const quirc_data &data)
{
    const char *bytes = reinterpret_cast<const char *>(data.payload);
    if (data.data_type == QUIRC_DATA_TYPE_KANJI || data.eci == QUIRC_ECI_SHIFT_JIS) {
        if (QTextCodec *codec = QTextCodec::codecForName("Shift-JIS"))
            return codec->toUnicode(bytes, data.payload_len);
    }
    // Most generators write UTF-8 without an ECI saying so; ISO 8859-1 (the
    // standard's default) for anything that is not valid UTF-8.
    if (data.eci == 0 || data.eci == QUIRC_ECI_UTF_8) {
        if (QTextCodec *utf8 = QTextCodec::codecForName("UTF-8")) {
            QTextCodec::ConverterState state;
            const QString text = utf8->toUnicode(bytes, data.payload_len, &state);
            if (state.invalidChars == 0)
                return text;
        }
    }
    return QString::fromLatin1(bytes, data.payload_len);
}

// ---- Documents -----------------------------------------------------------

// Float image, pixel (x, y) centred on integer coordinates.
struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;

    float at(int x, int y) const { return v[std::size_t(y) * w + x]; }
    // Bilinear, clamped to the plane.
    float sample(double x, double y) const
    {
        x = std::min(std::max(x, 0.0), w - 1.001);
        y = std::min(std::max(y, 0.0), h - 1.001);
        const int x0 = int(x), y0 = int(y);
        const float fx = float(x - x0), fy = float(y - y0);
        const float *a = &v[std::size_t(y0) * w + x0], *b = a + w;
        return (a[0] * (1 - fx) + a[1] * fx) * (1 - fy) + (b[0] * (1 - fx) + b[1] * fx) * fy;
    }
};

// Box-averaged by an integer factor to at most maxSide on the long side.
Plane downscaled(const PreviewAnalysis::LumaView &luma, int maxSide, int *factor)
{
    const int f = std::max(1, (std::max(luma.width, luma.height) + maxSide - 1) / maxSide);
    Plane p;
    p.w = luma.width / f;
    p.h = luma.height / f;
    p.v.assign(std::size_t(p.w) * p.h, 0.0f);
    for (int y = 0; y < p.h; ++y) {
        float *out = &p.v[std::size_t(y) * p.w];
        for (int dy = 0; dy < f; ++dy) {
            const int sy = y * f + dy;
            for (int x = 0; x < p.w; ++x) {
                int sum = 0;
                for (int dx = 0; dx < f; ++dx)
                    sum += luma.at(x * f + dx, sy);
                out[x] += float(sum);
            }
        }
        for (int x = 0; x < p.w; ++x)
            out[x] /= float(f * f);
    }
    *factor = f;
    return p;
}

// Binomial 1-4-6-4-1 blur: the gradients then follow paper edges rather
// than texture and sensor noise.
Plane blurred(const Plane &p)
{
    static const float k[5] = {1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f};
    Plane tmp = p, out = p;
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            float sum = 0;
            for (int i = -2; i <= 2; ++i)
                sum += k[i + 2] * p.at(std::min(std::max(x + i, 0), p.w - 1), y);
            tmp.v[std::size_t(y) * p.w + x] = sum;
        }
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            float sum = 0;
            for (int i = -2; i <= 2; ++i)
                sum += k[i + 2] * tmp.at(x, std::min(std::max(y + i, 0), p.h - 1));
            out.v[std::size_t(y) * p.w + x] = sum;
        }
    return out;
}

// Sobel gradients and thinned (non-maximum suppressed) edge pixels.
struct Edges {
    int w = 0, h = 0;
    std::vector<float> gx, gy, mag;
    std::vector<uint8_t> edge;
};

// A step of about 8 levels: after the 1-4-6-4-1 blur, Sobel gives 2.5x a
// sharp step's height.
const float EdgeThreshold = 20.0f;

Edges edgesOf(const Plane &p)
{
    Edges e;
    e.w = p.w;
    e.h = p.h;
    const std::size_t n = std::size_t(p.w) * p.h;
    e.gx.assign(n, 0.0f);
    e.gy.assign(n, 0.0f);
    e.mag.assign(n, 0.0f);
    e.edge.assign(n, 0);
    for (int y = 1; y < p.h - 1; ++y)
        for (int x = 1; x < p.w - 1; ++x) {
            const float gx = (p.at(x + 1, y - 1) + 2 * p.at(x + 1, y) + p.at(x + 1, y + 1))
                    - (p.at(x - 1, y - 1) + 2 * p.at(x - 1, y) + p.at(x - 1, y + 1));
            const float gy = (p.at(x - 1, y + 1) + 2 * p.at(x, y + 1) + p.at(x + 1, y + 1))
                    - (p.at(x - 1, y - 1) + 2 * p.at(x, y - 1) + p.at(x + 1, y - 1));
            const std::size_t i = std::size_t(y) * p.w + x;
            e.gx[i] = gx;
            e.gy[i] = gy;
            e.mag[i] = std::hypot(gx, gy);
        }
    for (int y = 2; y < p.h - 2; ++y)
        for (int x = 2; x < p.w - 2; ++x) {
            const std::size_t i = std::size_t(y) * p.w + x;
            const float m = e.mag[i];
            if (m < EdgeThreshold) continue;
            const float ax = std::abs(e.gx[i]), ay = std::abs(e.gy[i]);
            int dx, dy;
            if (ay <= 0.4142f * ax) { dx = 1; dy = 0; }
            else if (ax <= 0.4142f * ay) { dx = 0; dy = 1; }
            else if (e.gx[i] * e.gy[i] > 0) { dx = 1; dy = 1; }
            else { dx = 1; dy = -1; }
            const float a = e.mag[std::size_t(y + dy) * p.w + x + dx];
            const float b = e.mag[std::size_t(y - dy) * p.w + x - dx];
            if (m >= a && m > b) e.edge[i] = 1;
        }
    return e;
}

// nx * x + ny * y = rho, in plane pixels.
struct Line {
    int theta = 0; // degrees, 0..179
    double nx = 1, ny = 0, rho = 0;
    int votes = 0;
};

double lineAngle(const Line &a, const Line &b)
{
    const int d = std::abs(a.theta - b.theta) % 180;
    return std::min(d, 180 - d);
}

bool intersect(const Line &a, const Line &b, QPointF *point)
{
    const double det = a.nx * b.ny - a.ny * b.nx;
    if (std::abs(det) < 1e-6) return false;
    *point = QPointF((a.rho * b.ny - a.ny * b.rho) / det, (a.nx * b.rho - a.rho * b.nx) / det);
    return true;
}

// The strongest straight edges: a Hough transform in which each edge pixel
// votes only for lines within 8 degrees of its own edge direction.
std::vector<Line> houghLines(const Edges &e, int maxLines, int minVotes)
{
    const int thetas = 180;
    const int rhoMax = int(std::ceil(std::hypot(e.w, e.h)));
    const int rhos = 2 * rhoMax + 1;
    std::vector<int> acc(std::size_t(thetas) * rhos, 0);
    double cosT[thetas], sinT[thetas];
    for (int t = 0; t < thetas; ++t) {
        cosT[t] = std::cos(t * Pi / 180);
        sinT[t] = std::sin(t * Pi / 180);
    }
    for (int y = 0; y < e.h; ++y)
        for (int x = 0; x < e.w; ++x) {
            const std::size_t i = std::size_t(y) * e.w + x;
            if (!e.edge[i]) continue;
            int phi = int(std::lround(std::atan2(e.gy[i], e.gx[i]) * 180 / Pi));
            phi = ((phi % 180) + 180) % 180;
            for (int d = -8; d <= 8; ++d) {
                const int t = (phi + d + thetas) % thetas;
                const int r = int(std::lround(x * cosT[t] + y * sinT[t])) + rhoMax;
                ++acc[std::size_t(t) * rhos + r];
            }
        }
    auto votesAt = [&](int t, int r) {
        // Theta wraps around with the sign of rho flipped.
        if (t < 0) { t += thetas; r = rhos - 1 - r; }
        else if (t >= thetas) { t -= thetas; r = rhos - 1 - r; }
        if (r < 0 || r >= rhos) return 0;
        return acc[std::size_t(t) * rhos + r];
    };
    std::vector<Line> peaks;
    for (int t = 0; t < thetas; ++t)
        for (int r = 0; r < rhos; ++r) {
            const int v = acc[std::size_t(t) * rhos + r];
            if (v < minVotes) continue;
            bool peak = true;
            for (int dt = -3; dt <= 3 && peak; ++dt)
                for (int dr = -3; dr <= 3; ++dr) {
                    if (!dt && !dr) continue;
                    const int other = votesAt(t + dt, r + dr);
                    // Ties go to the first cell in scan order.
                    if (other > v || (other == v && (dt < 0 || (dt == 0 && dr < 0)))) { peak = false; break; }
                }
            if (!peak) continue;
            Line line;
            line.theta = t;
            line.nx = cosT[t];
            line.ny = sinT[t];
            line.rho = r - rhoMax;
            line.votes = v;
            peaks.push_back(line);
        }
    std::sort(peaks.begin(), peaks.end(), [](const Line &a, const Line &b) { return a.votes > b.votes; });
    std::vector<Line> lines;
    for (const Line &line : peaks) {
        bool similar = false;
        for (const Line &kept : lines) {
            if (lineAngle(line, kept) > 5) continue;
            // Same orientation: compare offsets, flipped across the wrap.
            const double rho = line.nx * kept.nx + line.ny * kept.ny < 0 ? -line.rho : line.rho;
            if (std::abs(rho - kept.rho) <= 4) { similar = true; break; }
        }
        if (similar) continue;
        lines.push_back(line);
        if (int(lines.size()) >= maxLines) break;
    }
    return lines;
}

// How much of a side (plane pixels) is an edge across it with the inside
// brighter, and with the inside darker, as fractions of its length.
struct SideSupport {
    double brighterInside = 0, darkerInside = 0;
    // The same per fifth of the side, lowest but one: a real side is an
    // edge all along (a finger may cover a fifth); a line that only
    // continues a paper edge past its corner is not.
    double brighterSpread = 0, darkerSpread = 0;
};

const float StepContrast = 8.0f;

SideSupport sideSupport(const Plane &plane, const Edges &e, const QPointF &a, const QPointF &b,
                        const QPointF &centre)
{
    SideSupport support;
    const double len = QLineF(a, b).length();
    if (len < 4) return support;
    const QPointF dir = (b - a) / len;
    QPointF normal(-dir.y(), dir.x());
    if (dot(normal, (a + b) / 2 - centre) < 0) normal = -normal; // outward
    const int count = std::max(10, int(len * 0.9));
    int brighter = 0, darker = 0;
    int binBrighter[5] = {}, binDarker[5] = {}, binCount[5] = {};
    for (int i = 0; i < count; ++i) {
        const int bin = i * 5 / count;
        ++binCount[bin];
        // Corners left out: there the sides meet and gradients turn.
        const double t = 0.06 + 0.88 * (i + 0.5) / count;
        const QPointF s = a + (b - a) * t;
        bool edge = false;
        for (double o : {0.0, -1.0, 1.0}) {
            const QPointF q = s + normal * o;
            const int x = int(std::lround(q.x())), y = int(std::lround(q.y()));
            if (x < 0 || y < 0 || x >= e.w || y >= e.h) continue;
            const std::size_t k = std::size_t(y) * e.w + x;
            if (!e.edge[k]) continue;
            const double across = (e.gx[k] * normal.x() + e.gy[k] * normal.y())
                    / std::max(1e-6f, std::hypot(e.gx[k], e.gy[k]));
            if (std::abs(across) >= 0.92) { edge = true; break; }
        }
        if (!edge) continue;
        // Clear of a printed line on either side of it (whose edge the
        // side may run along), well inside a page's margin.
        const float inside = plane.sample(s.x() - 4 * normal.x(), s.y() - 4 * normal.y());
        const float outside = plane.sample(s.x() + 4 * normal.x(), s.y() + 4 * normal.y());
        if (inside - outside >= StepContrast) { ++brighter; ++binBrighter[bin]; }
        else if (outside - inside >= StepContrast) { ++darker; ++binDarker[bin]; }
    }
    support.brighterInside = double(brighter) / count;
    support.darkerInside = double(darker) / count;
    double bright[5], dark[5];
    for (int b = 0; b < 5; ++b) {
        bright[b] = binCount[b] ? double(binBrighter[b]) / binCount[b] : 0;
        dark[b] = binCount[b] ? double(binDarker[b]) / binCount[b] : 0;
    }
    std::sort(bright, bright + 5);
    std::sort(dark, dark + 5);
    support.brighterSpread = bright[1];
    support.darkerSpread = dark[1];
    return support;
}

double polygonArea(const Quad &q)
{
    double area = 0;
    for (int i = 0; i < 4; ++i) {
        const QPointF &a = q[i], &b = q[(i + 1) % 4];
        area += a.x() * b.y() - b.x() * a.y();
    }
    return area / 2;
}

bool convex(const Quad &q)
{
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = q[(i + 1) % 4] - q[i], b = q[(i + 2) % 4] - q[(i + 1) % 4];
        const double cross = a.x() * b.y() - a.y() * b.x();
        const int s = cross > 0 ? 1 : cross < 0 ? -1 : 0;
        if (!s || (sign && s != sign)) return false;
        sign = s;
    }
    return true;
}

double meanCornerDistance(const Quad &a, const Quad &b)
{
    double sum = 0;
    for (int i = 0; i < 4; ++i)
        sum += length(a[i] - b[i]);
    return sum / 4;
}

QPointF centreOf(const Quad &q)
{
    return (q[0] + q[1] + q[2] + q[3]) / 4;
}

// Bilinear in an 8-bit gray image; pixel i covers [i, i + 1).
double graySample(const QImage &gray, double x, double y)
{
    x = std::min(std::max(x - 0.5, 0.0), gray.width() - 1.001);
    y = std::min(std::max(y - 0.5, 0.0), gray.height() - 1.001);
    const int x0 = int(x), y0 = int(y);
    const double fx = x - x0, fy = y - y0;
    const uchar *a = gray.constScanLine(y0) + x0, *b = gray.constScanLine(y0 + 1) + x0;
    return (a[0] * (1 - fx) + a[1] * fx) * (1 - fy) + (b[0] * (1 - fx) + b[1] * fx) * fy;
}

struct FittedLine {
    QPointF point, direction;
};

// Total least squares, dropping points over 1.5 px off and fitting again.
bool fitLine(std::vector<QPointF> points, FittedLine *line)
{
    for (int pass = 0; pass < 2; ++pass) {
        if (points.size() < 10) return false;
        QPointF mean;
        for (const QPointF &p : points) mean += p;
        mean /= double(points.size());
        double sxx = 0, syy = 0, sxy = 0;
        for (const QPointF &p : points) {
            const QPointF d = p - mean;
            sxx += d.x() * d.x();
            syy += d.y() * d.y();
            sxy += d.x() * d.y();
        }
        const double angle = 0.5 * std::atan2(2 * sxy, sxx - syy);
        line->point = mean;
        line->direction = QPointF(std::cos(angle), std::sin(angle));
        if (pass == 0) {
            const QPointF normal(-line->direction.y(), line->direction.x());
            points.erase(std::remove_if(points.begin(), points.end(), [&](const QPointF &p) {
                return std::abs(dot(p - mean, normal)) > 1.5;
            }), points.end());
        }
    }
    return true;
}

bool intersect(const FittedLine &a, const FittedLine &b, QPointF *point)
{
    const double det = a.direction.x() * b.direction.y() - a.direction.y() * b.direction.x();
    if (std::abs(det) < 1e-9) return false;
    const QPointF d = b.point - a.point;
    const double t = (d.x() * b.direction.y() - d.y() * b.direction.x()) / det;
    *point = a.point + a.direction * t;
    return true;
}

} // namespace

std::vector<QrCode> findQrCodes(const PreviewAnalysis::LumaView &luma)
{
    std::vector<QrCode> codes;
    if (!luma.data || luma.width < 21 || luma.height < 21
            || qint64(luma.width) * luma.height > 16 * 1024 * 1024)
        return codes;
    std::unique_ptr<quirc, void (*)(quirc *)> decoder(quirc_new(), quirc_destroy);
    if (!decoder || quirc_resize(decoder.get(), luma.width, luma.height) < 0)
        return codes;
    binarize(luma, quirc_begin(decoder.get(), nullptr, nullptr));
    quirc_end(decoder.get());
    const int count = quirc_count(decoder.get());
    std::unique_ptr<quirc_code> code(new quirc_code);
    std::unique_ptr<quirc_data> data(new quirc_data);
    for (int i = 0; i < count; ++i) {
        quirc_extract(decoder.get(), i, code.get());
        quirc_decode_error_t error = quirc_decode(code.get(), data.get());
        if (error == QUIRC_ERROR_FORMAT_ECC || error == QUIRC_ERROR_DATA_ECC) {
            // Perhaps mirrored (seen through glass, or printed so).
            quirc_flip(code.get());
            error = quirc_decode(code.get(), data.get());
        }
        if (error != QUIRC_SUCCESS || data->payload_len <= 0)
            continue;
        QrCode result;
        result.text = payloadText(*data);
        for (int k = 0; k < 4; ++k)
            result.corners[k] = QPointF((code->corners[k].x + 0.5) / luma.width,
                                        (code->corners[k].y + 0.5) / luma.height);
        const bool duplicate = std::any_of(codes.begin(), codes.end(), [&](const QrCode &other) {
            return other.text == result.text;
        });
        if (!duplicate)
            codes.push_back(result);
    }
    return codes;
}

Quad clockwise(Quad corners)
{
    const QPointF centre = centreOf(corners);
    // y points down, so increasing angle runs clockwise on screen.
    std::sort(corners.begin(), corners.end(), [&](const QPointF &a, const QPointF &b) {
        return std::atan2(a.y() - centre.y(), a.x() - centre.x())
                < std::atan2(b.y() - centre.y(), b.x() - centre.x());
    });
    int first = 0;
    for (int i = 1; i < 4; ++i)
        if (corners[i].x() + corners[i].y() < corners[first].x() + corners[first].y())
            first = i;
    std::rotate(corners.begin(), corners.begin() + first, corners.end());
    return corners;
}

bool findDocument(const PreviewAnalysis::LumaView &luma, Document *document, const Quad *near, int maxSide)
{
    if (!luma.data || luma.width < 32 || luma.height < 32 || maxSide < 32)
        return false;
    int factor = 1;
    const Plane plane = blurred(downscaled(luma, maxSide, &factor));
    if (plane.w < 32 || plane.h < 32)
        return false;
    const Edges edges = edgesOf(plane);
    const double shortSide = std::min(plane.w, plane.h);
    // Enough lines that a page's edges are among them next to a busy
    // background's (wood grain, tiles).
    const std::vector<Line> lines = houghLines(edges, 28, std::max(12, int(shortSide * 0.12)));
    if (lines.size() < 4)
        return false;

    // Plane pixel (index) coordinates <-> normalised frame coordinates.
    const double scaleX = double(factor) / luma.width, scaleY = double(factor) / luma.height;
    auto toPlane = [&](const QPointF &n) { return QPointF(n.x() / scaleX - 0.5, n.y() / scaleY - 0.5); };
    auto toFrame = [&](const QPointF &p) { return QPointF((p.x() + 0.5) * scaleX, (p.y() + 0.5) * scaleY); };
    Quad nearPlane;
    if (near) {
        for (int i = 0; i < 4; ++i)
            nearPlane[i] = toPlane((*near)[i]);
        nearPlane = clockwise(nearPlane);
    }
    const double diagonal = std::hypot(plane.w, plane.h);
    const double nearLimit = 0.08 * diagonal;
    const double frameArea = double(plane.w) * plane.h;
    const double margin = 0.01 * std::max(plane.w, plane.h);

    // Opposite sides: within 35 degrees of each other (perspective makes
    // them converge) and apart by an eighth of the short side.
    struct Pair { int a, b; };
    std::vector<Pair> pairs;
    const QPointF middle((plane.w - 1) / 2.0, (plane.h - 1) / 2.0);
    for (int i = 0; i < int(lines.size()); ++i)
        for (int j = i + 1; j < int(lines.size()); ++j) {
            if (lineAngle(lines[i], lines[j]) > 35) continue;
            const double di = lines[i].nx * middle.x() + lines[i].ny * middle.y() - lines[i].rho;
            double dj = lines[j].nx * middle.x() + lines[j].ny * middle.y() - lines[j].rho;
            if (lines[i].nx * lines[j].nx + lines[i].ny * lines[j].ny < 0) dj = -dj;
            if (std::abs(di - dj) < shortSide / 8) continue;
            pairs.push_back({i, j});
        }

    bool found = false;
    Document best;
    double bestRank = 0;
    for (std::size_t p = 0; p < pairs.size(); ++p)
        for (std::size_t q = p + 1; q < pairs.size(); ++q) {
            const Line &a = lines[pairs[p].a], &b = lines[pairs[p].b];
            const Line &c = lines[pairs[q].a], &d = lines[pairs[q].b];
            if (pairs[q].a == pairs[p].a || pairs[q].a == pairs[p].b
                    || pairs[q].b == pairs[p].a || pairs[q].b == pairs[p].b)
                continue;
            // Neighbouring sides meet at 45 to 135 degrees.
            if (lineAngle(a, c) < 45 || lineAngle(a, d) < 45 || lineAngle(b, c) < 45 || lineAngle(b, d) < 45)
                continue;
            Quad quad;
            if (!intersect(a, c, &quad[0]) || !intersect(c, b, &quad[1])
                    || !intersect(b, d, &quad[2]) || !intersect(d, a, &quad[3]))
                continue;
            bool inside = true;
            for (const QPointF &corner : quad)
                if (corner.x() < -margin || corner.y() < -margin
                        || corner.x() > plane.w - 1 + margin || corner.y() > plane.h - 1 + margin)
                    inside = false;
            if (!inside || !convex(quad)) continue;
            quad = clockwise(quad);
            const double area = std::abs(polygonArea(quad));
            if (area < frameArea / 8 || area > frameArea * 0.97) continue;
            bool sidesLong = true;
            for (int i = 0; i < 4; ++i)
                if (QLineF(quad[i], quad[(i + 1) % 4]).length() < shortSide * 0.15) sidesLong = false;
            if (!sidesLong) continue;
            double distance = 0;
            if (near) {
                distance = meanCornerDistance(quad, nearPlane);
                if (distance > nearLimit) continue;
            }

            const QPointF centre = centreOf(quad);
            // Thresholds: the viewfinder's, and looser for finding the
            // shown quad again in the photo.
            const double minSide = near ? 0.4 : 0.5, minMean = near ? 0.55 : 0.65;
            const double minSpread = near ? 0.25 : 0.35;
            double brighter[4], darker[4], brighterSpread[4], darkerSpread[4];
            double sumBrighter = 0, sumDarker = 0;
            bool weak = false;
            for (int i = 0; i < 4 && !weak; ++i) {
                const SideSupport s = sideSupport(plane, edges, quad[i], quad[(i + 1) % 4], centre);
                brighter[i] = s.brighterInside;
                darker[i] = s.darkerInside;
                brighterSpread[i] = s.brighterSpread;
                darkerSpread[i] = s.darkerSpread;
                sumBrighter += brighter[i];
                sumDarker += darker[i];
                // Too little of this side is a step either way.
                if (std::max(brighter[i], darker[i]) < minSide) weak = true;
            }
            if (weak) continue;
            const bool paperBrighter = sumBrighter >= sumDarker;
            const double *support = paperBrighter ? brighter : darker;
            const double *spread = paperBrighter ? brighterSpread : darkerSpread;
            double minimum = 1, mean = 0, minimumSpread = 1;
            for (int i = 0; i < 4; ++i) {
                minimum = std::min(minimum, support[i]);
                minimumSpread = std::min(minimumSpread, spread[i]);
                mean += support[i] / 4;
            }
            if (minimum < minSide || mean < minMean || minimumSpread < minSpread) continue;
            // Larger and better outlined wins: the page over a box printed
            // on it; paper usually is the brighter.
            const double score = mean * std::sqrt(minimum) * std::pow(area / frameArea, 0.3)
                    * (paperBrighter ? 1.0 : 0.9);
            const double rank = near ? score * (1 - 0.5 * distance / nearLimit) : score;
            if (!found || rank > bestRank) {
                found = true;
                bestRank = rank;
                best.score = score;
                for (int i = 0; i < 4; ++i)
                    best.corners[i] = toFrame(quad[i]);
            }
        }
    if (found && document)
        *document = best;
    return found;
}

Quad refineCorners(const QImage &gray, const Quad &corners)
{
    if (gray.format() != QImage::Format_Grayscale8 || gray.width() < 16 || gray.height() < 16)
        return corners;
    const double diagonal = std::hypot(gray.width(), gray.height());
    const double reach = std::max(4.0, diagonal * 0.012);
    const QPointF centre = centreOf(corners);
    std::array<FittedLine, 4> sides;
    for (int k = 0; k < 4; ++k) {
        const QPointF a = corners[k], b = corners[(k + 1) % 4];
        const double len = QLineF(a, b).length();
        if (len < 16) return corners;
        const QPointF dir = (b - a) / len;
        QPointF normal(-dir.y(), dir.x());
        if (dot(normal, (a + b) / 2 - centre) < 0) normal = -normal;
        // Per sample along the side, the strongest rise and fall across it
        // (half-pixel steps, averaged over 3 px along the side).
        const int steps = int(std::ceil(reach * 2));
        struct Candidate { QPointF point; double strength; };
        std::vector<Candidate> rises, falls;
        const int samples = 40;
        std::vector<double> profile(2 * steps + 1);
        for (int i = 0; i < samples; ++i) {
            const QPointF s = a + (b - a) * (0.1 + 0.8 * (i + 0.5) / samples);
            for (int j = -steps; j <= steps; ++j) {
                const QPointF q = s + normal * (j * 0.5);
                profile[j + steps] = (graySample(gray, q.x() - 1.5 * dir.x(), q.y() - 1.5 * dir.y())
                                      + graySample(gray, q.x(), q.y())
                                      + graySample(gray, q.x() + 1.5 * dir.x(), q.y() + 1.5 * dir.y())) / 3;
            }
            int up = 1, down = 1;
            std::vector<double> slope(profile.size(), 0.0);
            for (int j = 1; j + 1 < int(profile.size()); ++j) {
                slope[j] = profile[j + 1] - profile[j - 1];
                if (slope[j] > slope[up]) up = j;
                if (slope[j] < slope[down]) down = j;
            }
            auto peak = [&](int j, double sign) {
                // Parabola through the slope's peak for the sub-step offset.
                double offset = 0;
                if (j > 1 && j + 2 < int(profile.size())) {
                    const double l = sign * slope[j - 1], c = sign * slope[j], r = sign * slope[j + 1];
                    const double den = l - 2 * c + r;
                    if (den < 0) offset = std::max(-0.5, std::min(0.5, 0.5 * (l - r) / den));
                }
                return Candidate{s + normal * ((j - steps + offset) * 0.5), sign * slope[j]};
            };
            rises.push_back(peak(up, 1));
            falls.push_back(peak(down, -1));
        }
        // The side's own step: brighter outside (rise) or inside (fall).
        double rise = 0, fall = 0;
        for (const Candidate &c : rises) rise += c.strength;
        for (const Candidate &c : falls) fall += c.strength;
        std::vector<QPointF> points;
        for (const Candidate &c : rise > fall ? rises : falls)
            if (c.strength >= 4) points.push_back(c.point);
        if (!fitLine(points, &sides[k])) return corners;
    }
    Quad refined;
    for (int k = 0; k < 4; ++k) {
        // Corner k joins the side ending there and the side starting there.
        if (!intersect(sides[(k + 3) % 4], sides[k], &refined[k])) return corners;
        if (length(refined[k] - corners[k]) > diagonal * 0.025) return corners;
    }
    return refined;
}

double rectangleAspect(const Quad &c, const QSizeF &imageSize, double focalLength)
{
    struct V { double x, y, z; };
    auto cross = [](const V &a, const V &b) {
        return V{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    auto dot3 = [](const V &a, const V &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const double cx = imageSize.width() / 2, cy = imageSize.height() / 2;
    auto h = [&](const QPointF &p) { return V{p.x() - cx, p.y() - cy, 1.0}; };
    // Zhang and He's m1..m4: top-left, top-right, bottom-left, bottom-right.
    const V m1 = h(c[0]), m2 = h(c[1]), m3 = h(c[3]), m4 = h(c[2]);
    const double k2 = dot3(cross(m1, m4), m3) / dot3(cross(m2, m4), m3);
    const double k3 = dot3(cross(m1, m4), m2) / dot3(cross(m3, m4), m2);
    const V n2{k2 * m2.x - m1.x, k2 * m2.y - m1.y, k2 * m2.z - m1.z};
    const V n3{k3 * m3.x - m1.x, k3 * m3.y - m1.y, k3 * m3.z - m1.z};
    const double diagonal = std::hypot(imageSize.width(), imageSize.height());
    double f2 = focalLength > 0 ? focalLength * focalLength : 0;
    if (f2 <= 0) {
        // From the quad's own perspective, which needs both pairs of
        // opposite sides to converge: with a pair parallel (a page tilted
        // straight back) any focal length fits. Then, or when implausible,
        // a typical phone camera's (26 mm equivalent: 0.6 x the diagonal).
        auto angle = [](const QPointF &a, const QPointF &b) {
            const double cosine = std::abs(dot(a, b)) / std::max(1e-9, length(a) * length(b));
            return std::acos(std::min(1.0, cosine)) * 180 / Pi;
        };
        const bool converging = angle(c[1] - c[0], c[2] - c[3]) > 2 && angle(c[3] - c[0], c[2] - c[1]) > 2;
        if (converging && std::abs(n2.z * n3.z) > 1e-12)
            f2 = -(n2.x * n3.x + n2.y * n3.y) / (n2.z * n3.z);
        if (!(f2 > std::pow(0.25 * diagonal, 2) && f2 < std::pow(4 * diagonal, 2)))
            f2 = std::pow(0.6 * diagonal, 2);
    }
    const double width2 = (n2.x * n2.x + n2.y * n2.y) / f2 + n2.z * n2.z;
    const double height2 = (n3.x * n3.x + n3.y * n3.y) / f2 + n3.z * n3.z;
    double aspect = height2 > 0 && width2 > 0 ? std::sqrt(width2 / height2) : 0;
    if (!(aspect > 0.05 && aspect < 20)) {
        // Degenerate: the mean side lengths instead.
        const double w = (QLineF(c[0], c[1]).length() + QLineF(c[3], c[2]).length()) / 2;
        const double hgt = (QLineF(c[0], c[3]).length() + QLineF(c[1], c[2]).length()) / 2;
        aspect = hgt > 0 ? w / hgt : 1;
    }
    return aspect;
}

QImage rectified(const QImage &source, const Quad &c, qint64 maxPixels, double focalLength)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    if (image.isNull() || image.width() < 2 || image.height() < 2)
        return QImage();
    const double top = QLineF(c[0], c[1]).length(), bottom = QLineF(c[3], c[2]).length();
    const double left = QLineF(c[0], c[3]).length(), right = QLineF(c[1], c[2]).length();
    const double aspect = rectangleAspect(c, image.size(), focalLength);
    // The better resolved direction keeps its pixels; the foreshortened one
    // is stretched to the rectangle's proportions.
    double width = std::max(top, bottom), height = std::max(left, right);
    if (width / height > aspect) height = width / aspect;
    else width = height * aspect;
    if (width * height > double(maxPixels)) {
        const double scale = std::sqrt(double(maxPixels) / (width * height));
        width *= scale;
        height *= scale;
    }
    const int outW = std::max(1, int(std::lround(width))), outH = std::max(1, int(std::lround(height)));
    QImage out(outW, outH, QImage::Format_RGB32);
    if (out.isNull())
        return out;

    // Unit square -> quad (Heckbert): (0,0) top-left, (1,0) top-right,
    // (1,1) bottom-right, (0,1) bottom-left.
    const double x0 = c[0].x(), y0 = c[0].y(), x1 = c[1].x(), y1 = c[1].y();
    const double x2 = c[2].x(), y2 = c[2].y(), x3 = c[3].x(), y3 = c[3].y();
    const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    double a, b, cc, d, e, f, g = 0, h = 0;
    if (std::abs(sx) < 1e-9 && std::abs(sy) < 1e-9) {
        a = x1 - x0; b = x3 - x0; cc = x0;
        d = y1 - y0; e = y3 - y0; f = y0;
    } else {
        const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
        const double den = dx1 * dy2 - dx2 * dy1;
        if (std::abs(den) < 1e-12) return QImage();
        g = (sx * dy2 - dx2 * sy) / den;
        h = (dx1 * sy - sx * dy1) / den;
        a = x1 - x0 + g * x1; b = x3 - x0 + h * x3; cc = x0;
        d = y1 - y0 + g * y1; e = y3 - y0 + h * y3; f = y0;
    }
    const int srcW = image.width(), srcH = image.height();
    for (int y = 0; y < outH; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        const double v = (y + 0.5) / outH;
        for (int x = 0; x < outW; ++x) {
            const double u = (x + 0.5) / outW;
            const double w = g * u + h * v + 1;
            // Pixel i covers [i, i + 1): its centre is at i + 0.5.
            double px = (a * u + b * v + cc) / w - 0.5, py = (d * u + e * v + f) / w - 0.5;
            px = std::min(std::max(px, 0.0), srcW - 1.001);
            py = std::min(std::max(py, 0.0), srcH - 1.001);
            const int ix = int(px), iy = int(py);
            const double fx = px - ix, fy = py - iy;
            const QRgb *r0 = reinterpret_cast<const QRgb *>(image.constScanLine(iy)) + ix;
            const QRgb *r1 = reinterpret_cast<const QRgb *>(image.constScanLine(iy + 1)) + ix;
            auto mix = [&](int (*channel)(QRgb)) {
                const double top = channel(r0[0]) * (1 - fx) + channel(r0[1]) * fx;
                const double bottom = channel(r1[0]) * (1 - fx) + channel(r1[1]) * fx;
                return int(std::lround(top * (1 - fy) + bottom * fy));
            };
            line[x] = qRgb(mix(qRed), mix(qGreen), mix(qBlue));
        }
    }
    return out;
}

}
