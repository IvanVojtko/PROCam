// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// rawframecheck FRAME.json...: whether RAW frames written by
// sfos-camera2-probe --capture (or kept burst frames) hold a whole picture,
// with the check ProCam applies before merging (src/rawframecheck.h).
// The RAW file is the sidecar's raw_path, or PREFIX.raw16/.raw10 next to it
// when the frames were copied off the phone. Exit status 1 if any frame is
// damaged, 2 if one cannot be read.

#include "../../src/rawframecheck.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <vector>

static QString rawFile(const QString &metadataPath, const QJsonObject &m)
{
    const QString recorded = m.value(QStringLiteral("raw_path")).toString();
    if (!recorded.isEmpty() && QFileInfo::exists(recorded))
        return recorded;
    const QFileInfo info(metadataPath);
    const QString base = info.absolutePath() + QLatin1Char('/') + info.completeBaseName();
    for (const char *suffix : {".raw16", ".raw10", ".raw"}) {
        if (QFileInfo::exists(base + QLatin1String(suffix)))
            return base + QLatin1String(suffix);
    }
    if (!recorded.isEmpty()) {
        const QString beside = info.absolutePath() + QLatin1Char('/') + QFileInfo(recorded).fileName();
        if (QFileInfo::exists(beside))
            return beside;
    }
    return QString();
}

int main(int argc, char **argv)
{
    QTextStream out(stdout);
    if (argc < 2) {
        out << "Usage: rawframecheck FRAME.json...\n";
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        const QString path = QString::fromLocal8Bit(argv[i]);
        QFile metadata(path);
        const QJsonObject m = metadata.open(QIODevice::ReadOnly)
                ? QJsonDocument::fromJson(metadata.readAll()).object() : QJsonObject();
        const int width = m.value(QStringLiteral("width")).toInt();
        const int height = m.value(QStringLiteral("height")).toInt();
        const int stride = m.value(QStringLiteral("row_stride")).toInt();
        const bool raw10 = m.value(QStringLiteral("format")).toString() == QLatin1String("RAW10")
                || m.value(QStringLiteral("bits_per_sample")).toInt() == 10;
        const QJsonArray blackPattern = m.value(QStringLiteral("black_level_pattern")).toArray();
        QFile raw(rawFile(path, m));
        if (width <= 0 || height <= 0 || blackPattern.size() != 4 || !raw.open(QIODevice::ReadOnly)
                || stride < (raw10 ? (width + 3) / 4 * 5 : width * 2)
                || raw.size() < qint64(stride) * height) {
            out << path << ": unreadable (no RAW metadata or file)\n";
            status = qMax(status, 2);
            continue;
        }
        const QByteArray bytes = raw.readAll();
        std::vector<uint16_t> pixels(std::size_t(width) * height);
        for (int y = 0; y < height; ++y) {
            const auto *b = reinterpret_cast<const unsigned char *>(bytes.constData()) + qint64(y) * stride;
            uint16_t *row = &pixels[std::size_t(y) * width];
            for (int x = 0; x < width; ++x) {
                row[x] = raw10 ? uint16_t((b[x / 4 * 5 + x % 4] << 2) | ((b[x / 4 * 5 + 4] >> (x % 4 * 2)) & 3))
                               : uint16_t(b[2 * x] | (b[2 * x + 1] << 8));
            }
        }
        double black[4];
        for (int k = 0; k < 4; ++k) black[k] = blackPattern.at(k).toDouble();
        const QString damage = rawFrameDamage(pixels.data(), width, height, black);
        out << path << ": " << width << "x" << height << " "
            << (damage.isEmpty() ? QStringLiteral("ok") : QStringLiteral("DAMAGED, ") + damage) << "\n";
        if (!damage.isEmpty())
            status = qMax(status, 1);
    }
    return status;
}
