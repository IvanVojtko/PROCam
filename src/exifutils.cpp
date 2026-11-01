// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "exifutils.h"

#include <QDateTime>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStringList>
#include <QVector>
#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {
// Both backends consume these entries; tag selection and value precedence live here.
struct Field {
    ExifIfd directory;
    unsigned short tag;
    ExifFormat format;
    QVector<double> values;
    QByteArray bytes;
};
using Fields = QList<Field>;

void text(Fields &fields, ExifIfd directory, unsigned short tag, const QString &value)
{
    if (!value.isEmpty()) fields.append({directory, tag, EXIF_FORMAT_ASCII, {}, value.toUtf8() + '\0'});
}
void number(Fields &fields, ExifIfd directory, unsigned short tag, ExifFormat format, double value)
{
    if (!std::isfinite(value)) return;
    const double maximum = format == EXIF_FORMAT_BYTE ? 255.0
            : format == EXIF_FORMAT_SHORT ? 65535.0
            : format == EXIF_FORMAT_SRATIONAL ? 2147483647.0 : 4294967295.0;
    if (std::abs(value) > maximum || (value < 0 && format != EXIF_FORMAT_SRATIONAL)) return;
    fields.append({directory, tag, format, {value}, {}});
}
double positive(const QJsonObject &m, const QStringList &keys)
{
    for (const QString &key : keys) {
        bool ok = false;
        const double value = m.value(key).toVariant().toDouble(&ok);
        if (ok && std::isfinite(value) && value > 0) return value;
    }
    return 0;
}
void positiveTag(Fields &fields, unsigned short tag, ExifFormat format, double value)
{
    if (value > 0) number(fields, EXIF_IFD_EXIF, tag, format, value);
}
QByteArray asciiJson(const QJsonObject &object)
{
    const QString json = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    QByteArray result;
    for (QChar ch : json) {
        if (ch.unicode() < 128) result.append(char(ch.unicode()));
        else result.append(QStringLiteral("\\u%1").arg(ch.unicode(), 4, 16, QLatin1Char('0')).toLatin1());
    }
    return result;
}
QByteArray comment(const QJsonObject &metadata)
{
    QJsonObject details = metadata;
    details.remove(QStringLiteral("_original_exif"));
    details.insert(QStringLiteral("metadata_schema_version"), 1);
    return QByteArray("ASCII\0\0\0", 8) + asciiJson(details);
}
QByteArray boundedComment(QJsonObject metadata, int commentBudget)
{
    QByteArray userComment = comment(metadata);
    // Keep standard tags and core scalars. The complete metadata remains in JSON.
    QJsonArray omitted;
    QStringList bulky;
    for (auto it = metadata.begin(); it != metadata.end(); ++it)
        if ((it.value().isArray() || it.value().isObject()) && it.key() != QLatin1String("requested_settings")) bulky.append(it.key());
    std::sort(bulky.begin(), bulky.end(), [&metadata](const QString &a, const QString &b) {
        return asciiJson(QJsonObject{{a, metadata.value(a)}}).size() > asciiJson(QJsonObject{{b, metadata.value(b)}}).size();
    });
    for (const QString &key : bulky) {
        if (userComment.size() <= commentBudget) break;
        metadata.remove(key); omitted.append(key);
        metadata.insert(QStringLiteral("omitted_fields"), omitted);
        userComment = comment(metadata);
    }
    if (userComment.size() > commentBudget) {
        // Pathological scalar diagnostics must not turn a successful photo into a failure.
        const QStringList core = {"captured_at", "camera_id", "capture_mode", "capture_orientation", "pixels_rotated", "software", "requested_settings"};
        for (const QString &key : metadata.keys()) {
            if (core.contains(key) || key == QLatin1String("omitted_fields") || key == QLatin1String("_original_exif")) continue;
            metadata.remove(key); omitted.append(key);
            metadata.insert(QStringLiteral("omitted_fields"), omitted);
            userComment = comment(metadata);
            if (userComment.size() <= commentBudget) break;
        }
    }
    return userComment.size() <= commentBudget ? userComment : QByteArray();
}
QVector<double> degrees(double value)
{
    value = std::abs(value);
    const double d = std::floor(value);
    const double m = std::floor((value - d) * 60);
    return {d, m, ((value - d) * 60 - m) * 60};
}
bool hasGps(const QJsonObject &m)
{
    const QJsonObject gps = m.value(QStringLiteral("gps")).toObject();
    return m.value(QStringLiteral("save_location")).toBool()
            && gps.value(QStringLiteral("latitude")).isDouble()
            && gps.value(QStringLiteral("longitude")).isDouble()
            && std::isfinite(gps.value(QStringLiteral("latitude")).toDouble())
            && std::isfinite(gps.value(QStringLiteral("longitude")).toDouble())
            && std::abs(gps.value(QStringLiteral("latitude")).toDouble()) <= 90
            && std::abs(gps.value(QStringLiteral("longitude")).toDouble()) <= 180;
}
Fields fieldsFor(const QJsonObject &m, bool dng)
{
    Fields fields;
    const auto photo = EXIF_IFD_EXIF;
    const auto image = EXIF_IFD_0;
    text(fields, image, 0x010f, m.value(QStringLiteral("make")).toString());
    text(fields, image, 0x0110, m.value(QStringLiteral("model")).toString());
    text(fields, image, 0x0131, QStringLiteral("ProCam 1.3.1-5"));
    number(fields, image, 0x0112, EXIF_FORMAT_SHORT, ExifUtils::orientation(m, dng));
    for (const auto &dimension : {qMakePair(QStringLiteral("width"), 0xa002),
                                 qMakePair(QStringLiteral("height"), 0xa003)}) {
        const double value = positive(m, {dimension.first});
        if (value > 0) {
            number(fields, image, dimension.second == 0xa002 ? 0x0100 : 0x0101, EXIF_FORMAT_LONG, value);
            positiveTag(fields, dimension.second, EXIF_FORMAT_LONG, value);
        }
    }
    const QDateTime captured = QDateTime::fromString(m.value(QStringLiteral("captured_at")).toString(), Qt::ISODate);
    if (captured.isValid()) {
        const QString date = captured.toString(QStringLiteral("yyyy:MM:dd HH:mm:ss"));
        text(fields, image, 0x0132, date);
        text(fields, photo, 0x9003, date);
        text(fields, photo, 0x9004, date);
        const QString subsec = captured.toString(QStringLiteral("zzz"));
        for (unsigned short tag : {0x9290, 0x9291, 0x9292}) text(fields, photo, tag, subsec);
        const int minutes = captured.offsetFromUtc() / 60;
        const QString offset = QStringLiteral("%1%2:%3").arg(minutes < 0 ? "-" : "+")
                .arg(std::abs(minutes) / 60, 2, 10, QLatin1Char('0'))
                .arg(std::abs(minutes) % 60, 2, 10, QLatin1Char('0'));
        for (unsigned short tag : {0x9010, 0x9011, 0x9012}) text(fields, photo, tag, offset);
    }
    // Requested values stay in UserComment; standard exposure tags describe measured results.
    positiveTag(fields, 0x829a, EXIF_FORMAT_RATIONAL,
                positive(m, {"exposure_time_ns", "live_exposure_time_ns"}) / 1e9);
    const double iso = positive(m, {"iso", "live_sensor_sensitivity"});
    if (iso > 0) {
        positiveTag(fields, 0x8827, EXIF_FORMAT_SHORT, std::min(iso, 65535.0));
        positiveTag(fields, 0x8833, EXIF_FORMAT_LONG, iso);
        number(fields, photo, 0x8830, EXIF_FORMAT_SHORT, 3); // ISO speed
    }
    positiveTag(fields, 0x829d, EXIF_FORMAT_RATIONAL, positive(m, {"lens_aperture"}));
    positiveTag(fields, 0x920a, EXIF_FORMAT_RATIONAL, positive(m, {"focal_length_mm"}));
    positiveTag(fields, 0xa405, EXIF_FORMAT_SHORT, positive(m, {"focal_length_35mm"}));
    positiveTag(fields, 0xa404, EXIF_FORMAT_RATIONAL, positive(m, {"zoom_ratio"}));
    const double distance = positive(m, {"lens_focus_distance_diopters"});
    if (distance > 0) positiveTag(fields, 0x9206, EXIF_FORMAT_RATIONAL, 1 / distance);
    text(fields, photo, 0xa433, m.value(QStringLiteral("lens_make")).toString());
    text(fields, photo, 0xa434, m.value(QStringLiteral("lens_model")).toString());
    // These keys contain EXIF values, not Android enum values or compensation steps.
    for (const auto &tag : {qMakePair(QStringLiteral("exif_flash"), 0x9209),
                            qMakePair(QStringLiteral("exif_metering_mode"), 0x9207),
                            qMakePair(QStringLiteral("exif_white_balance"), 0xa403),
                            qMakePair(QStringLiteral("exif_exposure_mode"), 0xa402),
                            qMakePair(QStringLiteral("exif_exposure_program"), 0x8822)}) {
        if (m.value(tag.first).isDouble()) number(fields, photo, tag.second, EXIF_FORMAT_SHORT, m.value(tag.first).toDouble());
    }
    if (m.value(QStringLiteral("exposure_compensation_ev")).isDouble())
        number(fields, photo, 0x9204, EXIF_FORMAT_SRATIONAL, m.value(QStringLiteral("exposure_compensation_ev")).toDouble());
    fields.append({photo, 0x9000, EXIF_FORMAT_UNDEFINED, {}, QByteArrayLiteral("0232")});
    // libtiff's built-in UserComment uses a 16-bit count even in DNG.
    fields.append({photo, 0x9286, EXIF_FORMAT_UNDEFINED, {}, dng ? boundedComment(m, 65535) : comment(m)});
    if (hasGps(m)) {
        const QJsonObject gps = m.value(QStringLiteral("gps")).toObject();
        const double latitude = gps.value(QStringLiteral("latitude")).toDouble();
        const double longitude = gps.value(QStringLiteral("longitude")).toDouble();
        fields.append({EXIF_IFD_GPS, 0, EXIF_FORMAT_BYTE, {2, 3, 0, 0}, {}});
        text(fields, EXIF_IFD_GPS, 1, latitude < 0 ? "S" : "N");
        fields.append({EXIF_IFD_GPS, 2, EXIF_FORMAT_RATIONAL, degrees(latitude), {}});
        text(fields, EXIF_IFD_GPS, 3, longitude < 0 ? "W" : "E");
        fields.append({EXIF_IFD_GPS, 4, EXIF_FORMAT_RATIONAL, degrees(longitude), {}});
        if (gps.value(QStringLiteral("altitude")).isDouble()) {
            const double altitude = gps.value(QStringLiteral("altitude")).toDouble();
            number(fields, EXIF_IFD_GPS, 5, EXIF_FORMAT_BYTE, altitude < 0 ? 1 : 0);
            number(fields, EXIF_IFD_GPS, 6, EXIF_FORMAT_RATIONAL, std::abs(altitude));
        }
        text(fields, EXIF_IFD_GPS, 18, "WGS-84");
        const QDateTime timestamp = QDateTime::fromString(gps.value(QStringLiteral("timestamp")).toString(), Qt::ISODate).toUTC();
        if (timestamp.isValid()) {
            const QTime time = timestamp.time();
            fields.append({EXIF_IFD_GPS, 7, EXIF_FORMAT_RATIONAL,
                           {double(time.hour()), double(time.minute()), time.second() + time.msec() / 1000.0}, {}});
            text(fields, EXIF_IFD_GPS, 29, timestamp.toString(QStringLiteral("yyyy:MM:dd")));
        }
    }
    return fields;
}
void removeTag(ExifData *data, ExifIfd directory, unsigned short tag)
{
    ExifEntry *entry = exif_content_get_entry(data->ifd[directory], ExifTag(tag));
    if (entry) exif_content_remove_entry(data->ifd[directory], entry);
}
void clearDirectory(ExifData *data, ExifIfd directory)
{
    ExifContent *content = data->ifd[directory];
    while (content->count) exif_content_remove_entry(content, content->entries[0]);
}
void cleanOriginal(ExifData *data)
{
    clearDirectory(data, EXIF_IFD_GPS);
    clearDirectory(data, EXIF_IFD_1); // Old thumbnails no longer describe transformed pixels.
    std::free(data->data);
    data->data = nullptr;
    data->size = 0;
    // Opaque MakerNotes can contain absolute offsets and location; do not relocate them.
    removeTag(data, EXIF_IFD_EXIF, 0x927c);
}
QByteArray serialized(ExifData *data)
{
    unsigned char *bytes = nullptr;
    unsigned int size = 0;
    exif_data_save_data(data, &bytes, &size);
    const QByteArray result(reinterpret_cast<const char *>(bytes), int(size));
    std::free(bytes);
    return result;
}
void put(ExifData *data, const Field &field)
{
    removeTag(data, field.directory, field.tag);
    ExifEntry *entry = exif_entry_new();
    if (!entry) return;
    entry->tag = ExifTag(field.tag);
    entry->format = field.format;
    entry->components = field.bytes.isEmpty() ? field.values.size() : field.bytes.size();
    entry->size = entry->components * exif_format_get_size(field.format);
    entry->data = static_cast<unsigned char *>(std::calloc(1, entry->size));
    if (!entry->data) { exif_entry_unref(entry); return; }
    if (!field.bytes.isEmpty()) {
        std::copy(field.bytes.constBegin(), field.bytes.constEnd(), entry->data);
    } else {
        const ExifByteOrder order = exif_data_get_byte_order(data);
        for (int i = 0; i < field.values.size(); ++i) {
            unsigned char *p = entry->data + i * exif_format_get_size(field.format);
            const double value = field.values[i];
            switch (field.format) {
            case EXIF_FORMAT_BYTE: *p = static_cast<unsigned char>(value); break;
            case EXIF_FORMAT_SHORT: exif_set_short(p, order, ExifShort(value)); break;
            case EXIF_FORMAT_LONG: exif_set_long(p, order, ExifLong(value)); break;
            case EXIF_FORMAT_RATIONAL:
            case EXIF_FORMAT_SRATIONAL: {
                const bool signedValue = field.format == EXIF_FORMAT_SRATIONAL;
                const double maximum = signedValue ? 2147483647.0 : 4294967295.0;
                const auto denominator = quint32(std::max(1.0, std::min(1e9, std::floor(maximum / std::max(1.0, std::abs(value))))));
                if (signedValue) exif_set_srational(p, order, {ExifSLong(std::llround(value * denominator)), ExifSLong(denominator)});
                else exif_set_rational(p, order, {ExifLong(std::llround(value * denominator)), denominator});
                break;
            }
            default: break;
            }
        }
    }
    exif_content_add_entry(data->ifd[field.directory], entry);
    exif_entry_unref(entry);
}
bool setTiff(TIFF *tiff, const Field &field)
{
    const TIFFField *info = TIFFFieldWithTag(tiff, field.tag);
    if (!info) return true; // Older libtiff may not expose newer EXIF tags; UserComment retains their source.
    if (field.format == EXIF_FORMAT_ASCII) return TIFFSetField(tiff, field.tag, field.bytes.constData());
    const bool counted = TIFFFieldPassCount(info);
    if (field.tag == 0x9286 && field.bytes.isEmpty()) return false;
    if (field.format == EXIF_FORMAT_UNDEFINED) {
        if (counted) {
            if (TIFFFieldWriteCount(info) == TIFF_VARIABLE2)
                return TIFFSetField(tiff, field.tag, uint32_t(field.bytes.size()), field.bytes.constData());
            return TIFFSetField(tiff, field.tag, int(field.bytes.size()), field.bytes.constData());
        }
        return TIFFSetField(tiff, field.tag, field.bytes.constData());
    }
    if (field.format == EXIF_FORMAT_BYTE) {
        QByteArray bytes;
        for (double value : field.values) bytes.append(char(value));
        return field.values.size() == 1 ? TIFFSetField(tiff, field.tag, int(field.values[0]))
                                       : TIFFSetField(tiff, field.tag, bytes.constData());
    }
    if (field.format == EXIF_FORMAT_SHORT) {
        const uint16_t value = uint16_t(field.values[0]);
        return counted ? TIFFSetField(tiff, field.tag, 1, &value) : TIFFSetField(tiff, field.tag, int(value));
    }
    if (field.format == EXIF_FORMAT_LONG) return TIFFSetField(tiff, field.tag, uint32_t(field.values[0]));
    if (field.values.size() == 1) return TIFFSetField(tiff, field.tag, field.values[0]);
    return TIFFSetField(tiff, field.tag, field.values.constData()); // GPS rational arrays use double[].
}
bool rewriteJpeg(const QString &path, const QByteArray &payload, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return false; }
    const QByteArray jpeg = file.readAll();
    if (!jpeg.startsWith(QByteArray::fromHex("ffd8")) || payload.isEmpty() || payload.size() > 65533) {
        *error = QStringLiteral("Invalid JPEG or oversized EXIF block"); return false;
    }
    QByteArray output = jpeg.left(2);
    output.append(QByteArray::fromHex("ffe1"));
    const int length = payload.size() + 2;
    output.append(char(length >> 8)); output.append(char(length & 255)); output.append(payload);
    int offset = 2;
    while (offset + 4 <= jpeg.size() && uchar(jpeg[offset]) == 0xff) {
        const uchar marker = uchar(jpeg[offset + 1]);
        if (marker == 0xda || marker == 0xd9) break;
        if (marker == 0xff) { ++offset; continue; }
        const int size = (uchar(jpeg[offset + 2]) << 8) | uchar(jpeg[offset + 3]);
        if (size < 2 || offset + size + 2 > jpeg.size()) {
            *error = QStringLiteral("Invalid JPEG segment"); return false;
        }
        if (!(marker == 0xe1 && jpeg.mid(offset + 4, 6) == QByteArray("Exif\0\0", 6)))
            output.append(jpeg.mid(offset, size + 2));
        offset += size + 2;
    }
    output.append(jpeg.mid(offset));
    QSaveFile target(path);
    if (!target.open(QIODevice::WriteOnly) || target.write(output) != output.size() || !target.commit()) {
        *error = target.errorString(); return false;
    }
    return true;
}
}

