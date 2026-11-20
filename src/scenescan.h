// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef SCENESCAN_H
#define SCENESCAN_H

// What the viewfinder recognises besides the scene itself: QR codes
// (decoded with quirc) and documents (a sheet of paper, a page, a card:
// a quadrilateral whose four sides are brightness steps against what is
// behind it), and the perspective correction that turns a photo of a
// document into a flat, upright scan. No Qt Quick, so the host tests can
// run it.

#include "previewanalysis.h"

#include <QImage>
#include <QPointF>
#include <QSizeF>
#include <QString>

#include <array>
#include <vector>

namespace SceneScan {

using Quad = std::array<QPointF, 4>;

struct QrCode {
    QString text;
    // Normalised to the frame (0..1), in the code's own order (its
    // top-left first, clockwise).
    Quad corners;
};

// The QR codes in an 8-bit luma frame.
std::vector<QrCode> findQrCodes(const PreviewAnalysis::LumaView &luma);

struct Document {
    // Normalised to the frame (0..1), clockwise from the corner nearest
    // the frame's top-left.
    Quad corners;
    double score = 0;
};

// The most likely document in an 8-bit luma frame, analysed at maxSide
// pixels on the long side: a convex quadrilateral covering at least an
// eighth of the frame, all four sides brightness steps of the same sign
// (paper brighter or darker than what is behind it all round; drawn lines
// and printed boxes have the same brightness on both sides and do not
// count). With near (normalised, as Document::corners), only quads whose
// corners lie within about 8% of the frame diagonal of near's count: the
// capture finds the document the viewfinder showed again.
bool findDocument(const PreviewAnalysis::LumaView &luma, Document *document,
                  const Quad *near = nullptr, int maxSide = 320);

// The same four points, clockwise (y down) from the one with the smallest
// x + y.
Quad clockwise(Quad corners);

// Corners (pixels of gray, clockwise from the top-left) moved onto the
// brightness step along each side, with lines fitted to sub-pixel edge
// points: the viewfinder's quad is a few pixels off at photo resolution.
// Returns corners unchanged when a side has no clear step.
Quad refineCorners(const QImage &gray, const Quad &corners);

// Width / height of the flat rectangle that a camera with its principal
// point at the image centre sees as corners (pixels, clockwise from the
// top-left; Zhang and He, "Whiteboard scanning and image enhancement",
// 2007). focalLength in pixels, or 0 to estimate it from the quad.
double rectangleAspect(const Quad &corners, const QSizeF &imageSize, double focalLength = 0);

// The quad (pixels, clockwise from the top-left) as an upright rectangle
// of the rectangle's own proportions, at about the resolution the photo
// has there and at most maxPixels.
QImage rectified(const QImage &image, const Quad &corners, qint64 maxPixels, double focalLength = 0);

}

#endif
