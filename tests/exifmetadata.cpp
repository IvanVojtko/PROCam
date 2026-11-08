// SPDX-License-Identifier: BSD-3-Clause
#include "../src/exifutils.h"
#include "../src/heicencode.h"
#include <QtTest>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>
#include <memory>

using Exif = std::unique_ptr<ExifData, decltype(&exif_data_unref)>;
static Exif readExif(const QString &path)
{
    return Exif(exif_data_new_from_file(QFile::encodeName(path).constData()), exif_data_unref);
}
static QJsonObject details(ExifData *data)
{
    ExifEntry *entry = exif_content_get_entry(data->ifd[EXIF_IFD_EXIF], EXIF_TAG_USER_COMMENT);
    if (!entry || entry->size < 8) return {};
    return QJsonDocument::fromJson(QByteArray(reinterpret_cast<char *>(entry->data + 8), entry->size - 8)).object();
}
static bool writePhoto(const QString &path, const QJsonObject &metadata, QString *error)
{
    if (!QFile::exists(path)) {
        QImage image(4, 6, QImage::Format_RGB32);
        image.fill(Qt::green);
        if (!image.save(path, "JPEG")) return false;
    }
    QFile json(path + ".json");
    if (!json.open(QIODevice::WriteOnly)) return false;
    json.write(QJsonDocument(metadata).toJson());
    json.close();
    return ExifUtils::writeJpegExifFromJsonFile(path, json.fileName(), error);
}
static ExifEntry *tag(ExifData *data, ExifIfd directory, int id)
{
    return exif_content_get_entry(data->ifd[directory], ExifTag(id));
}
static QByteArray ascii(ExifData *data, ExifIfd directory, int id)
{
    ExifEntry *entry = tag(data, directory, id);
    return entry ? QByteArray(reinterpret_cast<const char *>(entry->data)) : QByteArray();
}
static int integer(ExifData *data, ExifIfd directory, int id)
{
    ExifEntry *entry = tag(data, directory, id);
    if (!entry) return -1;
    const ExifByteOrder order = exif_data_get_byte_order(data);
    return int(entry->format == EXIF_FORMAT_SHORT ? exif_get_short(entry->data, order)
                                                  : exif_get_long(entry->data, order));
}
static ExifRational rational(ExifData *data, ExifIfd directory, int id, int index = 0)
{
    ExifEntry *entry = tag(data, directory, id);
    if (!entry || int(entry->components) <= index) return {0, 0};
    return exif_get_rational(entry->data + 8 * index, exif_data_get_byte_order(data));
}
static double degrees(ExifData *data, int id)
{
    const auto part = [&](int i) { const ExifRational r = rational(data, EXIF_IFD_GPS, id, i);
                                   return double(r.numerator) / r.denominator; };
    return part(0) + part(1) / 60 + part(2) / 3600;
}
// Every (S)RATIONAL term fits a signed 32-bit integer, as some readers take them.
static bool rationalsInRange(ExifData *data)
{
    for (int directory = 0; directory < EXIF_IFD_COUNT; ++directory) {
        ExifContent *content = data->ifd[directory];
        for (unsigned int i = 0; i < content->count; ++i) {
            ExifEntry *entry = content->entries[i];
            if (entry->format != EXIF_FORMAT_RATIONAL && entry->format != EXIF_FORMAT_SRATIONAL) continue;
            for (unsigned int c = 0; c < entry->components; ++c) {
                const unsigned char *value = entry->data + 8 * c;
                const ExifByteOrder order = exif_data_get_byte_order(data);
                // SRATIONAL terms are signed already; only a zero denominator is wrong.
                const ExifRational r = entry->format == EXIF_FORMAT_RATIONAL ? exif_get_rational(value, order)
                        : ExifRational{0, ExifLong(qMax(0, exif_get_srational(value, order).denominator))};
                if (r.numerator > 2147483647u || r.denominator > 2147483647u || r.denominator == 0) {
                    qWarning("tag 0x%04x: %u/%u", entry->tag, r.numerator, r.denominator);
                    return false;
                }
            }
        }
    }
    return true;
}
// A portrait capture whose JPEG pixels are stored sideways (4x6, orientation 6).
static QJsonObject portraitWithLocation()
{
    return {{"captured_at", "2026-11-05T20:36:05.770+01:00"}, {"make", "Jolla"}, {"model", "Jolla Phone (2026)"},
            {"capture_orientation", 90}, {"pixels_rotated", false},
            {"exposure_time_ns", "66666000"}, {"iso", 19200}, {"focal_length_mm", 5.9499998099999996},
            {"save_location", true}, {"gps", QJsonObject{{"latitude", 60.169856}, {"longitude", -24.938379},
                                                        {"altitude", 12.5}, {"timestamp", "2026-11-05T19:36:04Z"}}}};
}
// What a HEIC of portraitWithLocation() must say: 6x4 upright pixels.
static void verifyHeicExif(ExifData *exif)
{
    QVERIFY(exif);
    QCOMPARE(ascii(exif, EXIF_IFD_EXIF, 0x9003), QByteArray("2026:11:05 20:36:05"));
    QCOMPARE(ascii(exif, EXIF_IFD_EXIF, 0x9011), QByteArray("+01:00"));
    QCOMPARE(ascii(exif, EXIF_IFD_EXIF, 0x9291), QByteArray("770"));
    QCOMPARE(integer(exif, EXIF_IFD_0, 0x0112), 1);
    QCOMPARE(integer(exif, EXIF_IFD_EXIF, 0xa002), 6);
    QCOMPARE(integer(exif, EXIF_IFD_EXIF, 0xa003), 4);
    QCOMPARE(integer(exif, EXIF_IFD_0, 0x0100), 6);
    QCOMPARE(integer(exif, EXIF_IFD_0, 0x0101), 4);
    QCOMPARE(integer(exif, EXIF_IFD_EXIF, 0xa001), 1); // sRGB
    QCOMPARE(ascii(exif, EXIF_IFD_GPS, 1), QByteArray("N"));
    QVERIFY(qAbs(degrees(exif, 2) - 60.169856) < 1e-7);
    QCOMPARE(ascii(exif, EXIF_IFD_GPS, 3), QByteArray("W"));
    QVERIFY(qAbs(degrees(exif, 4) - 24.938379) < 1e-7);
    const ExifRational altitude = rational(exif, EXIF_IFD_GPS, 6);
    QCOMPARE(double(altitude.numerator) / altitude.denominator, 12.5);
    const ExifRational focal = rational(exif, EXIF_IFD_EXIF, 0x920a);
    QCOMPARE(focal.numerator, 119u); QCOMPARE(focal.denominator, 20u);
    const ExifRational exposure = rational(exif, EXIF_IFD_EXIF, 0x829a);
    QCOMPARE(exposure.numerator, 1u); QCOMPARE(exposure.denominator, 15u);
    QVERIFY(ascii(exif, EXIF_IFD_0, 0x0131).startsWith("ProCam "));
    QVERIFY(rationalsInRange(exif));
}
static Exif exifBlock(const QByteArray &block)
{
    return Exif(exif_data_new_from_data(reinterpret_cast<const unsigned char *>(block.constData()), block.size()),
                exif_data_unref);
}
class MetadataTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_cache;
private slots:
    void initTestCase()
    {
        // HeicEncode stages its input in the cache; keep it out of the real one.
        QVERIFY(m_cache.isValid());
        qputenv("XDG_CACHE_HOME", QFile::encodeName(m_cache.path()));
    }
    void orientation_data()
    {
        QTest::addColumn<int>("rotation");
        QTest::addColumn<int>("expected");
        QTest::newRow("landscape") << 0 << 1;
        QTest::newRow("portrait") << 90 << 6;
        QTest::newRow("inverted-landscape") << 180 << 3;
        QTest::newRow("inverted-portrait") << 270 << 8;
    }
    void orientation()
    {
        QFETCH(int, rotation); QFETCH(int, expected);
        for (const QString &mode : {QString("simple"), QString("advanced"), QString("raw-render")}) {
            for (bool rotated : {false, true}) {
                QTemporaryDir dir;
                const QString path = dir.filePath("image.jpg");
                QJsonObject metadata{{"capture_mode", mode}, {"capture_orientation", rotation}, {"pixels_rotated", rotated}};
                QString error;
                QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
                auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
                ExifEntry *tag = exif_content_get_entry(exif->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
                QVERIFY(tag);
                QCOMPARE(int(exif_get_short(tag->data, exif_data_get_byte_order(exif.get()))), rotated ? 1 : expected);
                QCOMPARE(ExifUtils::orientation(metadata, true), expected);
                QCOMPARE(details(exif.get()).value("capture_mode").toString(), mode);
            }
        }
    }
    void longExposureAndTimestamp()
    {
        QTemporaryDir dir; QString error;
        QJsonObject metadata{{"exposure_time_ns", "31500000000"}, {"iso", 200},
                             {"captured_at", "2026-09-29T10:20:30.123+03:00"}};
        const QString path = dir.filePath("long.jpg");
        QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
        auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
        auto tag = exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], EXIF_TAG_EXPOSURE_TIME); QVERIFY(tag);
        const ExifRational exposure = exif_get_rational(tag->data, exif_data_get_byte_order(exif.get()));
        QVERIFY(qAbs(double(exposure.numerator) / exposure.denominator - 31.5) < 0.000001);
        tag = exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL); QVERIFY(tag);
        QCOMPARE(QByteArray(reinterpret_cast<char *>(tag->data)), QByteArray("2026:09:29 10:20:30"));
    }
    void privacyAndMissingValues()
    {
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("gps.jpg");
        QJsonObject metadata{{"save_location", true}, {"gps", QJsonObject{{"latitude", 24.7}, {"longitude", 46.6}, {"altitude", -12}}},
                             {"sensor_sensitivity_requested", 800}, {"exposure_time_requested_ns", "30000000000"}};
        QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
        auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
        QVERIFY(exif->ifd[EXIF_IFD_GPS]->count > 0);
        QVERIFY(!exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], EXIF_TAG_EXPOSURE_TIME));
        metadata["save_location"] = false;
        QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
        exif = readExif(path); QVERIFY(exif.get() != nullptr);
        QCOMPARE(exif->ifd[EXIF_IFD_GPS]->count, 0u);
        QVERIFY(!details(exif.get()).contains("gps"));
        const QJsonObject merged = ExifUtils::mergeCaptureMetadata(metadata, QJsonObject{{"save_location", false}});
        QVERIFY(!merged.contains("gps"));
    }
    void preserveMetadataAfterPixelRewrite()
    {
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("preserve.jpg");
        QVERIFY(writePhoto(path, {{"lens_model", "Test lens"}, {"iso", 100}}, &error));
        const QByteArray original = ExifUtils::originalJpegExif(path);
        QImage pixels(path); QVERIFY(!pixels.isNull());
        QVERIFY(pixels.save(path, "JPEG"));
        QVERIFY(writePhoto(path, {{"_original_exif", QString::fromLatin1(original.toBase64())}, {"pixels_rotated", true}}, &error));
        auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
        QVERIFY(exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], ExifTag(0xa434)));
        QVERIFY(exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], EXIF_TAG_ISO_SPEED_RATINGS));
    }
    void commentOverflowAndBrackets()
    {
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("bracket.jpg");
        QJsonArray calibration;
        for (int i = 0; i < 30000; ++i) calibration.append(i);
        QJsonObject metadata{{"capture_mode", "advanced"}, {"bracket_combined", true},
                             {"bracket_frames", QJsonArray{QJsonObject{{"iso", 100}}, QJsonObject{{"iso", 200}}}},
                             {"calibration", calibration}, {"label", QString::fromUtf8("صورة")}};
        QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
        auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
        const auto embedded = details(exif.get());
        QCOMPARE(embedded.value("label"), metadata.value("label"));
        QVERIFY(embedded.value("bracket_combined").toBool());
        QVERIFY(embedded.value("omitted_fields").toArray().contains("calibration"));
        QFile sidecar(path + ".json"); QVERIFY(sidecar.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(sidecar.readAll()).object().contains("calibration"));
    }
    void heicExifKeepsCaptureDetails()
    {
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("portrait.jpg");
        QVERIFY2(writePhoto(path, portraitWithLocation(), &error), qPrintable(error));
        auto jpeg = readExif(path); QVERIFY(jpeg.get() != nullptr);
        QCOMPARE(integer(jpeg.get(), EXIF_IFD_0, 0x0112), 6);
        QVERIFY(rationalsInRange(jpeg.get()));
        const QByteArray block = ExifUtils::heicExif(path, QSize(6, 4));
        QVERIFY(block.startsWith(QByteArray("Exif\0\0", 6)));
        auto exif = exifBlock(block);
        verifyHeicExif(exif.get());
    }
    void heicExifNeedsCaptureDate()
    {
        QTemporaryDir dir; QString error;
        const QString bare = dir.filePath("bare.jpg");
        QImage image(4, 6, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(bare, "JPEG"));
        QVERIFY(ExifUtils::heicExif(bare, QSize(4, 6)).isEmpty());
        const QString undated = dir.filePath("undated.jpg");
        QVERIFY2(writePhoto(undated, {{"iso", 100}}, &error), qPrintable(error));
        QVERIFY(readExif(undated).get() != nullptr);
        QVERIFY(ExifUtils::heicExif(undated, QSize(4, 6)).isEmpty());
    }
    void rationals()
    {
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("rationals.jpg");
        QVERIFY2(writePhoto(path, {{"exposure_time_ns", "4000000"}, {"exposure_compensation_ev", -1.0 / 3},
                                   {"lens_aperture", 1.79}, {"zoom_ratio", 2.0}}, &error), qPrintable(error));
        auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
        const ExifRational exposure = rational(exif.get(), EXIF_IFD_EXIF, 0x829a);
        QCOMPARE(exposure.numerator, 1u); QCOMPARE(exposure.denominator, 250u);
        ExifEntry *bias = tag(exif.get(), EXIF_IFD_EXIF, 0x9204); QVERIFY(bias);
        const ExifSRational ev = exif_get_srational(bias->data, exif_data_get_byte_order(exif.get()));
        QCOMPARE(ev.numerator, -1); QCOMPARE(ev.denominator, 3);
        const ExifRational aperture = rational(exif.get(), EXIF_IFD_EXIF, 0x829d);
        QCOMPARE(aperture.numerator, 179u); QCOMPARE(aperture.denominator, 100u);
        const ExifRational zoom = rational(exif.get(), EXIF_IFD_EXIF, 0xa404);
        QCOMPARE(zoom.numerator, 2u); QCOMPARE(zoom.denominator, 1u);
        QVERIFY(rationalsInRange(exif.get()));
    }
    void heicConversion_data()
    {
        // Stand-ins for the Camera2 helper: argument 7 is the output, 8 the EXIF.
        QTest::addColumn<QByteArray>("helper");
        QTest::addColumn<bool>("converts");
        QTest::newRow("exif stored") << QByteArray("printf ftypheic > \"$7\" && cat \"$8\" >> \"$7\"") << true;
        QTest::newRow("exif left out") << QByteArray("printf ftypheic > \"$7\"") << false;
        QTest::newRow("encoder failed") << QByteArray("echo '{\"ok\":false}'; exit 1") << false;
    }
    void heicConversion()
    {
        QFETCH(QByteArray, helper); QFETCH(bool, converts);
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("ProCam_1.jpg");
        QVERIFY2(writePhoto(path, portraitWithLocation(), &error), qPrintable(error));
        QFile script(dir.filePath("helper.sh"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\n" + helper + "\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QString heicPath;
        QCOMPARE(HeicEncode::convertJpeg(path, script.fileName(), 90, &heicPath, &error), converts);
        const QString heic = dir.filePath("ProCam_1.heic");
        // The JPEG goes only when the HEIC is complete.
        QCOMPARE(QFile::exists(path), !converts);
        QCOMPARE(QFile::exists(heic), converts);
        QVERIFY(!QFile::exists(dir.filePath(".ProCam_1.heic.part")));
        QCOMPARE(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).entryList(QDir::Files),
                 QStringList());
        if (!converts) {
            QVERIFY(!error.isEmpty());
            return;
        }
        QCOMPARE(heicPath, heic);
        QFile output(heic); QVERIFY(output.open(QIODevice::ReadOnly));
        const QByteArray bytes = output.readAll();
        auto exif = exifBlock(bytes.mid(bytes.indexOf(QByteArray("Exif\0\0", 6))));
        verifyHeicExif(exif.get());
    }
    void heicConversionNeedsCaptureDate()
    {
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("undated.jpg");
        QVERIFY2(writePhoto(path, {{"iso", 100}}, &error), qPrintable(error));
        QString heicPath;
        QVERIFY(!HeicEncode::convertJpeg(path, QStringLiteral("/bin/false"), 90, &heicPath, &error));
        QVERIFY(error.contains("capture date"));
        QVERIFY(QFile::exists(path));
    }
    void dngMetadata()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("raw.dng");
        TIFF *tiff = TIFFOpen(QFile::encodeName(path).constData(), "w"); QVERIFY(tiff);
        TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 1);
        TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 1);
        TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16);
        TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, 1);
        TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        uint16_t pixel = 1234;
        QVERIFY(TIFFWriteScanline(tiff, &pixel, 0) >= 0);
        QJsonObject metadata{{"capture_orientation", 90}, {"pixels_rotated", true},
                             {"iso", 400}, {"exposure_time_ns", "30000000000"}, {"width", 1}, {"height", 1},
                             {"save_location", true}, {"gps", QJsonObject{{"latitude", -24.7}, {"longitude", 46.6}}}};
        QString error;
        const bool ok = ExifUtils::writeDngMetadata(tiff, metadata, &error);
        TIFFClose(tiff); QVERIFY2(ok, qPrintable(error));
        tiff = TIFFOpen(QFile::encodeName(path).constData(), "r"); QVERIFY(tiff);
        uint16_t orientation = 0;
        QVERIFY(TIFFGetField(tiff, TIFFTAG_ORIENTATION, &orientation)); QCOMPARE(orientation, uint16_t(6));
        uint16_t readPixel = 0;
        QVERIFY(TIFFReadScanline(tiff, &readPixel, 0) >= 0); QCOMPARE(readPixel, pixel);
        uint64_t exifOffset = 0, gpsOffset = 0;
        QVERIFY(TIFFGetField(tiff, TIFFTAG_EXIFIFD, &exifOffset));
        QVERIFY(TIFFGetField(tiff, TIFFTAG_GPSIFD, &gpsOffset));
        QVERIFY(TIFFReadEXIFDirectory(tiff, exifOffset));
        float exposure = 0;
        QVERIFY(TIFFGetField(tiff, EXIFTAG_EXPOSURETIME, &exposure)); QCOMPARE(exposure, 30.0f);
        QVERIFY(TIFFReadGPSDirectory(tiff, gpsOffset));
        char *reference = nullptr;
        QVERIFY(TIFFGetField(tiff, GPSTAG_LATITUDEREF, &reference)); QCOMPARE(QByteArray(reference), QByteArray("S"));
        TIFFClose(tiff);
    }
};
QTEST_GUILESS_MAIN(MetadataTest)
#include "exifmetadata.moc"
