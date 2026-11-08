// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "heicencode.h"
#include "exifutils.h"

#include <QAtomicInt>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <unistd.h>

namespace HeicEncode {

static inline uint8_t clampByte(int value)
{
    return uint8_t(std::min(255, std::max(0, value)));
}

QByteArray i420FromImage(const QImage &source)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB888);
    const int width = image.width(), height = image.height();
    if (width < 2 || height < 2 || (width & 1) || (height & 1)) return QByteArray();
    const int chromaWidth = width / 2, chromaHeight = height / 2;
    QByteArray out(width * height + 2 * chromaWidth * chromaHeight, Qt::Uninitialized);
    uint8_t *yPlane = reinterpret_cast<uint8_t *>(out.data());
    uint8_t *uPlane = yPlane + width * height;
    uint8_t *vPlane = uPlane + chromaWidth * chromaHeight;
    // JFIF YCbCr in 16.16 fixed point.
    for (int y = 0; y < height; y += 2) {
        const uint8_t *rows[2] = { image.constScanLine(y), image.constScanLine(y + 1) };
        for (int x = 0; x < width; x += 2) {
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t *p = rows[dy] + 3 * (x + dx);
                    yPlane[(y + dy) * width + x + dx] =
                            clampByte((19595 * p[0] + 38470 * p[1] + 7471 * p[2] + 32768) >> 16);
                    r += p[0]; g += p[1]; b += p[2];
                }
            }
            // The 2x2 average (sum / 4) folded into the shift.
            const int index = (y / 2) * chromaWidth + x / 2;
            uPlane[index] = clampByte(128 + ((-11059 * r - 21709 * g + 32768 * b + 131072) >> 18));
            vPlane[index] = clampByte(128 + ((32768 * r - 27439 * g - 5329 * b + 131072) >> 18));
        }
    }
    return out;
}

int qualityForJpegQuality(int jpegQuality)
{
    // The hardware encoder's scale sits about 8 below JPEG's: its 100 is a
    // little better than JPEG 92 (the default) in two thirds of the size.
    return std::max(50, std::min(100, jpegQuality + 8));
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && file.write(data) == data.size();
}

static bool fileContains(const QString &path, const QByteArray &block)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.readAll().indexOf(block) >= 0;
}

bool convertJpeg(const QString &jpegPath, const QString &helperPath, int quality,
                 QString *heicPath, QString *error)
{
    QImageReader reader(jpegPath);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) {
        *error = QStringLiteral("Cannot read %1: %2").arg(jpegPath, reader.errorString());
        return false;
    }
    // Even sizes for 4:2:0; at most one row or column is lost.
    image = image.copy(0, 0, image.width() & ~1, image.height() & ~1);
    const QByteArray i420 = i420FromImage(image);
    if (i420.isEmpty()) {
        *error = QStringLiteral("Image too small for HEIC");
        return false;
    }
    // No HEIC without the JPEG's EXIF: capture date, location, camera.
    const QByteArray exif = ExifUtils::heicExif(jpegPath, image.size());
    if (exif.isEmpty()) {
        *error = QStringLiteral("No EXIF capture date in %1").arg(jpegPath);
        return false;
    }

    static QAtomicInt counter;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cache);
    const QString stem = QStringLiteral("%1/heic-%2-%3").arg(cache).arg(getpid())
            .arg(counter.fetchAndAddRelaxed(1));
    const QString inputPath = stem + QStringLiteral(".i420");
    const QString exifPath = stem + QStringLiteral(".exif");
    const QFileInfo jpeg(jpegPath);
    const QString target = jpeg.absolutePath() + QLatin1Char('/') + jpeg.completeBaseName()
            + QStringLiteral(".heic");
    // Hidden while written: the photo list only picks up the finished file.
    const QString partial = jpeg.absolutePath() + QStringLiteral("/.") + jpeg.completeBaseName()
            + QStringLiteral(".heic.part");

    bool ok = writeFile(inputPath, i420) && writeFile(exifPath, exif);
    if (!ok) *error = QStringLiteral("Cannot write HEIC input to %1").arg(cache);

    if (ok) {
        QStringList arguments = {
            QStringLiteral("--encode-heic"), inputPath,
            QString::number(image.width()), QString::number(image.height()),
            QString::number(quality), QStringLiteral("0"), partial, exifPath,
        };
        QProcess helper;
        helper.setProcessChannelMode(QProcess::MergedChannels);
        helper.start(helperPath, arguments);
        ok = helper.waitForFinished(30000) && helper.exitStatus() == QProcess::NormalExit
                && helper.exitCode() == 0 && QFileInfo(partial).size() > 0;
        if (!ok) {
            const QByteArray output = helper.readAll();
            const int report = output.lastIndexOf("{\"ok\"");
            *error = QStringLiteral("HEIC encoder failed: %1").arg(
                        QString::fromLocal8Bit(report >= 0 ? output.mid(report).trimmed()
                                                           : output.right(300).trimmed()));
            if (helper.state() != QProcess::NotRunning) helper.kill();
        } else if (!fileContains(partial, exif)) {
            // The muxer stores the block unchanged; an older bridge only
            // logged EXIF it could not store.
            ok = false;
            *error = QStringLiteral("HEIC encoder left out the EXIF");
        }
    }
    QFile::remove(inputPath);
    QFile::remove(exifPath);
    if (ok) {
        QFile::remove(target);
        ok = QFile::rename(partial, target);
        if (!ok) *error = QStringLiteral("Cannot rename %1").arg(partial);
    }
    if (!ok) {
        QFile::remove(partial);
        return false;
    }
    QFile::remove(jpegPath);
    *heicPath = target;
    return true;
}

}
