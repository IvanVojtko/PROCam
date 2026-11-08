// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause
#ifndef HEICENCODE_H
#define HEICENCODE_H

#include <QByteArray>
#include <QImage>
#include <QString>

// HEIC photos: a finished JPEG becomes a HEIC with the same base name,
// encoded by the hardware HEIC encoder through the Camera2 helper
// (--encode-heic). The JPEG is the intermediate every capture path already
// writes and tags with EXIF; it is removed once the HEIC is in place.
namespace HeicEncode {

// I420 in full range with JPEG's YCbCr matrix (BT.601), which the helper
// tags the HEIC with. The image must have even width and height.
QByteArray i420FromImage(const QImage &image);

// The HEIC quality for a JPEG quality setting: the same or better quality
// in a smaller file.
int qualityForJpegQuality(int jpegQuality);

// Encodes jpegPath to <base>.heic with upright pixels and the JPEG's EXIF
// (see ExifUtils::heicExif), then removes the JPEG. On failure, including a
// JPEG without EXIF capture date or a HEIC without the EXIF, the JPEG stays
// and error says why.
bool convertJpeg(const QString &jpegPath, const QString &helperPath, int quality,
                 QString *heicPath, QString *error);

}

#endif
