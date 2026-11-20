// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
#ifndef EXIFUTILS_H
#define EXIFUTILS_H

#include <QJsonObject>
#include <QSize>
#include <QVariantMap>
#include <tiffio.h>

namespace ExifUtils {
int orientation(const QJsonObject &metadata, bool dng);
QJsonObject captureContext(const QVariantMap &values);
QJsonObject mergeCaptureMetadata(const QJsonObject &result, const QJsonObject &context);
bool enrichSidecar(const QString &path, const QJsonObject &context, QString *error);
QByteArray originalJpegExif(const QString &path);
// The JPEG's EXIF for its HEIC ("Exif\0\0" and TIFF data): GPS as the JPEG
// has it, orientation 1 and the HEIC's (upright) size. Empty when the JPEG
// has no EXIF capture date.
QByteArray heicExif(const QString &jpegPath, const QSize &size);
bool writeJpegExifFromJsonFile(const QString &jpegPath, const QString &metadataPath, QString *error);
// sourceJpeg's EXIF (as heicExif() makes it: GPS kept, orientation 1, the
// given size) written into targetJpeg, a new upright image made from it.
// True without writing when the source has no EXIF capture date.
bool copyUprightJpegExif(const QString &sourceJpeg, const QString &targetJpeg, const QSize &size,
                         QString *error);
// The JPEG's 35 mm equivalent focal length (EXIF), 0 without one.
int focalLength35mm(const QString &jpegPath);
bool writeDngMetadata(TIFF *tiff, const QJsonObject &metadata, QString *error);
}
#endif