namespace ExifUtils {
int orientation(const QJsonObject &metadata, bool dng)
{
    if (!dng && metadata.value(QStringLiteral("pixels_rotated")).toBool()) return 1;
    const int degrees = metadata.value(QStringLiteral("capture_orientation")).toInt(
                metadata.value(QStringLiteral("jpeg_orientation")).toInt(
                    metadata.value(QStringLiteral("orientation")).toInt()));
    switch (((degrees % 360) + 360) % 360) {
    case 90: return 6;
    case 180: return 3;
    case 270: return 8;
    default: return 1;
    }
}
QJsonObject captureContext(const QVariantMap &values)
{
    QJsonObject context = QJsonObject::fromVariantMap(values);
    const qint64 milliseconds = qint64(context.value(QStringLiteral("capture_time_ms")).toDouble());
    const QDateTime now = milliseconds > 0 ? QDateTime::fromMSecsSinceEpoch(milliseconds) : QDateTime::currentDateTime();
    QString captured = now.toOffsetFromUtc(now.offsetFromUtc()).toString(Qt::ISODate);
    captured.insert(19, QLatin1Char('.') + now.toString(QStringLiteral("zzz")));
    context.insert(QStringLiteral("captured_at"), captured);
    context.insert(QStringLiteral("software"), QStringLiteral("ProCam 1.3.1-5"));
    QFile hardware(QStringLiteral("/etc/hw-release"));
    if (hardware.open(QIODevice::ReadOnly)) {
        while (!hardware.atEnd()) {
            const QString line = QString::fromUtf8(hardware.readLine()).trimmed();
            const int equals = line.indexOf('=');
            const QString key = line.left(equals);
            QString value = line.mid(equals + 1);
            if (value.startsWith('"') && value.endsWith('"')) value = value.mid(1, value.size() - 2);
            if (key == QLatin1String("NAME") && !context.contains(QStringLiteral("model"))) context.insert(QStringLiteral("model"), value);
            if (key == QLatin1String("VENDOR") && !context.contains(QStringLiteral("make"))) context.insert(QStringLiteral("make"), value);
        }
    }
    if (!hasGps(context)) context.remove(QStringLiteral("gps"));
    return context;
}
QJsonObject mergeCaptureMetadata(const QJsonObject &result, const QJsonObject &context)
{
    QJsonObject merged = context;
    for (auto it = result.begin(); it != result.end(); ++it) merged.insert(it.key(), it.value());
    // The capture snapshot is authoritative for consent and location, never old files.
    merged.insert(QStringLiteral("save_location"), context.value(QStringLiteral("save_location")).toBool());
    merged.remove(QStringLiteral("gps"));
    if (hasGps(context)) merged.insert(QStringLiteral("gps"), context.value(QStringLiteral("gps")));
    return merged;
}
bool enrichSidecar(const QString &path, const QJsonObject &context, QString *error)
{
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) { *error = source.errorString(); return false; }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Invalid capture metadata: %1").arg(path); return false;
    }
    const QByteArray bytes = QJsonDocument(mergeCaptureMetadata(document.object(), context)).toJson();
    QSaveFile target(path);
    if (!target.open(QIODevice::WriteOnly) || target.write(bytes) != bytes.size() || !target.commit()) {
        *error = target.errorString(); return false;
    }
    return true;
}
QByteArray originalJpegExif(const QString &path)
{
    ExifData *data = exif_data_new_from_file(QFile::encodeName(path).constData());
    if (!data) return {};
    cleanOriginal(data);
    const QByteArray result = serialized(data);
    exif_data_unref(data);
    return result;
}
bool writeJpegExifFromJsonFile(const QString &path, const QString &metadataPath, QString *error)
{
    QFile file(metadataPath);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return false; }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Invalid EXIF metadata: %1").arg(metadataPath); return false;
    }
    QJsonObject metadata = document.object();
    if (!hasGps(metadata)) metadata.remove(QStringLiteral("gps"));
    const QSize size = QImageReader(path).size();
    if (size.isValid()) {
        metadata.insert(QStringLiteral("width"), size.width());
        metadata.insert(QStringLiteral("height"), size.height());
    }
    QByteArray original = QByteArray::fromBase64(metadata.value(QStringLiteral("_original_exif")).toString().toLatin1());
    if (original.isEmpty()) original = originalJpegExif(path);
    ExifData *data = original.isEmpty() ? exif_data_new() : exif_data_new_from_data(
                reinterpret_cast<const unsigned char *>(original.constData()), original.size());
    if (!data) { *error = QStringLiteral("Cannot allocate EXIF data"); return false; }
    cleanOriginal(data);
    exif_data_set_data_type(data, EXIF_DATA_TYPE_COMPRESSED);
    exif_data_unset_option(data, EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
    for (const Field &field : fieldsFor(metadata, false)) {
        if (field.tag != 0x9286) {
            if (field.tag == 0x0112 && metadata.value(QStringLiteral("preserve_original_orientation")).toBool()
                    && exif_content_get_entry(data->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION)) continue;
            // Native JPEG EXIF is more reliable than a live-preview fallback.
            if (((field.tag == 0x829a && positive(metadata, {"exposure_time_ns"}) == 0)
                 || ((field.tag == 0x8827 || field.tag == 0x8833) && positive(metadata, {"iso"}) == 0))
                    && exif_content_get_entry(data->ifd[field.directory], ExifTag(field.tag))) continue;
            put(data, field);
        }
    }
    removeTag(data, EXIF_IFD_EXIF, 0x9286);
    const int commentBudget = qMax(0, 65533 - serialized(data).size() - 64);
    const QByteArray userComment = boundedComment(metadata, commentBudget);
    if (userComment.isEmpty()) {
        exif_data_unref(data);
        *error = QStringLiteral("EXIF metadata exceeds JPEG capacity");
        return false;
    }
    put(data, {EXIF_IFD_EXIF, 0x9286, EXIF_FORMAT_UNDEFINED, {}, userComment});
    const QByteArray payload = serialized(data);
    exif_data_unref(data);
    return rewriteJpeg(path, payload, error);
}
bool writeDngMetadata(TIFF *tiff, const QJsonObject &metadata, QString *error)
{
    QJsonObject rawMetadata = metadata;
    rawMetadata.insert(QStringLiteral("pixels_rotated"), false);
    rawMetadata.insert(QStringLiteral("exif_orientation"), orientation(metadata, true));
    if (!hasGps(rawMetadata)) rawMetadata.remove(QStringLiteral("gps"));
    const Fields fields = fieldsFor(rawMetadata, true);
    bool ok = true;
    for (const Field &field : fields) {
        // Dimensions were set before writing RAW scanlines and cannot be changed now.
        if (field.directory == EXIF_IFD_0 && field.tag != 0x0100 && field.tag != 0x0101)
            ok = setTiff(tiff, field) && ok;
    }
    ok = TIFFWriteDirectory(tiff) && ok;
    uint64_t exifOffset = 0, gpsOffset = 0;
    TIFFCreateEXIFDirectory(tiff);
    for (const Field &field : fields) if (field.directory == EXIF_IFD_EXIF) ok = setTiff(tiff, field) && ok;
    ok = TIFFWriteCustomDirectory(tiff, &exifOffset) && ok;
    if (hasGps(metadata)) {
        TIFFCreateGPSDirectory(tiff);
        for (const Field &field : fields) if (field.directory == EXIF_IFD_GPS) ok = setTiff(tiff, field) && ok;
        ok = TIFFWriteCustomDirectory(tiff, &gpsOffset) && ok;
    }
    if (!TIFFSetDirectory(tiff, 0)) ok = false;
    else {
        ok = TIFFSetField(tiff, TIFFTAG_EXIFIFD, exifOffset) && ok;
        if (gpsOffset) ok = TIFFSetField(tiff, TIFFTAG_GPSIFD, gpsOffset) && ok;
        ok = TIFFRewriteDirectory(tiff) && ok;
    }
    if (!ok) *error = QStringLiteral("Could not write DNG metadata");
    return ok;
}
}
