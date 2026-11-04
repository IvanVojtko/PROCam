// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <QString>
#include <algorithm>
#include <cstdint>

// Whether a decoded RAW frame holds a whole picture. Some ports' HALs fill
// only part of the buffer they declare: the Xperia 10 III (multi-camera
// mode) writes a binned half-size readout into the first quarter of its
// 4032x3016 RAW16 buffer and leaves the rest unwritten. Merged, that is a
// photo of two squashed copies on black.
//
// A sensor row never lies entirely far below the black level: noise around
// black (64 counts on these sensors) keeps every row near it even in the
// dark. Rows below half the black level are therefore unwritten memory; a
// frame with an eighth of its rows like that is damaged. Cameras that report
// a black level near 0 cannot be told from a dark scene and are not judged.
//
// Returns an empty string for a usable frame, otherwise why it is not.
inline QString rawFrameDamage(const uint16_t *pixels, int width, int height, const double black[4])
{
    const double minBlack = std::min(std::min(black[0], black[1]), std::min(black[2], black[3]));
    if (!pixels || width <= 0 || height <= 0 || minBlack < 8)
        return QString();
    const uint16_t floor = uint16_t(minBlack / 2);
    int unwritten = 0;
    for (int y = 0; y < height; ++y) {
        const uint16_t *row = pixels + std::size_t(y) * width;
        // Most rows end at their first pixel.
        if (std::all_of(row, row + width, [floor](uint16_t v) { return v < floor; }))
            ++unwritten;
    }
    if (unwritten * 8 < height)
        return QString();
    return QStringLiteral("%1 of %2 rows unwritten").arg(unwritten).arg(height);
}
