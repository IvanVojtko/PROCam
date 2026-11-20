// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

// SceneScan on rendered scenes: a QR code seen at an angle in uneven light,
// a page on a textured desk, a printed box that must not count as a page,
// and a page photographed through a pinhole camera turned back into a flat
// A4 rectangle.

#include "../src/scenescan.h"

#include <QPainter>
#include <QPolygonF>
#include <QTransform>

#include <cmath>
#include <cstdio>
#include <random>

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

// "https://example.org/menu?table=12", version 3, level M.
static const char *const QrModules[] = {
    "#######...#..###..#...#######",
    "#.....#.#.###..#.#..#.#.....#",
    "#.###.#.#####.#.#.#.#.#.###.#",
    "#.###.#.#.#.#.##.#..#.#.###.#",
    "#.###.#..###.###...##.#.###.#",
    "#.....#....####.#...#.#.....#",
    "#######.#.#.#.#.#.#.#.#######",
    "........##..##......#........",
    "#.....#.###.#.##.###.##..###.",
    ".##.#..#.####.#..#.#.#.##.##.",
    "#.#..##.##.......#..##.##....",
    "###.##......#.###..##..#.#...",
    "#.....####.####.#...#.##....#",
    "#..##..###.####....#..###..##",
    "#.#..####.#.####....#.#####..",
    "###..#.#.###.####.##..#.#.#.#",
    ".#..#####.###.####.......##..",
    "#####...###.#..#..###.###.###",
    "##....#####..###.####.####..#",
    "#.#.##.##...#.#....#.#.......",
    "#...#.#..#.##..###..#####.###",
    "........###..#.##.###...##...",
    "#######...###########.#.###..",
    "#.....#..#####..#.###...#..#.",
    "#.###.#...#.##.....#######.##",
    "#.###.#..#.#...#.####..#.##.#",
    "#.###.#..###..###..#########.",
    "#.....#...#.#.#...#..#.####.#",
    "#######.#.#.##.#.#.#.####.#..",
};

static PreviewAnalysis::LumaView view(const QImage &gray)
{
    return PreviewAnalysis::LumaView{gray.constBits(), gray.width(), gray.height(),
                                     int(gray.bytesPerLine()), 1};
}

// A textured desk: smooth blotches and grain, light falling off to one side.
static QImage desk(int width, int height, int level, unsigned seed)
{
    std::mt19937 random(seed);
    std::normal_distribution<double> noise(0, 1);
    QImage coarse(16, 12, QImage::Format_RGB32);
    for (int y = 0; y < coarse.height(); ++y)
        for (int x = 0; x < coarse.width(); ++x) {
            const int v = qBound(0, int(level + 18 * noise(random)), 255);
            coarse.setPixel(x, y, qRgb(v, v, v));
        }
    QImage image = coarse.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const double light = 0.85 + 0.3 * x / width;
            const int v = qBound(0, int(qGray(line[x]) * light + 3 * noise(random)), 255);
            line[x] = qRgb(v, v, v);
        }
    }
    return image;
}

static QTransform rectToQuad(const QRectF &rect, const SceneScan::Quad &quad)
{
    QTransform transform;
    QPolygonF from, to;
    from << rect.topLeft() << rect.topRight() << rect.bottomRight() << rect.bottomLeft();
    for (const QPointF &p : quad) to << p;
    QTransform::quadToQuad(from, to, transform);
    return transform;
}

// A white page with text-like bars, a printed box and a dark marker in its
// top-left corner, drawn in the page's own coordinates (size: page units).
static void drawPage(QPainter &painter, const QSizeF &size)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(236, 236, 236));
    painter.drawRect(QRectF(QPointF(0, 0), size));
    painter.setBrush(QColor(50, 50, 50));
    for (double y = size.height() * 0.15; y < size.height() * 0.85; y += size.height() / 28)
        painter.drawRect(QRectF(size.width() * 0.12, y, size.width() * 0.76, size.height() / 120));
    painter.drawRect(QRectF(size.width() * 0.05, size.height() * 0.03, size.width() * 0.1, size.height() * 0.05));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(40, 40, 40), size.width() / 200));
    painter.drawRect(QRectF(size.width() * 0.55, size.height() * 0.6, size.width() * 0.3, size.height() * 0.2));
}

