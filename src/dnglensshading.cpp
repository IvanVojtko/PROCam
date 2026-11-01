/*
 * SPDX-FileCopyrightText: 2026 RAWfish Contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dnglensshading.h"

#include <QDataStream>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>
#include <cstring>

namespace {

// Filesystem-safe identifier for a free-form string (typically a phone
// model). Must match slugify() in tools/calibration/generate_lens_shading.py
// byte-for-byte, since both sides need to construct the exact same file
// name independently: lowercase, any run of characters other than a-z0-9
// collapsed to a single '-', leading/trailing '-' trimmed.
QString slugify(const QString &text)
{
    QString slug = text.toLower();
    static const QRegularExpression nonAlnum(QStringLiteral("[^a-z0-9]+"));
    slug.replace(nonAlnum, QStringLiteral("-"));
    while (slug.startsWith(QLatin1Char('-'))) {
        slug.remove(0, 1);
    }
    while (slug.endsWith(QLatin1Char('-'))) {
        slug.chop(1);
    }
    return slug;
}

// std::gcd() is C++17; this project builds as C++14 (CONFIG += c++14 in
// src.pro), hence this by-hand Euclidean gcd instead.
int gcd(int a, int b)
{
    while (b != 0) {
        const int remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

// GCD-reduced aspect ratio label, e.g. "4x3", "16x9". Must match
// ratio_label() in tools/calibration/generate_lens_shading.py.
QString ratioLabel(int width, int height)
{
    const int divisor = gcd(width, height);
    return QStringLiteral("%1x%2")
            .arg(divisor > 0 ? width / divisor : width)
            .arg(divisor > 0 ? height / divisor : height);
}

// lens_shading_<model-slug>_camera<id>_<width>x<height>_<ratio>.json --
// entirely determined by the device/camera/resolution being written, so a
// calibration for another phone or another resolution is structurally
// impossible to pick up by accident, regardless of what else happens to sit
// in the same calibration directory.
QString calibrationFileName(const QString &deviceModel, const QString &cameraId,
                             int width, int height)
{
    return QStringLiteral("lens_shading_%1_camera%2_%3x%4_%5.json")
            .arg(slugify(deviceModel), cameraId)
            .arg(width)
            .arg(height)
            .arg(ratioLabel(width, height));
}

}

namespace {

// DNG opcode id for GainMap (DNG specification, "Opcode List Overview").
const quint32 OpcodeIdGainMap = 9;
// DNG version, one byte per component: 1.3.0.0 -> 0x01030000. GainMap was
// introduced in DNG 1.3, so the opcodes are tagged with that version.
const quint32 DngSpecVersion_1_3_0_0 = 0x01030000;
// Bit 0 of OpcodeFlags: readers that don't understand this opcode may skip
// it and still produce a usable (uncorrected) image, rather than rejecting
// the file outright.
const quint32 OpcodeFlagOptional = 1;

// Maps each of the four CFA phases, visited in (row, col) =
// (0,0),(0,1),(1,0),(1,1) order, to the plane name used in the calibration
// JSON. Must be kept in sync with cfaPatternBytes() in
// declarativecameraextensions.cpp and with cfa_phase_names() in
// tools/calibration/generate_lens_shading.py.
QStringList cfaPhaseNames(const QString &cfa)
{
    if (cfa == QLatin1String("RGGB")) {
        return { QStringLiteral("R"), QStringLiteral("Gr"), QStringLiteral("Gb"), QStringLiteral("B") };
    } else if (cfa == QLatin1String("GRBG")) {
        return { QStringLiteral("Gr"), QStringLiteral("R"), QStringLiteral("B"), QStringLiteral("Gb") };
    } else if (cfa == QLatin1String("GBRG")) {
        return { QStringLiteral("Gb"), QStringLiteral("B"), QStringLiteral("R"), QStringLiteral("Gr") };
    } else if (cfa == QLatin1String("BGGR")) {
        return { QStringLiteral("B"), QStringLiteral("Gb"), QStringLiteral("Gr"), QStringLiteral("R") };
    }
    return {};
}

// QDataStream::operator<<(float) on at least some Qt 5 builds (observed:
// Qt 5.15.13) silently promotes its argument to double and writes 8 bytes
// instead of the 4-byte IEEE-754 float the DNG spec requires for MapGains,
// which corrupts every opcode after the first (the declared ParameterSize
// no longer matches what was actually written, so readers desync trying to
// find the next opcode). Bypass the ambiguous overload entirely by writing
// the float's raw 4-byte representation as a quint32.
quint32 floatBitsBigEndian(float value)
{
    quint32 bits;
    static_assert(sizeof(bits) == sizeof(value), "unexpected float size");
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void writeGainMapOpcode(QDataStream &stream, quint32 top, quint32 left,
                        quint32 bottom, quint32 right, quint32 mapPointsV,
                        quint32 mapPointsH, double mapSpacingV, double mapSpacingH,
                        const QVector<float> &mapGains)
{
    const quint32 parameterSize =
            10 * sizeof(quint32) + 4 * sizeof(double) + sizeof(quint32)
            + quint32(mapGains.size()) * sizeof(float);

    stream << OpcodeIdGainMap;
    stream << DngSpecVersion_1_3_0_0;
    stream << OpcodeFlagOptional;
    stream << parameterSize;

    stream << top << left << bottom << right;
    stream << quint32(0) /* Plane */ << quint32(1) /* Planes */;
    stream << quint32(2) /* RowPitch */ << quint32(2) /* ColPitch */;
    stream << mapPointsV << mapPointsH;
    stream << mapSpacingV << mapSpacingH;
    stream << double(0.0) /* MapOriginV */ << double(0.0) /* MapOriginH */;
    stream << quint32(1) /* MapPlanes */;
    for (float gain : mapGains) {
        stream << floatBitsBigEndian(gain);
    }
}

}