static double worstCornerError(const SceneScan::Quad &found, const SceneScan::Quad &truth, const QSizeF &scale)
{
    double worst = 0;
    for (int i = 0; i < 4; ++i)
        worst = std::max(worst, std::hypot((found[i].x() - truth[i].x()) * scale.width(),
                                           (found[i].y() - truth[i].y()) * scale.height()));
    return worst;
}

static void qrCode()
{
    const int modules = int(sizeof(QrModules) / sizeof(QrModules[0]));
    QImage code(modules + 8, modules + 8, QImage::Format_RGB32);
    code.fill(qRgb(235, 235, 235));
    for (int y = 0; y < modules; ++y)
        for (int x = 0; x < modules; ++x)
            if (QrModules[y][x] == '#')
                code.setPixel(x + 4, y + 4, qRgb(30, 30, 30));
    for (int variant = 0; variant < 3; ++variant) {
        QImage scene = desk(1440, 1080, 110, 10 + variant);
        QPainter painter(&scene);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        // Turned and seen at an angle (the far side shorter).
        const double side = 300 + 120 * variant, cx = 600 + 150 * variant, cy = 500;
        const double angle = (-25 + 25 * variant) * 3.14159265 / 180;
        SceneScan::Quad quad;
        const double half[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int i = 0; i < 4; ++i) {
            const double shrink = i < 2 ? 0.85 : 1.0;
            const double x = half[i][0] * side / 2 * shrink, y = half[i][1] * side / 2;
            quad[i] = QPointF(cx + x * std::cos(angle) - y * std::sin(angle),
                              cy + x * std::sin(angle) + y * std::cos(angle));
        }
        painter.setTransform(rectToQuad(QRectF(0, 0, code.width(), code.height()), quad));
        painter.drawImage(0, 0, code);
        painter.end();
        const QImage gray = scene.convertToFormat(QImage::Format_Grayscale8);
        const std::vector<SceneScan::QrCode> codes = SceneScan::findQrCodes(view(gray));
        CHECK(codes.size() == 1);
        if (codes.size() == 1) {
            CHECK(codes[0].text == QStringLiteral("https://example.org/menu?table=12"));
            // Its corners lie on the code (inside the drawn quiet zone).
            const QPointF centre = (quad[0] + quad[1] + quad[2] + quad[3]) / 4;
            for (const QPointF &corner : codes[0].corners) {
                const QPointF pixel(corner.x() * gray.width(), corner.y() * gray.height());
                CHECK(std::hypot(pixel.x() - centre.x(), pixel.y() - centre.y()) < side * 0.75);
            }
        }
    }
    // Nothing in a scene without a code.
    const QImage empty = desk(1440, 1080, 110, 3).convertToFormat(QImage::Format_Grayscale8);
    CHECK(SceneScan::findQrCodes(view(empty)).empty());
}

static void documentOnDesk()
{
    const QSizeF page(210, 297);
    for (int variant = 0; variant < 4; ++variant) {
        QImage scene = desk(1920, 1440, variant % 2 ? 70 : 120, 20 + variant);
        const double angle = (-20 + 13 * variant) * 3.14159265 / 180;
        const double cx = 900 + 40 * variant, cy = 720, scale = 3.0 + 0.3 * variant;
        SceneScan::Quad truth;
        const QPointF corners[4] = {{0, 0}, {page.width(), 0}, {page.width(), page.height()}, {0, page.height()}};
        for (int i = 0; i < 4; ++i) {
            const double x = (corners[i].x() - page.width() / 2) * scale * (i < 2 ? 0.9 : 1.0);
            const double y = (corners[i].y() - page.height() / 2) * scale;
            truth[i] = QPointF(cx + x * std::cos(angle) - y * std::sin(angle),
                               cy + x * std::sin(angle) + y * std::cos(angle));
        }
        QPainter painter(&scene);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setTransform(rectToQuad(QRectF(QPointF(0, 0), page), truth));
        drawPage(painter, page);
        painter.end();
        SceneScan::Quad normalised;
        for (int i = 0; i < 4; ++i)
            normalised[i] = QPointF(truth[i].x() / scene.width(), truth[i].y() / scene.height());
        normalised = SceneScan::clockwise(normalised);
        const QImage gray = scene.convertToFormat(QImage::Format_Grayscale8);
        SceneScan::Document document;
        CHECK(SceneScan::findDocument(view(gray), &document));
        // Within 12 px at 1920: the viewfinder's outline.
        CHECK(worstCornerError(document.corners, normalised, scene.size()) < 12);
        // Found again near the shown quad, as the capture does.
        SceneScan::Document again;
        CHECK(SceneScan::findDocument(view(gray), &again, &normalised, 480));
        CHECK(worstCornerError(again.corners, normalised, scene.size()) < 8);
    }
}

static void notADocument()
{
    // A drawn outline (the same brightness on both sides) and a desk alone.
    QImage scene = desk(1920, 1440, 160, 31);
    QPainter painter(&scene);
    painter.setPen(QPen(QColor(30, 30, 30), 8));
    painter.drawRect(QRectF(500, 300, 900, 800));
    painter.end();
    CHECK(!SceneScan::findDocument(view(scene.convertToFormat(QImage::Format_Grayscale8)), nullptr));
    CHECK(!SceneScan::findDocument(view(desk(1920, 1440, 90, 32).convertToFormat(QImage::Format_Grayscale8)),
                                   nullptr));
}

// A4 page tilted back and turned, projected through a 26 mm-equivalent
// pinhole camera: the corners refine onto the page's edges and the scan
// comes out A4-shaped and upright (marker top-left).
static void pageFromPhoto()
{
    const int width = 2400, height = 1800;
    const double focal = 0.6 * std::hypot(width, height);
    struct Pose { double tilt, turn, roll; };
    for (const Pose &pose : {Pose{0, 0, 4}, Pose{30, 0, -8}, Pose{20, 20, 6}, Pose{40, 0, 0}}) {
        const double pageW = 210, pageH = 297;
        const double rx = pose.tilt * 3.14159265 / 180, ry = pose.turn * 3.14159265 / 180;
        const double rz = pose.roll * 3.14159265 / 180;
        const double distance = pageH * focal / (0.62 * height);
        SceneScan::Quad truth;
        const double local[4][2] = {{-pageW / 2, -pageH / 2}, {pageW / 2, -pageH / 2},
                                    {pageW / 2, pageH / 2}, {-pageW / 2, pageH / 2}};
        for (int i = 0; i < 4; ++i) {
            // Roll, then tilt about x, then turn about y.
            double x = local[i][0] * std::cos(rz) - local[i][1] * std::sin(rz);
            double y = local[i][0] * std::sin(rz) + local[i][1] * std::cos(rz), z = 0;
            const double y1 = y * std::cos(rx) - z * std::sin(rx), z1 = y * std::sin(rx) + z * std::cos(rx);
            const double x2 = x * std::cos(ry) + z1 * std::sin(ry), z2 = -x * std::sin(ry) + z1 * std::cos(ry);
            truth[i] = QPointF(width / 2.0 + focal * x2 / (z2 + distance),
                               height / 2.0 + focal * y1 / (z2 + distance));
        }
        QImage scene = desk(width, height, 100, 40);
        QPainter painter(&scene);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setTransform(rectToQuad(QRectF(0, 0, pageW, pageH), truth));
        drawPage(painter, QSizeF(pageW, pageH));
        painter.end();

        const QImage gray = scene.convertToFormat(QImage::Format_Grayscale8);
        SceneScan::Quad rough;
        for (int i = 0; i < 4; ++i)
            rough[i] = truth[i] + QPointF(i % 2 ? 4 : -3, i < 2 ? 3 : -4);
        const SceneScan::Quad refined = SceneScan::refineCorners(gray, rough);
        CHECK(worstCornerError(refined, truth, QSizeF(1, 1)) < 1.5);
        CHECK(std::abs(SceneScan::rectangleAspect(refined, scene.size()) - pageW / pageH) < 0.03);
        CHECK(std::abs(SceneScan::rectangleAspect(refined, scene.size(), focal) - pageW / pageH) < 0.02);

        const QImage scan = SceneScan::rectified(scene, refined, 4000000);
        CHECK(!scan.isNull());
        CHECK(std::abs(double(scan.width()) / scan.height() - pageW / pageH) < 0.03);
        // Marker (dark) top-left; paper (light) top-right and in the margins.
        auto at = [&](double u, double v) { return qGray(scan.pixel(int(u * scan.width()), int(v * scan.height()))); };
        CHECK(at(0.1, 0.055) < 100);
        CHECK(at(0.9, 0.055) > 180);
        CHECK(at(0.03, 0.5) > 180 && at(0.97, 0.5) > 180 && at(0.5, 0.97) > 180);
    }
}

int main()
{
    qrCode();
    documentOnDesk();
    notADocument();
    pageFromPhoto();
    if (failures)
        std::fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