namespace DngLensShading {

bool loadGainGrid(const QString &calibrationDir, const QString &deviceModel,
                  const QString &cameraId, const QString &cfaPattern,
                  int width, int height, GainGrid *grid, QString *warning)
{
    const QString path = calibrationDir + QLatin1Char('/')
            + calibrationFileName(deviceModel, cameraId, width, height);
    QFile file(path);
    if (!file.exists()) {
        // No calibration for this camera: not an error, just nothing to add.
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *warning = QStringLiteral("Cannot read lens shading calibration: %1").arg(path);
        return false;
    }

    QJsonParseError parseError;
    const QJsonObject calibration =
            QJsonDocument::fromJson(file.readAll(), &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        *warning = QStringLiteral("Lens shading calibration is not valid JSON: %1").arg(path);
        return false;
    }

    if (calibration.value(QStringLiteral("version")).toInt() != 1) {
        *warning = QStringLiteral("Unsupported lens shading calibration version: %1").arg(path);
        return false;
    }
    if (calibration.value(QStringLiteral("cfa_pattern")).toString() != cfaPattern) {
        *warning = QStringLiteral("Lens shading calibration CFA pattern does not match capture: %1").arg(path);
        return false;
    }
    if (calibration.value(QStringLiteral("image_width")).toInt() != width
            || calibration.value(QStringLiteral("image_height")).toInt() != height) {
        // Most likely a calibration captured at a different resolution/binning
        // mode than the current capture: applying it would misalign the grid.
        *warning = QStringLiteral("Lens shading calibration resolution does not match capture: %1").arg(path);
        return false;
    }

    const int gridRows = calibration.value(QStringLiteral("grid_rows")).toInt();
    const int gridCols = calibration.value(QStringLiteral("grid_cols")).toInt();
    if (gridRows < 2 || gridCols < 2) {
        *warning = QStringLiteral("Lens shading calibration grid is too small: %1").arg(path);
        return false;
    }

    const QStringList phaseNames = cfaPhaseNames(cfaPattern);
    if (phaseNames.size() != 4) {
        *warning = QStringLiteral("Unsupported CFA pattern for lens shading: %1").arg(cfaPattern);
        return false;
    }

    const QJsonObject planes = calibration.value(QStringLiteral("planes")).toObject();
    for (int phase = 0; phase < 4; ++phase) {
        const QString &name = phaseNames.at(phase);
        const QJsonArray values = planes.value(name).toArray();
        if (values.size() != gridRows * gridCols) {
            *warning = QStringLiteral("Lens shading calibration plane \"%1\" has the wrong size: %2")
                    .arg(name, path);
            return false;
        }
        QVector<float> gains;
        gains.reserve(values.size());
        for (const QJsonValue &value : values) {
            gains.append(float(value.toDouble(1.0)));
        }
        grid->phase[phase] = gains;
    }
    grid->rows = gridRows;
    grid->cols = gridCols;
    return true;
}

QByteArray buildOpcodeList2(const QString &calibrationDir, const QString &deviceModel,
                            const QString &cameraId, const QString &cfaPattern,
                            int width, int height, QString *warning)
{
    GainGrid grid;
    if (!loadGainGrid(calibrationDir, deviceModel, cameraId, cfaPattern, width, height,
                      &grid, warning)) {
        return QByteArray();
    }
    const int gridRows = grid.rows;
    const int gridCols = grid.cols;

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);
    // DNG opcode list data is always big-endian, regardless of the TIFF
    // file's own byte order (DNG spec, "Opcode List" chapter).
    stream.setByteOrder(QDataStream::BigEndian);

    stream << quint32(4); // opcode count: one GainMap per CFA phase

    for (int phase = 0; phase < 4; ++phase) {
        const int top = phase / 2;
        const int left = phase % 2;
        // MapSpacingV/H are fractions of the FULL image extent (imageBounds
        // in Adobe's reference dng_gain_map.cpp, buf_in.height/width in
        // darktable's rawprepare.c), not pixel counts and not relative to
        // this phase's subsampled plane. Pixel-count spacing would keep the
        // map coordinate near zero across the whole image, collapsing the
        // correction to a single grid sample.
        const double spacingV = gridRows > 1 ? 1.0 / double(gridRows - 1) : 1.0;
        const double spacingH = gridCols > 1 ? 1.0 / double(gridCols - 1) : 1.0;

        // Bottom/Right are EXCLUSIVE bounds (equal to the full image
        // height/width), not the index of the last row/column. darktable's
        // GainMap validator (_check_gain_maps in rawprepare.c) rejects the
        // whole set, hiding its "flat field correction" control, unless
        // bottom == image height and right == image width.
        writeGainMapOpcode(stream, quint32(top), quint32(left),
                           quint32(height), quint32(width),
                           quint32(gridRows), quint32(gridCols),
                           spacingV, spacingH,
                           grid.phase[phase]);
    }

    return buffer;
}

}
