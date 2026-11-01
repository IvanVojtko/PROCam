// SPDX-FileCopyrightText: 2013 - 2014 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include "declarativecameraextensions.h"
#include "previewsize.h"
#include "exifutils.h"
#include "dnglensshading.h"
#include "imageadjustments.h"
#include "rawbracket.h"
#include "rawpipeline.h"
#include "rawpipelinemath.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLockFile>
#include <QProcess>
#include <QQmlInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QTransform>
#include <QUrl>
#include <QVector>

#include <QtDebug>
#include <QtConcurrent/QtConcurrentRun>

#include <QQuickWindow>
#include <qpa/qplatformnativeinterface.h>

#include <tiffio.h>

// C++ counterpart of the QML DeviceInfo type used in
// src/capture/CaptureView.qml. Provided by the "systemsettings" pkg-config
// module already linked in src.pro.
#include <deviceinfo.h>

#ifndef TIFFTAG_NOISEPROFILE
#define TIFFTAG_NOISEPROFILE 51041
#endif

#ifndef TIFFTAG_OPCODELIST2
#define TIFFTAG_OPCODELIST2 51009
#endif

#include <algorithm>
#include <cmath>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <cstdlib>
#include <initializer_list>
#include <stdint.h>
#include <utime.h>

namespace {
// TIFFTAG_NOISEPROFILE and TIFFTAG_OPCODELIST2 are DNG-private tags that this
// build of libtiff does not register internally (unlike e.g. COLORMATRIX1 or
// ASSHOTNEUTRAL, which libtiff already knows about). Without registering
// them first, TIFFSetField() fails with "Unknown tag" and the tag is never
// written.
const TIFFFieldInfo dngPrivateFields[] = {
    { TIFFTAG_NOISEPROFILE, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, 1, 1, const_cast<char *>("DNGNoiseProfile") },
    { TIFFTAG_OPCODELIST2, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED, FIELD_CUSTOM, 1, 1, const_cast<char *>("DNGOpcodeList2") },
};
}

DeclarativeCameraExtensions::DeclarativeCameraExtensions(QObject *parent)
    : QObject(parent)
{
}

DeclarativeCameraExtensions::~DeclarativeCameraExtensions()
{
    // Bursts still queued are dropped with their frames; the running one
    // finishes so it does not write into freed state.
    if (m_burstWatcher) {
        m_burstWatcher->disconnect(this);
        if (m_burstWatcher->isRunning()) {
            m_burstWatcher->waitForFinished();
        }
    }
    if (m_bracketCombineWatcher) {
        m_bracketCombineWatcher->disconnect(this);
        if (m_bracketCombineWatcher->isRunning()) {
            m_bracketCombineWatcher->waitForFinished();
        }
    }
    if (m_rawRenderWatcher) {
        m_rawRenderWatcher->disconnect(this);
        if (m_rawRenderWatcher->isRunning()) {
            m_rawRenderWatcher->waitForFinished();
        }
    }
    if (m_rawCaptureProcess) {
        m_rawCaptureProcess->disconnect(this);
        if (m_rawCaptureProcess->state() != QProcess::NotRunning) {
            m_rawCaptureProcess->terminate();
            if (!m_rawCaptureProcess->waitForFinished(1000)) {
                m_rawCaptureProcess->kill();
                m_rawCaptureProcess->waitForFinished(1000);
            }
        }
    }
}

static QString rawCaptureProbePath()
{
    const QString override = QString::fromLocal8Bit(qgetenv("SFOS_CAMERA2_PROBE"));
    return override.isEmpty()
            ? QStringLiteral("/usr/libexec/procam/sfos-camera2-probe")
            : override;
}

static QString rawFastJpegConverterPath()
{
    const QString override = QString::fromLocal8Bit(qgetenv("SFOS_RAW16_JPEG_CONVERTER"));
    return override.isEmpty()
            ? QStringLiteral("/usr/libexec/procam/sfos-raw16-to-jpeg")
            : override;
}

static bool rawRenderEngineSupported(const QString &engine)
{
    return engine == QLatin1String("internal")
            || engine == QLatin1String("fastjpeg")
            || engine == QLatin1String("enhanced");
}

static bool rawCaptureFormatSupported(const QString &format)
{
    return format == QLatin1String("raw16")
            || format == QLatin1String("raw10");
}

static QVariantList integerVariantList(std::initializer_list<int> values)
{
    QVariantList list;
    for (int value : values) {
        list.append(value);
    }
    return list;
}

static bool executableExists(const QString &path)
{
    const QFileInfo file(path);
    return file.exists() && file.isExecutable();
}

static QStringList camera2OutputSizes(const QJsonObject &camera,
                                      const QString &key)
{
    QStringList sizes;
    const QJsonArray outputs = camera.value(key).toArray();
    for (const QJsonValue &value : outputs) {
        const QJsonObject output = value.toObject();
        const int width = output.value(QStringLiteral("width")).toInt();
        const int height = output.value(QStringLiteral("height")).toInt();
        if (width <= 0 || height <= 0) {
            continue;
        }

        const QString size = QStringLiteral("%1x%2").arg(width).arg(height);
        if (!sizes.contains(size)) {
            sizes.append(size);
        }
    }
    return sizes;
}

static QString procamUserDeviceProfileDir()
{
    const QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return dataPath.isEmpty()
            ? QString()
            : QDir(dataPath).filePath(QStringLiteral("device-profiles"));
}

// Lens shading lookup for RAW rendering, as in writeTiffDng(). Call on the
// GUI thread (DeviceInfo).
static RawPipeline::RenderSettings rawPipelineSettings()
{
    static const DeviceInfo deviceInfo;
    RawPipeline::RenderSettings settings;
    const QString profileDir = procamUserDeviceProfileDir();
    if (!profileDir.isEmpty())
        settings.calibrationDirs.append(QDir(profileDir).filePath(QStringLiteral("lens-shading")));
    settings.calibrationDirs.append(QStringLiteral(DEPLOYMENT_PATH "calibration"));
    settings.deviceModel = deviceInfo.prettyName();
    return settings;
}

static QString procamGeneratedHalProfileFileName()
{
    return QStringLiteral("generated-hal.json");
}

static QString procamOverrideHalProfileFileName()
{
    return QStringLiteral("override-hal.json");
}

static QString procamGeneratedHalProfilePath()
{
    const QString dir = procamUserDeviceProfileDir();
    return dir.isEmpty()
            ? QString()
            : QDir(dir).filePath(procamGeneratedHalProfileFileName());
}

static QString procamOverrideHalProfilePath()
{
    const QString dir = procamUserDeviceProfileDir();
    return dir.isEmpty()
            ? QString()
            : QDir(dir).filePath(procamOverrideHalProfileFileName());
}

static QStringList procamOverrideHalProfilePaths()
{
    QStringList paths;
    const QString dir = procamUserDeviceProfileDir();
    if (!dir.isEmpty()) {
        const QString path = QDir(dir).filePath(procamOverrideHalProfileFileName());
        paths.append(path);
    }
    return paths;
}

static QJsonObject cameraExposureObject(const QJsonObject &camera)
{
    return camera.value(QStringLiteral("exposure")).toObject();
}

static QJsonObject cameraFocusObject(const QJsonObject &camera)
{
    return camera.value(QStringLiteral("focus")).toObject();
}

static QJsonObject cameraZoomObject(const QJsonObject &camera)
{
    return camera.value(QStringLiteral("zoom")).toObject();
}

static QJsonObject cameraLensObject(const QJsonObject &camera)
{
    return camera.value(QStringLiteral("lens")).toObject();
}

static QJsonObject procamOverrideObject(const QJsonObject &profile)
{
    return profile.value(QStringLiteral("overrides")).toObject()
            .value(QStringLiteral("procam")).toObject();
}

static QJsonArray cameraIsoRange(const QJsonObject &camera)
{
    return cameraExposureObject(camera).value(QStringLiteral("iso_range")).toArray();
}

static qint64 camera2IsoMax(const QJsonObject &camera)
{
    const QJsonArray isoRange = cameraIsoRange(camera);
    return isoRange.count() >= 2 ? qint64(isoRange.at(1).toDouble()) : 0;
}

static qint64 camera2IsoMin(const QJsonObject &camera)
{
    const QJsonArray isoRange = cameraIsoRange(camera);
    return isoRange.count() >= 2 ? qint64(isoRange.at(0).toDouble()) : 0;
}

static int clampCamera2Iso(const QJsonObject &camera, int iso)
{
    if (iso <= 0) {
        return 0;
    }
    const qint64 minIso = camera2IsoMin(camera);
    const qint64 maxIso = camera2IsoMax(camera);
    int value = iso;
    if (minIso > 0 && value < minIso) {
        value = int(minIso);
    }
    if (maxIso > 0 && value > maxIso) {
        value = int(maxIso);
    }
    return value;
}

static qreal camera2ZoomMaximum(const QJsonObject &camera)
{
    const QJsonObject zoom = cameraZoomObject(camera);
    const QJsonArray range = zoom.value(QStringLiteral("zoom_ratio_range")).toArray();
    if (range.count() >= 2 && range.at(1).toDouble() > 1.0) {
        return range.at(1).toDouble();
    }
    const qreal maxDigitalZoom = zoom.value(QStringLiteral("max_digital_zoom")).toDouble();
    return maxDigitalZoom > 1.0 ? maxDigitalZoom : 1.0;
}

static qreal clampCamera2Zoom(const QJsonObject &camera, qreal zoom)
{
    qreal minZoom = 1.0;
    const QJsonArray range = cameraZoomObject(camera)
            .value(QStringLiteral("zoom_ratio_range")).toArray();
    if (range.count() >= 2 && range.at(0).toDouble() > 0.0) {
        minZoom = range.at(0).toDouble();
    }
    return qBound(minZoom, zoom, camera2ZoomMaximum(camera));
}

static QStringList stringListFromJsonArray(const QJsonArray &array,
                                           const QStringList &fallback)
{
    QStringList values;
    for (const QJsonValue &value : array) {
        const QString text = value.isString()
                ? value.toString()
                : QString::number(value.toDouble());
        if (!text.isEmpty() && !values.contains(text)) {
            values.append(text);
        }
    }
    return values.isEmpty() ? fallback : values;
}

static QVariantList intListFromJsonArray(const QJsonArray &array,
                                         const QVariantList &fallback)
{
    QVariantList values;
    for (const QJsonValue &value : array) {
        const int number = value.toInt(-1);
        if (number >= 0 && !values.contains(number)) {
            values.append(number);
        }
    }
    return values.isEmpty() ? fallback : values;
}

static QList<int> camera2IsoSteps()
{
    return QList<int>()
            << 100 << 200 << 400 << 800 << 1600
            << 3200 << 6400 << 12800 << 19200;
}

static QStringList camera2ShutterSteps()
{
    return QStringList()
            << QStringLiteral("0")
            << QStringLiteral("100000")
            << QStringLiteral("250000")
            << QStringLiteral("500000")
            << QStringLiteral("1000000")
            << QStringLiteral("2000000")
            << QStringLiteral("4000000")
            << QStringLiteral("8333333")
            << QStringLiteral("16666667")
            << QStringLiteral("33333333")
            << QStringLiteral("66666667")
            << QStringLiteral("125000000")
            << QStringLiteral("250000000")
            << QStringLiteral("500000000")
            << QStringLiteral("1000000000")
            << QStringLiteral("2000000000")
            << QStringLiteral("4000000000")
            << QStringLiteral("8000000000")
            << QStringLiteral("16000000000");
}

static QStringList camera2FocusDistanceSteps()
{
    return QStringList()
            << QStringLiteral("0.25")
            << QStringLiteral("0.5")
            << QStringLiteral("0.75")
            << QStringLiteral("1")
            << QStringLiteral("1.5")
            << QStringLiteral("2")
            << QStringLiteral("3")
            << QStringLiteral("4")
            << QStringLiteral("5")
            << QStringLiteral("7.5")
            << QStringLiteral("10")
            << QStringLiteral("15")
            << QStringLiteral("20");
}

static QStringList rawCaptureExposureDefaults()
{
    return QStringList()
            << QStringLiteral("0.5")
            << QStringLiteral("1.0")
            << QStringLiteral("1.5")
            << QStringLiteral("2.0")
            << QStringLiteral("4.0");
}

static QVariantList rawCaptureRotationDefaults()
{
    return integerVariantList({0, 90, 180, 270});
}

static QVariantList rawCaptureAfWaitDefaults()
{
    return integerVariantList({1, 3, 5, 10});
}

static QJsonArray camera2ProbeCameras(const QByteArray &json)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return QJsonArray();
    }
    return document.object().value(QStringLiteral("cameras")).toArray();
}

static QJsonObject camera2ProbeCamera(const QByteArray &json,
                                      const QString &cameraId)
{
    const QJsonArray cameras = camera2ProbeCameras(json);
    for (const QJsonValue &value : cameras) {
        const QJsonObject camera = value.toObject();
        if (camera.value(QStringLiteral("id")).toString() == cameraId) {
            return camera;
        }
    }
    return QJsonObject();
}

static QString camera2FocalLengthLabel(const QJsonObject &camera)
{
    const qreal focalLength = cameraLensObject(camera)
            .value(QStringLiteral("focal_length_mm")).toDouble();
    if (focalLength <= 0.0) {
        return QStringLiteral("-- MM");
    }
    const QString value = focalLength < 10.0
            ? QString::number(focalLength, 'f', 1)
            : QString::number(qRound(focalLength));
    return QStringLiteral("%1 MM").arg(value);
}

static bool camera2ProfileNumberMatches(const QJsonObject &match,
                                        const QString &key,
                                        qint64 value)
{
    return !match.contains(key) || qint64(match.value(key).toDouble()) == value;
}

static QJsonObject mergeJsonObjects(const QJsonObject &base,
                                    const QJsonObject &overrides)
{
    QJsonObject merged = base;
    for (QJsonObject::const_iterator it = overrides.constBegin();
         it != overrides.constEnd(); ++it) {
        if (it.value().isObject() && merged.value(it.key()).isObject()) {
            merged.insert(it.key(),
                          mergeJsonObjects(merged.value(it.key()).toObject(),
                                           it.value().toObject()));
        } else {
            merged.insert(it.key(), it.value());
        }
    }
    return merged;
}

static bool deviceProfileMatches(const QJsonObject &profile,
                                 const QJsonObject &camera,
                                 const QStringList &raw16Sizes,
                                 qint64 halMaxShutterNs)
{
    const QJsonObject match = profile.value(QStringLiteral("match")).toObject();
    if (match.contains(QStringLiteral("camera_id")) &&
            match.value(QStringLiteral("camera_id")).toString() !=
            camera.value(QStringLiteral("id")).toString()) {
        return false;
    }
    if (match.contains(QStringLiteral("raw_capability")) &&
            match.value(QStringLiteral("raw_capability")).toBool() !=
            camera.value(QStringLiteral("raw_capability")).toBool(false)) {
        return false;
    }
    const QString rawSize = match.value(QStringLiteral("raw_size")).toString();
    if (!rawSize.isEmpty() && !raw16Sizes.contains(rawSize)) {
        return false;
    }
    if (!camera2ProfileNumberMatches(match, QStringLiteral("iso_max"),
                                     camera2IsoMax(camera))) {
        return false;
    }
    return camera2ProfileNumberMatches(
                match, QStringLiteral("hal_shutter_max_ns"), halMaxShutterNs);
}

static bool overrideProfileMatches(const QJsonObject &profile,
                                   const QJsonObject &camera,
                                   const QStringList &raw16Sizes,
                                   qint64 halMaxShutterNs,
                                   int profileCount)
{
    const QJsonObject match = profile.value(QStringLiteral("match")).toObject();
    if (match.isEmpty() && profileCount == 1) {
        return true;
    }
    if (match.contains(QStringLiteral("camera_id")) &&
            match.value(QStringLiteral("camera_id")).toString() !=
            camera.value(QStringLiteral("id")).toString()) {
        return false;
    }
    if (match.contains(QStringLiteral("camera_id"))) {
        return true;
    }
    return deviceProfileMatches(profile, camera, raw16Sizes, halMaxShutterNs);
}

static qint64 procamOverrideHalProfileMtime()
{
    qint64 mtime = 0;
    for (const QString &path : procamOverrideHalProfilePaths()) {
        const QFileInfo file(path);
        if (file.exists()) {
            mtime = qMax(mtime, file.lastModified().toMSecsSinceEpoch());
        }
    }
    return mtime;
}

static bool writeGeneratedHalProfile(const QJsonObject &probe,
                                     QString *writtenPath,
                                     QString *error)
{
    const QString path = procamGeneratedHalProfilePath();
    if (path.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Qt config location is empty");
        }
        return false;
    }

    const QJsonArray cameras = probe.value(QStringLiteral("cameras")).toArray();
    QJsonArray profiles;
    for (const QJsonValue &value : cameras) {
        const QJsonObject camera = value.toObject();
        const QString cameraId = camera.value(QStringLiteral("id")).toString();
        if (cameraId.isEmpty()) {
            continue;
        }

        QJsonObject match;
        match.insert(QStringLiteral("camera_id"), cameraId);

        QJsonObject profile;
        profile.insert(QStringLiteral("id"),
                       QStringLiteral("hal-camera-%1").arg(cameraId));
        profile.insert(QStringLiteral("name"),
                       QStringLiteral("Camera %1").arg(cameraId));
        profile.insert(QStringLiteral("match"), match);
        profile.insert(QStringLiteral("hal"), camera);
        profile.insert(QStringLiteral("overrides"), QJsonObject());
        profiles.append(profile);
    }

    QJsonObject config;
    config.insert(QStringLiteral("schema_version"), 1);
    config.insert(QStringLiteral("kind"),
                  QStringLiteral("procam-device-config"));
    config.insert(QStringLiteral("source"), QStringLiteral("hal-probe"));
    config.insert(QStringLiteral("generated_at"),
                  QDateTime::currentDateTime().toString(Qt::ISODate));
    config.insert(QStringLiteral("bridge_version"),
                  probe.value(QStringLiteral("bridge_version")).toString());
    config.insert(QStringLiteral("probe"), probe);
    config.insert(QStringLiteral("profiles"), profiles);

    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("Could not create generated HAL profile directory: %1")
                    .arg(QFileInfo(path).absolutePath());
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("Could not write generated HAL profile: %1")
                    .arg(path);
        }
        return false;
    }
    const QByteArray data = QJsonDocument(config).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        if (error) {
            *error = QStringLiteral("Could not commit generated HAL profile: %1")
                    .arg(path);
        }
        return false;
    }
    if (writtenPath) {
        *writtenPath = path;
    }
    return true;
}

static void appendDeviceProfileObject(QVector<QPair<QJsonObject, QString> > *profiles,
                                      const QJsonObject &object,
                                      const QString &source)
{
    if (object.contains(QStringLiteral("profiles"))) {
        const QJsonArray array = object.value(QStringLiteral("profiles")).toArray();
        for (const QJsonValue &value : array) {
            if (value.isObject()) {
                profiles->append(qMakePair(value.toObject(), source));
            }
        }
    } else if (object.contains(QStringLiteral("match")) ||
               object.contains(QStringLiteral("overrides"))) {
        profiles->append(qMakePair(object, source));
    }
}

static QVector<QPair<QJsonObject, QString> > procamDeviceProfiles()
{
    QVector<QPair<QJsonObject, QString> > profiles;
    qDebug() << "override-hal"
             << "config_dir=" + procamUserDeviceProfileDir();

    for (const QString &path : procamOverrideHalProfilePaths()) {
        const QFileInfo info(path);
        qDebug() << "override-hal"
                 << "path=" + path
                 << "exists=" + QString::number(info.exists() ? 1 : 0)
                 << "size=" + QString::number(info.exists() ? info.size() : 0);
        if (!info.exists()) {
            continue;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qDebug() << "override-hal"
                     << "path=" + path
                     << "open=0"
                     << "error=" + file.errorString();
            continue;
        }

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(
                    file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            qDebug() << "override-hal"
                     << "path=" + path
                     << "parse=0"
                     << "error=" + error.errorString();
            continue;
        }

        appendDeviceProfileObject(&profiles, document.object(),
                                  QStringLiteral("user-override"));
        qDebug() << "override-hal"
                 << "path=" + path
                 << "parse=1"
                 << "profiles=" + QString::number(profiles.count());
        if (!profiles.isEmpty()) {
            return profiles;
        }
    }

    qDebug() << "override-hal"
             << "valid=0";
    return profiles;
}

static QStringList camera2NamedValues(const QJsonObject &camera,
                                      const QString &groupKey,
                                      const QString &arrayKey)
{
    QStringList values;
    const QJsonArray array = camera.value(groupKey).toObject()
            .value(arrayKey).toArray();
    for (const QJsonValue &value : array) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty() && !values.contains(name)) {
            values.append(name);
        }
    }
    return values;
}

static QVariantList camera2NoiseReductionValues(const QJsonObject &camera)
{
    QVariantList values;
    const QJsonArray array = camera.value(QStringLiteral("noise_reduction"))
            .toObject().value(QStringLiteral("modes")).toArray();
    for (const QJsonValue &value : array) {
        const int mode = value.toObject().value(QStringLiteral("value")).toInt(-1);
        if (mode >= 0 && !values.contains(mode)) {
            values.append(mode);
        }
    }
    if (values.isEmpty()) {
        values.append(0);
    }
    return values;
}

static QString procamPicturesPath()
{
    const QString picturesPath = QStandardPaths::writableLocation(
                QStandardPaths::PicturesLocation);
    return (picturesPath.isEmpty() ? QDir::homePath() + QLatin1String("/Pictures")
                                   : picturesPath) + QLatin1String("/ProCam");
}

static bool readableImageExists(const QString &path, QString *error)
{
    const QFileInfo file(path);
    if (!file.isFile() || file.size() <= 0) {
        *error = QStringLiteral("RAW JPEG output is missing or empty: %1").arg(path);
        return false;
    }

    QImageReader reader(path);
    if (!reader.canRead() || !reader.size().isValid()) {
        *error = QStringLiteral("RAW JPEG output is not readable: %1 size=%2 error=%3")
                .arg(path)
                .arg(file.size())
                .arg(reader.errorString());
        return false;
    }
    return true;
}

static QString jsonSidecarPath(const QString &targetPath)
{
    const QFileInfo targetInfo(targetPath);
    return targetInfo.absolutePath() + QLatin1Char('/')
            + targetInfo.completeBaseName() + QLatin1String(".json");
}

static bool writeMetadataWithRawPath(const QString &sourcePath,
                                     const QString &destinationPath,
                                     const QString &rawPath,
                                     QString *error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read metadata: %1").arg(sourcePath);
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(source.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("RAW metadata is not valid JSON");
        return false;
    }

    QJsonObject object = document.object();
    object.insert(QStringLiteral("raw_path"), rawPath);
    document.setObject(object);

    QDir().mkpath(QFileInfo(destinationPath).absolutePath());
    QFile::remove(destinationPath);
    QFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("Cannot write metadata: %1").arg(destinationPath);
        return false;
    }
    destination.write(document.toJson(QJsonDocument::Indented));
    return true;
}

static void appendRawCaptureLog(const QString &message)
{
    QString picturesPath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (picturesPath.isEmpty()) {
        picturesPath = QDir::homePath() + QLatin1String("/Pictures");
    }
    const QString logDirectory = picturesPath + QLatin1String("/ProCam");
    QDir().mkpath(logDirectory);
    QFile file(logDirectory + QLatin1String("/procam-capture.log"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    file.write(QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8());
    file.write(" ");
    file.write(message.toUtf8());
    file.write("\n");
}

static bool isBenignCamera2HelperWarning(const QString &line)
{
    return line.contains(QStringLiteral("not accessible for the namespace")) ||
            line.contains(QStringLiteral("libandroidicu.so")) ||
            line.contains(QStringLiteral("libisu.so"));
}

static QString filterCamera2HelperErrors(const QString &output)
{
    const QStringList lines = output.split(QLatin1Char('\n'),
                                          QString::SkipEmptyParts);
    QStringList errors;
    for (const QString &line : lines) {
        if (!isBenignCamera2HelperWarning(line)) {
            errors.append(line);
        }
    }
    return errors.join(QLatin1Char('\n')).trimmed();
}

static QString camera2HelperStatusMessage(const QByteArray &json,
                                          const QString &fallback)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fallback;
    }

    const QJsonObject object = document.object();
    const QString stage = object.value(QStringLiteral("stage")).toString();
    const int code = object.value(QStringLiteral("code")).toInt();
    const QString rawPath = object.value(QStringLiteral("raw_path")).toString();
    const QString metadataPath = object.value(QStringLiteral("metadata_path")).toString();

    if (stage.isEmpty()) {
        return fallback;
    }
    if (stage == QLatin1String("raw_size") &&
            rawPath.endsWith(QLatin1String(".raw10"))) {
        return QStringLiteral(
                    "RAW10 is not exposed by the camera HAL/NDK for this camera");
    }

    QString message = QStringLiteral("Camera2 helper failed: stage=%1 code=%2")
            .arg(stage)
            .arg(code);
    if (!rawPath.isEmpty()) {
        message += QStringLiteral(" raw=%1").arg(rawPath);
    }
    if (!metadataPath.isEmpty()) {
        message += QStringLiteral(" metadata=%1").arg(metadataPath);
    }
    return message;
}

static QString rawPathFromMetadata(const QString &metadataPath, QString *error)
{
    QFile file(metadataPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read metadata: %1").arg(metadataPath);
        return QString();
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("RAW metadata is not valid JSON: %1").arg(metadataPath);
        return QString();
    }

    const QString rawPath = document.object().value(QStringLiteral("raw_path")).toString();
    if (rawPath.isEmpty()) {
        *error = QStringLiteral("RAW metadata has no raw_path: %1").arg(metadataPath);
    }
    return rawPath;
}

bool DeclarativeCameraExtensions::rawImageCaptureAvailable() const
{
    return executableExists(rawCaptureProbePath());
}

QString DeclarativeCameraExtensions::camera2DeviceConfigDir() const
{
    return procamUserDeviceProfileDir();
}

QString DeclarativeCameraExtensions::ensureCamera2GeneratedHalConfig(
        const QString &cameraId)
{
    Q_UNUSED(cameraId);
    const auto fail = [this](const QString &reason) {
        const QString message = QStringLiteral("Could not generate HAL config: %1").arg(reason);
        qWarning() << message;
        appendRawCaptureLog(message);
        return message;
    };
    const QString path = procamGeneratedHalProfilePath();
    if (path.isEmpty()) {
        return fail(QStringLiteral("Qt application data location is empty"));
    }
    // Existing snapshots are user-owned, even when empty or invalid. Never replace them.
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        return path;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return fail(QStringLiteral("Could not create directory for %1").arg(path));
    }
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) {
        return fail(QStringLiteral("HAL config creation is locked: %1").arg(path));
    }
    // Another launch may have finished between the initial check and acquiring the lock.
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        return path;
    }

    const auto validProbe = [](const QJsonDocument &document) {
        if (!document.isObject()) return false;
        const QJsonArray cameras = document.object().value(QStringLiteral("cameras")).toArray();
        for (const QJsonValue &value : cameras) {
            const QJsonObject camera = value.toObject();
            if (!camera.value(QStringLiteral("id")).toString().isEmpty() &&
                    camera.value(QStringLiteral("status")).toString() == QLatin1String("ok")) {
                return true;
            }
        }
        return false;
    };
    QJsonParseError parseError;
    QJsonDocument document;
    if (m_camera2CapabilitiesValid) {
        document = QJsonDocument::fromJson(m_camera2ProbeJson, &parseError);
    }
    if (!validProbe(document)) {
        if (!rawImageCaptureAvailable()) {
            return fail(QStringLiteral("Camera2 helper is not installed"));
        }
        QProcess probe;
        probe.start(rawCaptureProbePath());
        if (!probe.waitForStarted(3000)) {
            return fail(QStringLiteral("Camera2 helper did not start"));
        }
        if (!probe.waitForFinished(5000) || probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
            const QString helperError = filterCamera2HelperErrors(
                        QString::fromLocal8Bit(probe.readAllStandardError()));
            return fail(QStringLiteral("Camera2 probe failed: %1").arg(helperError));
        }
        document = QJsonDocument::fromJson(probe.readAllStandardOutput(), &parseError);
    }
    if (!validProbe(document)) {
        return fail(QStringLiteral("Invalid or empty Camera2 probe JSON"));
    }
    QString error;
    if (!writeGeneratedHalProfile(document.object(), nullptr, &error)) {
        return fail(error);
    }
    const QString message = QStringLiteral("Generated HAL config: %1").arg(path);
    qDebug() << message;
    appendRawCaptureLog(message);
    return path;
}

bool DeclarativeCameraExtensions::loadCamera2Capabilities(const QString &cameraId)
{
    const QString effectiveCameraId = cameraId.isEmpty()
            ? QStringLiteral("0") : cameraId;
    const qint64 overrideProfileMtime = procamOverrideHalProfileMtime();
    if (m_camera2CapabilitiesLoaded &&
            m_camera2CapabilitiesCameraId == effectiveCameraId &&
            m_camera2OverrideProfileMtime == overrideProfileMtime) {
        return m_camera2CapabilitiesValid;
    }

    m_camera2CapabilitiesLoaded = true;
    m_camera2CapabilitiesValid = false;
    m_camera2CapabilitiesCameraId = effectiveCameraId;
    m_camera2OverrideProfileMtime = overrideProfileMtime;
    m_camera2Raw16Sizes.clear();
    m_camera2Raw10Sizes.clear();
    m_camera2JpegSizes.clear();
    m_camera2PreviewSizes.clear();
    m_camera2FocusModes.clear();
    m_camera2Scenes.clear();
    m_camera2NoiseReductionModes.clear();
    m_camera2ManualExposureSupported = false;
    m_camera2ManualBracketingSupported = false;
    m_camera2HalMaxShutterNs = 0;
    m_camera2MaxShutterNs = 0;
    m_camera2DeviceProfile = QJsonObject();
    m_camera2DeviceProfileId.clear();
    m_camera2DeviceProfileSource.clear();
    m_camera2ProbeJson.clear();
    m_camera2ProbeErrors.clear();
    m_camera2SelectedCamera = QJsonObject();

    if (!rawImageCaptureAvailable()) {
        m_camera2ProbeErrors = QByteArrayLiteral("Camera2 helper is not installed");
        return false;
    }

    QProcess probe;
    probe.start(rawCaptureProbePath());
    if (!probe.waitForStarted(3000) || !probe.waitForFinished(5000) ||
            probe.exitStatus() != QProcess::NormalExit ||
            probe.exitCode() != 0) {
        m_camera2ProbeJson = probe.readAllStandardOutput();
        m_camera2ProbeErrors = probe.readAllStandardError();
        return false;
    }

    m_camera2ProbeJson = probe.readAllStandardOutput();
    m_camera2ProbeErrors = probe.readAllStandardError();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(m_camera2ProbeJson,
                                                           &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_camera2ProbeErrors += QByteArrayLiteral("\nInvalid Camera2 probe JSON");
        return false;
    }

    const QJsonArray cameras = document.object().value(
                QStringLiteral("cameras")).toArray();
    QJsonObject selectedCamera;
    for (const QJsonValue &value : cameras) {
        const QJsonObject camera = value.toObject();
        if (camera.value(QStringLiteral("id")).toString() == effectiveCameraId) {
            selectedCamera = camera;
            break;
        }
        if (selectedCamera.isEmpty() &&
                camera.value(QStringLiteral("status")).toString() == QLatin1String("ok")) {
            selectedCamera = camera;
        }
    }
    if (selectedCamera.isEmpty()) {
        return false;
    }

    const QStringList matchRaw16Sizes = camera2OutputSizes(
                selectedCamera, QStringLiteral("raw_outputs"));
    qint64 matchHalMaxShutterNs = 0;
    const QJsonArray matchShutterRange = selectedCamera
            .value(QStringLiteral("exposure")).toObject()
            .value(QStringLiteral("shutter_ns_range")).toArray();
    if (matchShutterRange.count() >= 2) {
        matchHalMaxShutterNs = qint64(matchShutterRange.at(1).toDouble());
    }
    const QVector<QPair<QJsonObject, QString> > profiles = procamDeviceProfiles();
    for (const QPair<QJsonObject, QString> &candidate : profiles) {
        if (overrideProfileMatches(candidate.first, selectedCamera,
                                   matchRaw16Sizes, matchHalMaxShutterNs,
                                   profiles.count())) {
            const QJsonObject candidateObject = candidate.first;
            QJsonObject merged = m_camera2DeviceProfile;
            for (QJsonObject::const_iterator it = candidateObject.constBegin();
                 it != candidateObject.constEnd(); ++it) {
                if (it.key() != QLatin1String("overrides") &&
                        it.key() != QLatin1String("hal")) {
                    merged.insert(it.key(), it.value());
                }
            }

            const QJsonObject candidateOverrides = candidateObject
                    .value(QStringLiteral("overrides")).toObject();
            QJsonObject overrides = merged.value(
                        QStringLiteral("overrides")).toObject();
            overrides = mergeJsonObjects(overrides, candidateOverrides);
            merged.insert(QStringLiteral("overrides"), overrides);
            m_camera2DeviceProfile = merged;
            m_camera2DeviceProfileSource = candidate.second;
            m_camera2DeviceProfileId = merged.value(
                        QStringLiteral("id")).toString(QStringLiteral("unnamed"));
            selectedCamera = mergeJsonObjects(selectedCamera, candidateOverrides);
            qDebug() << "override-hal"
                     << "matched=" + m_camera2DeviceProfileId
                     << "camera=" + selectedCamera.value(QStringLiteral("id")).toString();
        } else {
            const QJsonObject match = candidate.first.value(
                        QStringLiteral("match")).toObject();
            qDebug() << "override-hal"
                     << "matched=0"
                     << "camera=" + selectedCamera.value(QStringLiteral("id")).toString()
                     << "profile=" + candidate.first.value(
                            QStringLiteral("id")).toString(QStringLiteral("unnamed"))
                     << "match_camera=" + match.value(
                            QStringLiteral("camera_id")).toString(QStringLiteral("unset"));
        }
    }

    m_camera2SelectedCamera = selectedCamera;
    m_camera2Raw16Sizes = camera2OutputSizes(
                selectedCamera, QStringLiteral("raw_outputs"));
    m_camera2Raw10Sizes = camera2OutputSizes(
                selectedCamera, QStringLiteral("raw10_outputs"));
    m_camera2JpegSizes = camera2OutputSizes(
                selectedCamera, QStringLiteral("jpeg_outputs"));
    m_camera2PreviewSizes = camera2OutputSizes(
                selectedCamera, QStringLiteral("preview_outputs"));
    const QJsonObject focus = selectedCamera.value(QStringLiteral("focus")).toObject();
    const QStringList androidFocusModes = camera2NamedValues(
                selectedCamera, QStringLiteral("focus"), QStringLiteral("af_modes"));
    if (androidFocusModes.contains(QStringLiteral("auto"))) {
        m_camera2FocusModes.append(QStringLiteral("auto"));
    }
    if (androidFocusModes.contains(QStringLiteral("continuous_picture")) ||
            androidFocusModes.contains(QStringLiteral("continuous_video"))) {
        m_camera2FocusModes.append(QStringLiteral("continuous"));
    }
    if (!focus.value(QStringLiteral("fixed_focus")).toBool(false)) {
        m_camera2FocusModes.append(QStringLiteral("manual"));
        m_camera2FocusModes.append(QStringLiteral("infinity"));
    }
    if (!m_camera2FocusModes.contains(QStringLiteral("none"))) {
        m_camera2FocusModes.append(QStringLiteral("none"));
    }
    m_camera2Scenes = camera2NamedValues(selectedCamera, QStringLiteral("scene"),
                                         QStringLiteral("modes"));
    const QJsonArray excludedScenes = procamOverrideObject(m_camera2DeviceProfile)
            .value(QStringLiteral("scene_exclude")).toArray();
    for (const QJsonValue &value : excludedScenes) {
        if (!value.isString()) {
            continue;
        }
        const QString name = value.toString();
        // Keep the ordinary capture modes available as normalization fallbacks.
        if (name != QLatin1String("manual") && name != QLatin1String("auto")) {
            m_camera2Scenes.removeAll(name);
        }
    }
    if (!m_camera2Scenes.contains(QStringLiteral("manual"))) {
        m_camera2Scenes.prepend(QStringLiteral("manual"));
    }
    if (!m_camera2Scenes.contains(QStringLiteral("auto"))) {
        m_camera2Scenes.insert(qMin(1, m_camera2Scenes.length()),
                               QStringLiteral("auto"));
    }
    m_camera2NoiseReductionModes = camera2NoiseReductionValues(selectedCamera);
    const QJsonObject exposure = selectedCamera.value(
                QStringLiteral("exposure")).toObject();
    m_camera2ManualExposureSupported = exposure.value(
                QStringLiteral("manual_supported")).toBool(false);
    const QJsonArray shutterRange = exposure.value(
                QStringLiteral("shutter_ns_range")).toArray();
    if (shutterRange.count() >= 2) {
        m_camera2HalMaxShutterNs = qint64(shutterRange.at(1).toDouble());
        m_camera2MaxShutterNs = m_camera2HalMaxShutterNs;
    }
    const QJsonObject overrides = m_camera2DeviceProfile.value(
                QStringLiteral("overrides")).toObject();
    if (!overrides.value(QStringLiteral("raw10_enabled")).toBool(true)) {
        m_camera2Raw10Sizes.clear();
    }
    const qint64 observedMaxShutterNs = qint64(overrides.value(
                QStringLiteral("observed_shutter_max_ns")).toDouble());
    if (observedMaxShutterNs > 0 &&
            (m_camera2MaxShutterNs <= 0 ||
             observedMaxShutterNs < m_camera2MaxShutterNs)) {
        m_camera2MaxShutterNs = observedMaxShutterNs;
    }
    if (m_camera2HalMaxShutterNs > 0) {
        qDebug() << "shutter-limit"
                 << "camera=" + selectedCamera.value(QStringLiteral("id")).toString()
                 << "hal_max=" + QString::number(m_camera2HalMaxShutterNs)
                 << "observed_max=" + QString::number(m_camera2MaxShutterNs)
                 << "profile=" + (m_camera2DeviceProfileId.isEmpty()
                                   ? QStringLiteral("none")
                                   : m_camera2DeviceProfileId);
    }
    if (!m_camera2DeviceProfileId.isEmpty()) {
        qDebug() << "device-profile"
                 << "id=" + m_camera2DeviceProfileId
                 << "source=" + m_camera2DeviceProfileSource
                 << "hal_overrides_applied=1";
    }
    m_camera2ManualBracketingSupported = selectedCamera.value(
                QStringLiteral("hdr_dol")).toObject()
            .value(QStringLiteral("manual_bracketing_supported")).toBool(false);
    m_camera2CapabilitiesValid = true;
    return true;
}

QStringList DeclarativeCameraExtensions::camera2RawSizeModel(
        const QString &cameraId, const QString &rawFormat)
{
    const bool valid = loadCamera2Capabilities(cameraId);
    if (rawFormat == QLatin1String("raw10")) {
        return valid ? m_camera2Raw10Sizes : QStringList();
    }
    if (valid && !m_camera2Raw16Sizes.isEmpty()) {
        return m_camera2Raw16Sizes;
    }
    return QStringList();
}

QSize DeclarativeCameraExtensions::camera2PreferredPreviewSize(const QString &cameraId)
{
    const QString id = cameraId.isEmpty() ? QStringLiteral("0") : cameraId;
    if (!loadCamera2Capabilities(id))
        return QSize();
    const QJsonObject camera = camera2ProbeCamera(m_camera2ProbeJson, id);
    if (camera.value(QStringLiteral("status")).toString() != QLatin1String("ok") ||
            m_camera2SelectedCamera.value(QStringLiteral("id")).toString() != id)
        return QSize();
    // 1440x1080: sharper than 1280x960 now that preview frames travel as
    // YUV, and still within Camera2's guaranteed PREVIEW size, so the HAL
    // keeps the RAW stream alongside it.
    return preferredPreviewSize(camera2OutputSizes(camera, QStringLiteral("preview_outputs")),
                                m_camera2PreviewSizes, QSize(1440, 1080));
}

int DeclarativeCameraExtensions::camera2PreviewOrientation(const QString &cameraId,
                                                          int fallback)
{
    const QString id = cameraId.isEmpty() ? QStringLiteral("0") : cameraId;
    if (!loadCamera2Capabilities(id))
        return fallback;
    const QJsonObject camera = camera2ProbeCamera(m_camera2ProbeJson, id);
    if (camera.value(QStringLiteral("status")).toString() != QLatin1String("ok") ||
            m_camera2SelectedCamera.value(QStringLiteral("id")).toString() != id)
        return fallback;
    const QJsonValue value = procamOverrideObject(m_camera2DeviceProfile)
            .value(QStringLiteral("preview_orientation"));
    if (value.isDouble()) {
        const double degrees = value.toDouble();
        if (degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270)
            return int(degrees);
    }
    // SENSOR_ORIENTATION: clockwise rotation that makes the image upright
    // (front cameras are usually mounted the other way round, 270).
    const int sensor = camera.value(QStringLiteral("lens")).toObject()
            .value(QStringLiteral("sensor_orientation")).toInt(-1);
    if (sensor == 0 || sensor == 90 || sensor == 180 || sensor == 270)
        return sensor;
    return fallback;
}

bool DeclarativeCameraExtensions::camera2PreviewMirror(const QString &cameraId,
                                                      bool fallback)
{
    const QString id = cameraId.isEmpty() ? QStringLiteral("0") : cameraId;
    if (!loadCamera2Capabilities(id))
        return fallback;
    const QJsonObject camera = camera2ProbeCamera(m_camera2ProbeJson, id);
    if (camera.value(QStringLiteral("status")).toString() != QLatin1String("ok") ||
            m_camera2SelectedCamera.value(QStringLiteral("id")).toString() != id)
        return fallback;
    const QJsonValue value = procamOverrideObject(m_camera2DeviceProfile)
            .value(QStringLiteral("preview_mirror"));
    if (value.isBool())
        return value.toBool();
    // Front cameras preview mirrored, like every phone camera app.
    const QString facing = camera.value(QStringLiteral("lens")).toObject()
            .value(QStringLiteral("facing")).toString();
    if (facing == QLatin1String("front"))
        return true;
    if (facing == QLatin1String("back"))
        return false;
    return fallback;
}

QString DeclarativeCameraExtensions::camera2LensFacing(const QString &cameraId)
{
    if (m_camera2ProbeJson.isEmpty() && !loadCamera2Capabilities(cameraId))
        return QStringLiteral("unknown");
    const QJsonObject camera = camera2ProbeCamera(m_camera2ProbeJson, cameraId);
    if (camera.value(QStringLiteral("id")).toString() != cameraId)
        return QStringLiteral("unknown");
    const QString facing = camera.value(QStringLiteral("lens")).toObject()
            .value(QStringLiteral("facing")).toString();
    return facing.isEmpty() ? QStringLiteral("unknown") : facing;
}

QVariantMap DeclarativeCameraExtensions::camera2CameraRoles()
{
    QVariantMap roles;
    roles.insert(QStringLiteral("front"), QString());
    roles.insert(QStringLiteral("main"), QString());
    roles.insert(QStringLiteral("ultrawide"), QString());
    if (m_camera2ProbeJson.isEmpty() && !loadCamera2Capabilities(QString()))
        return roles;

    struct Lens {
        QString id;
        QString facing;
        bool raw = false;
        double focal = 0, sensorWidth = 0;
        // Horizontal field of view in radians, 0 when unknown.
        double fov() const
        {
            return focal > 0 && sensorWidth > 0 ? 2 * std::atan(sensorWidth / (2 * focal)) : 0;
        }
    };
    QVector<Lens> back;
    for (const QJsonValue &value : camera2ProbeCameras(m_camera2ProbeJson)) {
        const QJsonObject camera = value.toObject();
        if (camera.value(QStringLiteral("status")).toString() != QLatin1String("ok"))
            continue;
        const QJsonObject lens = camera.value(QStringLiteral("lens")).toObject();
        Lens entry;
        entry.id = camera.value(QStringLiteral("id")).toString();
        entry.facing = lens.value(QStringLiteral("facing")).toString();
        entry.raw = !camera.value(QStringLiteral("raw_outputs")).toArray().isEmpty();
        entry.focal = lens.value(QStringLiteral("focal_length_mm")).toDouble();
        entry.sensorWidth = lens.value(QStringLiteral("sensor_physical_size_mm")).toArray().at(0).toDouble();
        if (entry.id.isEmpty())
            continue;
        if (entry.facing == QLatin1String("front")) {
            if (roles.value(QStringLiteral("front")).toString().isEmpty())
                roles.insert(QStringLiteral("front"), entry.id);
        } else if (entry.facing == QLatin1String("back")) {
            back.append(entry);
        }
    }
    if (back.isEmpty())
        return roles; // older bridge without facing: callers fall back

    // Main: the back camera with RAW output (the primary sensor), else the first.
    const Lens *main = &back.first();
    for (const Lens &lens : back) {
        if (lens.raw) {
            main = &lens;
            break;
        }
    }
    roles.insert(QStringLiteral("main"), main->id);
    // Ultra-wide: the widest other back camera, clearly wider than the main
    // one. Without sensor sizes, a much shorter focal length.
    const Lens *wide = nullptr;
    for (const Lens &lens : back) {
        if (lens.id == main->id)
            continue;
        const bool wider = main->fov() > 0 && lens.fov() > 0
                ? lens.fov() > main->fov() * 1.15
                : lens.focal > 0 && main->focal > 0 && lens.focal < main->focal * 0.75;
        if (!wider)
            continue;
        if (!wide || (lens.fov() > 0 && wide->fov() > 0 ? lens.fov() > wide->fov() : lens.focal < wide->focal))
            wide = &lens;
    }
    if (wide)
        roles.insert(QStringLiteral("ultrawide"), wide->id);
    return roles;
}

QVariantMap DeclarativeCameraExtensions::camera2SimpleModeOverrides(const QString &cameraId)
{
    const QString id = cameraId.isEmpty() ? QStringLiteral("0") : cameraId;
    if (!loadCamera2Capabilities(id)
            || m_camera2SelectedCamera.value(QStringLiteral("id")).toString() != id
            || camera2ProbeCamera(m_camera2ProbeJson, id)
                .value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
        return QVariantMap();
    }
    return procamOverrideObject(m_camera2DeviceProfile)
            .value(QStringLiteral("simple_mode")).toObject().toVariantMap();
}

QStringList DeclarativeCameraExtensions::camera2JpegSizeModel(
        const QString &cameraId)
{
    if (loadCamera2Capabilities(cameraId) && !m_camera2JpegSizes.isEmpty()) {
        return m_camera2JpegSizes;
    }
    return QStringList();
}

QStringList DeclarativeCameraExtensions::camera2CaptureFormatModel(
        const QString &cameraId)
{
    QStringList formats;
    if (loadCamera2Capabilities(cameraId)) {
        if (!m_camera2JpegSizes.isEmpty()) {
            formats.append(QStringLiteral("jpeg"));
        }
        if (!m_camera2Raw16Sizes.isEmpty() || !m_camera2Raw10Sizes.isEmpty()) {
            formats.append(QStringLiteral("raw"));
        }
    } else if (rawImageCaptureAvailable()) {
        formats.append(QStringLiteral("jpeg"));
    }
    if (formats.isEmpty()) {
        formats.append(QStringLiteral("jpeg"));
    }
    return formats;
}

QStringList DeclarativeCameraExtensions::camera2LensModel(const QString &cameraId)
{
    if (!loadCamera2Capabilities(cameraId)) {
        return cameraId.isEmpty() ? QStringList() : QStringList() << cameraId;
    }

    QStringList ids;
    const QJsonArray cameras = camera2ProbeCameras(m_camera2ProbeJson);
    for (const QJsonValue &value : cameras) {
        const QJsonObject camera = value.toObject();
        if (camera.value(QStringLiteral("status")).toString()
                != QLatin1String("ok")) {
            continue;
        }
        const QString id = camera.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
        }
    }
    if (ids.isEmpty() && !m_camera2SelectedCamera.isEmpty()) {
        ids.append(m_camera2SelectedCamera.value(QStringLiteral("id")).toString());
    }
    return ids;
}

QString DeclarativeCameraExtensions::camera2LensLabel(const QString &cameraId)
{
    if (m_camera2ProbeJson.isEmpty() && !loadCamera2Capabilities(cameraId)) {
        return QStringLiteral("-- MM");
    }

    QJsonObject camera = camera2ProbeCamera(m_camera2ProbeJson, cameraId);
    if (camera.isEmpty() ||
            camera.value(QStringLiteral("id")).toString() != cameraId) {
        camera = m_camera2SelectedCamera;
    }
    return camera2FocalLengthLabel(camera);
}

QStringList DeclarativeCameraExtensions::camera2FocusModeModel(
        const QString &cameraId)
{
    if (loadCamera2Capabilities(cameraId) && !m_camera2FocusModes.isEmpty()) {
        return m_camera2FocusModes;
    }
    return QStringList() << QStringLiteral("none");
}

QVariantMap DeclarativeCameraExtensions::camera2ExposureCapabilities(const QString &cameraId)
{
    const QString id = cameraId.isEmpty() ? QStringLiteral("0") : cameraId;
    if (!loadCamera2Capabilities(id)) return QVariantMap();
    QJsonObject exposure = cameraExposureObject(camera2ProbeCamera(m_camera2ProbeJson, id));
    if (m_camera2SelectedCamera.value(QStringLiteral("id")).toString() == id) {
        // Honour observed/profile shutter limits only by narrowing the hardware range.
        QJsonArray range = exposure.value(QStringLiteral("shutter_ns_range")).toArray();
        const QJsonArray effective = cameraExposureObject(m_camera2SelectedCamera).value(QStringLiteral("shutter_ns_range")).toArray();
        if (range.size() == 2 && effective.size() == 2) {
            range[0] = qMax(range[0].toDouble(), effective[0].toDouble());
            range[1] = qMin(range[1].toDouble(), effective[1].toDouble());
            if (m_camera2MaxShutterNs > 0)
                range[1] = qMin(range[1].toDouble(), double(m_camera2MaxShutterNs));
            exposure.insert(QStringLiteral("shutter_ns_range"), range);
        }
    }
    return exposure.toVariantMap();
}

QStringList DeclarativeCameraExtensions::camera2SceneModel(const QString &cameraId)
{
    if (loadCamera2Capabilities(cameraId) && !m_camera2Scenes.isEmpty()) {
        return m_camera2Scenes;
    }
    return QStringList() << QStringLiteral("manual") << QStringLiteral("auto");
}

QVariantList DeclarativeCameraExtensions::camera2NoiseReductionModel(
        const QString &cameraId)
{
    if (loadCamera2Capabilities(cameraId) && !m_camera2NoiseReductionModes.isEmpty()) {
        return m_camera2NoiseReductionModes;
    }
    QVariantList values;
    values.append(0);
    return values;
}

QVariantList DeclarativeCameraExtensions::camera2IsoModel(const QString &cameraId)
{
    QVariantList values;
    values.append(0);

    if (!loadCamera2Capabilities(cameraId)) {
        return values;
    }

    const QJsonArray isoRange = cameraIsoRange(m_camera2SelectedCamera);
    const int minIso = isoRange.count() >= 2 ? isoRange.at(0).toInt() : 0;
    const int maxIso = isoRange.count() >= 2 ? isoRange.at(1).toInt() : 0;
    for (int iso : camera2IsoSteps()) {
        if ((minIso <= 0 || iso >= minIso) &&
                (maxIso <= 0 || iso <= maxIso)) {
            values.append(iso);
        }
    }
    if (values.count() == 1 && minIso > 0) {
        values.append(minIso);
    }
    return values;
}

QStringList DeclarativeCameraExtensions::camera2BracketModel(
        const QString &cameraId)
{
    QStringList model;
    model.append(QStringLiteral("off"));
    if (loadCamera2Capabilities(cameraId) && m_camera2ManualBracketingSupported
            && cameraExposureObject(m_camera2SelectedCamera).value(QStringLiteral("manual_supported")).toBool()
            && cameraExposureObject(m_camera2SelectedCamera).value(QStringLiteral("result_exposure_supported")).toBool()
            && (!m_camera2Raw16Sizes.isEmpty() || !m_camera2Raw10Sizes.isEmpty())) {
        model.append(QStringLiteral("ev2"));
        model.append(QStringLiteral("ev1"));
        model.append(QStringLiteral("ev3"));
    }
    return model;
}

bool DeclarativeCameraExtensions::camera2BurstSupported(const QString &cameraId)
{
    return loadCamera2Capabilities(cameraId) && m_camera2ManualBracketingSupported
            && cameraExposureObject(m_camera2SelectedCamera).value(QStringLiteral("manual_supported")).toBool()
            && cameraExposureObject(m_camera2SelectedCamera).value(QStringLiteral("result_exposure_supported")).toBool()
            && !camera2BurstRawSize(cameraId, QString()).isEmpty();
}

QString DeclarativeCameraExtensions::camera2BurstFormat(const QString &cameraId)
{
    if (!loadCamera2Capabilities(cameraId)) return QString();
    if (!m_camera2Raw16Sizes.isEmpty())
        return camera2BurstRawSize(cameraId, QString()).isEmpty() ? QString() : QStringLiteral("raw16");
    return camera2BurstRawSize(cameraId, QString()).isEmpty() ? QString() : QStringLiteral("yuv");
}

QString DeclarativeCameraExtensions::camera2BurstRawSize(const QString &cameraId,
                                                        const QString &preferred)
{
    if (!loadCamera2Capabilities(cameraId)) return QString();
    if (m_camera2Raw16Sizes.isEmpty()) {
        // No RAW: a YUV burst at up to 16 MP in the aspect ratio of the
        // largest output (the sensor's), e.g. the binned mode of a quad-Bayer
        // front sensor rather than its upscaled full resolution.
        qint64 largestPixels = 0;
        double aspect = 4.0 / 3.0;
        for (const QString &size : m_camera2PreviewSizes) {
            const QStringList parts = size.split(QLatin1Char('x'));
            const qint64 pixels = parts.size() == 2 ? qint64(parts[0].toInt()) * parts[1].toInt() : 0;
            if (pixels > largestPixels) {
                largestPixels = pixels;
                aspect = double(parts[0].toInt()) / std::max(1, parts[1].toInt());
            }
        }
        QString best;
        qint64 bestPixels = 0;
        for (const QString &size : m_camera2PreviewSizes) {
            const QStringList parts = size.split(QLatin1Char('x'));
            if (parts.size() != 2) continue;
            const int width = parts[0].toInt(), height = parts[1].toInt();
            const qint64 pixels = qint64(width) * height;
            if (height <= 0 || pixels > qint64(4096) * 4096 || pixels <= bestPixels
                    || std::abs(double(width) / height - aspect) > 0.02)
                continue;
            best = size;
            bestPixels = pixels;
        }
        return best;
    }
    if (camera2WarmRawCaptureSupported(cameraId, preferred)) return preferred;
    QString best;
    qint64 bestPixels = 0;
    for (const QString &size : m_camera2Raw16Sizes) {
        const QStringList parts = size.split(QLatin1Char('x'));
        const qint64 pixels = parts.size() == 2 ? qint64(parts[0].toInt()) * parts[1].toInt() : 0;
        if (pixels > bestPixels && camera2WarmRawCaptureSupported(cameraId, size)) {
            best = size;
            bestPixels = pixels;
        }
    }
    return best;
}

void DeclarativeCameraExtensions::logCaptureEvent(const QString &message)
{
    appendRawCaptureLog(QStringLiteral("qml ") + message);
}

QVariantMap DeclarativeCameraExtensions::camera2BurstPlan(
        const QString &cameraId, const QString &meteredShutterNs, int meteredIso,
        const QVariantList &histogram, bool night, const QVariantMap &scene)
{
    QVariantMap result;
    if (!loadCamera2Capabilities(cameraId)) return result;
    const QJsonArray range = cameraExposureObject(m_camera2SelectedCamera)
            .value(QStringLiteral("shutter_ns_range")).toArray();
    if (range.size() != 2 || range[0].toDouble() <= 0) return result;
    // Share of preview pixels in the top histogram bin, worst channel.
    double clipped = 0;
    for (const QString &channel : {QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b")}) {
        double total = 0;
        for (const QVariant &bin : histogram) total += bin.toMap().value(channel).toDouble();
        if (total > 0) clipped = std::max(clipped, histogram.last().toMap().value(channel).toDouble() / total);
    }
    qint64 maxShutter = qint64(range[1].toDouble());
    if (m_camera2MaxShutterNs > 0) maxShutter = qMin(maxShutter, m_camera2MaxShutterNs);
    // YUV frames have the tone curve applied: equal exposures, no headroom.
    const bool yuv = m_camera2Raw16Sizes.isEmpty();
    RawPipeline::BurstScene burstScene;
    burstScene.motion = scene.value(QStringLiteral("motion"), -1.0).toDouble();
    burstScene.subjectMotion = scene.value(QStringLiteral("subjectMotion"), 0.0).toDouble();
    burstScene.faceLuma = scene.value(QStringLiteral("faceLuma"), -1.0).toDouble();
    burstScene.faceClipped = scene.value(QStringLiteral("faceClipped"), 0.0).toDouble();
    const RawPipeline::BurstPlan plan = RawPipeline::planBurst(
                meteredShutterNs.toLongLong(), meteredIso, clipped, qint64(range[0].toDouble()),
                maxShutter, int(camera2IsoMin(m_camera2SelectedCamera)),
                // Night mode: 16 frames (a stop cleaner); a normal burst
                // gains little beyond 8 indoors and would only take longer.
                int(camera2IsoMax(m_camera2SelectedCamera)), night ? 16 : 8, yuv, night, burstScene);
    if (plan.frames < 2) return result;
    result.insert(QStringLiteral("frames"), plan.frames);
    result.insert(QStringLiteral("shutterNs"), QString::number(plan.exposureNs));
    result.insert(QStringLiteral("iso"), plan.iso);
    result.insert(QStringLiteral("targetGain"), plan.targetGain);
    result.insert(QStringLiteral("underexposureEv"), plan.underexposureEv);
    result.insert(QStringLiteral("clippedFraction"), clipped);
    result.insert(QStringLiteral("night"), night);
    return result;
}

QString DeclarativeCameraExtensions::camera2MaxShutterNs(const QString &cameraId)
{
    return loadCamera2Capabilities(cameraId) && m_camera2MaxShutterNs > 0
            ? QString::number(m_camera2MaxShutterNs) : QString();
}

QString DeclarativeCameraExtensions::camera2BracketShutterNs(
        const QString &cameraId, const QString &base, int ev)
{
    if (!loadCamera2Capabilities(cameraId)) return QStringLiteral("0");
    const QJsonArray range = cameraExposureObject(m_camera2SelectedCamera)
            .value(QStringLiteral("shutter_ns_range")).toArray();
    const double time = base.toDouble();
    if (range.size() != 2 || !std::isfinite(time) || time <= 0 || ev > 0 || ev < -6
            || range[0].toDouble() <= 0 || m_camera2MaxShutterNs < range[0].toDouble())
        return QStringLiteral("0");
    return QString::number(qint64(std::round(qBound(range[0].toDouble(),
                    time * std::pow(2.0, ev), double(m_camera2MaxShutterNs)))));
}

QStringList DeclarativeCameraExtensions::camera2ShutterModel(const QString &cameraId)
{
    const QStringList values = camera2ShutterSteps();
    if (!loadCamera2Capabilities(cameraId) || m_camera2MaxShutterNs <= 0) {
        return values;
    }
    const QJsonArray shutterRange = cameraExposureObject(m_camera2SelectedCamera)
            .value(QStringLiteral("shutter_ns_range")).toArray();
    const qint64 minShutterNs = shutterRange.count() >= 2
            ? qint64(shutterRange.at(0).toDouble()) : 0;
    QStringList filtered;
    for (const QString &value : values) {
        const qint64 shutterNs = value.toLongLong();
        if (value == QLatin1String("0") ||
                ((minShutterNs <= 0 || shutterNs >= minShutterNs) &&
                 shutterNs <= m_camera2MaxShutterNs)) {
            filtered.append(value);
        }
    }
    return filtered;
}

QStringList DeclarativeCameraExtensions::camera2FocusDistanceModel(
        const QString &cameraId)
{
    QStringList values;
    values << QStringLiteral("0");
    if (!loadCamera2Capabilities(cameraId)) {
        return values;
    }

    const qreal maximumDiopters = cameraFocusObject(m_camera2SelectedCamera)
            .value(QStringLiteral("minimum_focus_distance_diopters")).toDouble();
    if (maximumDiopters <= 0.0) {
        return values;
    }

    for (const QString &value : camera2FocusDistanceSteps()) {
        if (value.toDouble() <= maximumDiopters) {
            values.append(value);
        }
    }
    if (values.count() == 1 || values.last().toDouble() < maximumDiopters) {
        values.append(QString::number(maximumDiopters, 'g', 6));
    }
    return values;
}

qreal DeclarativeCameraExtensions::camera2MaximumZoom(const QString &cameraId)
{
    return loadCamera2Capabilities(cameraId)
            ? camera2ZoomMaximum(m_camera2SelectedCamera) : 1.0;
}

QStringList DeclarativeCameraExtensions::rawCaptureExposureModel(
        const QString &cameraId)
{
    const QStringList defaults = rawCaptureExposureDefaults();
    if (!loadCamera2Capabilities(cameraId)) {
        return defaults;
    }
    return stringListFromJsonArray(
                procamOverrideObject(m_camera2DeviceProfile)
                .value(QStringLiteral("exposure_multipliers")).toArray(),
                defaults);
}

QVariantList DeclarativeCameraExtensions::rawCaptureRotationModel(
        const QString &cameraId, const QString &mode)
{
    const QVariantList defaults = rawCaptureRotationDefaults();
    if (!loadCamera2Capabilities(cameraId)) {
        return defaults;
    }

    const QJsonObject rotationValues = procamOverrideObject(m_camera2DeviceProfile)
            .value(QStringLiteral("rotation_values")).toObject();
    QJsonArray values = rotationValues.value(mode).toArray();
    if (values.isEmpty()) {
        values = rotationValues.value(QStringLiteral("default")).toArray();
    }
    return intListFromJsonArray(values, defaults);
}

QVariantList DeclarativeCameraExtensions::rawCaptureFocusTimeoutModel(
        const QString &cameraId)
{
    const QVariantList defaults = rawCaptureAfWaitDefaults();
    if (!loadCamera2Capabilities(cameraId)) {
        return defaults;
    }
    return intListFromJsonArray(
                procamOverrideObject(m_camera2DeviceProfile)
                .value(QStringLiteral("af_wait_times")).toArray(),
                defaults);
}

QStringList DeclarativeCameraExtensions::rawCaptureRawFormatModel(
        const QString &cameraId)
{
    QStringList formats;
    if (loadCamera2Capabilities(cameraId) && !m_camera2Raw16Sizes.isEmpty()) {
        formats.append(QStringLiteral("raw16"));
    }
    if (raw10CaptureAvailable(cameraId)) {
        formats.append(QStringLiteral("raw10"));
    }
    return formats;
}

bool DeclarativeCameraExtensions::raw10CaptureAvailable(const QString &cameraId)
{
    return loadCamera2Capabilities(cameraId) && !m_camera2Raw10Sizes.isEmpty();
}

QString DeclarativeCameraExtensions::camera2PreferredCaptureSize(
        const QString &cameraId, const QString &format)
{
    if (!loadCamera2Capabilities(cameraId)) {
        return QString();
    }

    const QJsonObject overrides = m_camera2DeviceProfile.value(
                QStringLiteral("overrides")).toObject();
    const QString formatKey = format == QLatin1String("raw")
            ? QStringLiteral("preferred_raw_capture_size")
            : QStringLiteral("preferred_jpeg_capture_size");
    QString size = overrides.value(formatKey).toString();
    if (size.isEmpty()) {
        size = overrides.value(QStringLiteral("preferred_capture_size")).toString();
    }

    const QStringList sizes = format == QLatin1String("raw")
            ? m_camera2Raw16Sizes : m_camera2JpegSizes;
    if (!size.isEmpty() && sizes.contains(size)) {
        return size;
    }
    // The largest size up to 16 MP: beyond that, quad-Bayer sensors only give
    // the HAL's remosaicked output, which is softer than the binned frame.
    QString best;
    qint64 bestPixels = 0;
    for (const QString &candidate : sizes) {
        const QStringList parts = candidate.split(QLatin1Char('x'));
        const qint64 pixels = parts.size() == 2 ? qint64(parts[0].toInt()) * parts[1].toInt() : 0;
        if (pixels > bestPixels && pixels <= qint64(4096) * 4096) {
            best = candidate;
            bestPixels = pixels;
        }
    }
    return !best.isEmpty() ? best : sizes.isEmpty() ? QString() : sizes.first();
}

QString DeclarativeCameraExtensions::camera2WarmCaptureSize(const QString &cameraId)
{
    if (!loadCamera2Capabilities(cameraId)) {
        return QString();
    }

    const QJsonObject overrides = m_camera2DeviceProfile.value(
                QStringLiteral("overrides")).toObject();
    const QString size = overrides.value(QStringLiteral("warm_capture_size")).toString();
    if (!size.isEmpty() &&
            (m_camera2Raw16Sizes.contains(size) || m_camera2JpegSizes.contains(size))) {
        return size;
    }
    return camera2PreferredCaptureSize(cameraId, QStringLiteral("jpeg"));
}

bool DeclarativeCameraExtensions::camera2WarmJpegCaptureSupported(const QString &cameraId,
                                                                  const QString &size)
{
    if (!loadCamera2Capabilities(cameraId) || !m_camera2JpegSizes.contains(size)) {
        return false;
    }
    const QString pinned = m_camera2DeviceProfile.value(QStringLiteral("overrides")).toObject()
            .value(QStringLiteral("warm_capture_size")).toString();
    if (!pinned.isEmpty()) {
        return size == pinned;
    }
    const QStringList parts = size.split(QLatin1Char('x'));
    return parts.size() == 2
            && qint64(parts[0].toInt()) * parts[1].toInt() <= qint64(4096) * 4096;
}

bool DeclarativeCameraExtensions::camera2WarmRawCaptureSupported(const QString &cameraId,
                                                                 const QString &size)
{
    if (!loadCamera2Capabilities(cameraId) || !m_camera2Raw16Sizes.contains(size)) {
        return false;
    }
    const QString pinned = m_camera2DeviceProfile.value(QStringLiteral("overrides")).toObject()
            .value(QStringLiteral("warm_raw_capture_size")).toString();
    if (!pinned.isEmpty()) {
        return size == pinned;
    }
    // Up to 16 MP: larger RAW16 frames make the preview session and bursts
    // too heavy. The bridge drops the RAW stream if the HAL refuses it.
    const QStringList parts = size.split(QLatin1Char('x'));
    return parts.size() == 2
            && qint64(parts[0].toInt()) * parts[1].toInt() <= qint64(4096) * 4096;
}

QString DeclarativeCameraExtensions::preferredCamera2CameraId(const QString &cameraId)
{
    if (loadCamera2Capabilities(cameraId)) {
        const QString selectedId = m_camera2SelectedCamera.value(
                    QStringLiteral("id")).toString();
        if (!selectedId.isEmpty()) {
            return selectedId;
        }
    }
    return cameraId.isEmpty() ? QStringLiteral("0") : cameraId;
}

QString DeclarativeCameraExtensions::camera2CompatibilityLevel(
        const QString &cameraId)
{
    if (!loadCamera2Capabilities(cameraId)) {
        return rawImageCaptureAvailable() ? QStringLiteral("Basic")
                                          : QStringLiteral("Unavailable");
    }
    if (!m_camera2JpegSizes.isEmpty() &&
            !m_camera2Raw16Sizes.isEmpty() &&
            !m_camera2PreviewSizes.isEmpty()) {
        return QStringLiteral("Full");
    }
    if (!m_camera2JpegSizes.isEmpty() || !m_camera2Raw16Sizes.isEmpty()) {
        return QStringLiteral("Limited");
    }
    return QStringLiteral("Basic");
}

QString DeclarativeCameraExtensions::camera2CompatibilitySummary(
        const QString &cameraId)
{
    const QString level = camera2CompatibilityLevel(cameraId);
    if (!m_camera2CapabilitiesValid) {
        return QStringLiteral("%1 Camera2 support").arg(level);
    }
    QStringList features;
    if (!m_camera2JpegSizes.isEmpty()) {
        features.append(QStringLiteral("JPEG"));
    }
    if (!m_camera2Raw16Sizes.isEmpty()) {
        features.append(QStringLiteral("RAW16"));
    }
    if (!m_camera2Raw10Sizes.isEmpty()) {
        features.append(QStringLiteral("RAW10"));
    }
    if (!m_camera2PreviewSizes.isEmpty()) {
        features.append(QStringLiteral("warm preview"));
    }
    if (features.isEmpty()) {
        features.append(QStringLiteral("no direct capture outputs"));
    }
    return QStringLiteral("%1: %2").arg(level, features.join(QStringLiteral(", ")));
}

QString DeclarativeCameraExtensions::exportCamera2CompatibilityReport(
        const QString &cameraId)
{
    loadCamera2Capabilities(cameraId);

    QJsonObject report;
    report.insert(QStringLiteral("camera_id"),
                  cameraId.isEmpty() ? QStringLiteral("0") : cameraId);
    report.insert(QStringLiteral("compatibility_level"),
                  camera2CompatibilityLevel(cameraId));
    report.insert(QStringLiteral("compatibility_summary"),
                  camera2CompatibilitySummary(cameraId));
    report.insert(QStringLiteral("procam_probe_path"), rawCaptureProbePath());
    report.insert(QStringLiteral("helper_installed"), rawImageCaptureAvailable());
    report.insert(QStringLiteral("probe_valid"), m_camera2CapabilitiesValid);
    report.insert(QStringLiteral("selected_camera"), m_camera2SelectedCamera);
    report.insert(QStringLiteral("device_profile_id"), m_camera2DeviceProfileId);
    report.insert(QStringLiteral("device_profile_source"), m_camera2DeviceProfileSource);
    report.insert(QStringLiteral("device_profile"), m_camera2DeviceProfile);
    report.insert(QStringLiteral("hal_max_shutter_ns"),
                  QString::number(m_camera2HalMaxShutterNs));
    report.insert(QStringLiteral("effective_max_shutter_ns"),
                  QString::number(m_camera2MaxShutterNs));
    report.insert(QStringLiteral("probe_stderr"),
                  QString::fromLocal8Bit(m_camera2ProbeErrors));
    report.insert(QStringLiteral("probe_json"), QJsonDocument::fromJson(
                      m_camera2ProbeJson).isObject()
                  ? QJsonDocument::fromJson(m_camera2ProbeJson).object()
                  : QJsonObject());

    QDir directory(procamPicturesPath());
    directory.mkpath(QStringLiteral("."));
    const QString path = directory.filePath(QStringLiteral(
        "ProCam_camera2_compatibility_%1.json").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    file.close();
    return path;
}

bool DeclarativeCameraExtensions::finalizeImageMetadata(const QString &path)
{
    const QUrl url(path);
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : path;
    QJsonObject metadata = m_captureMetadata;
    metadata.insert(QStringLiteral("preserve_original_orientation"), true);
    const QByteArray bytes = QJsonDocument(metadata).toJson();
    QFile sidecar(jsonSidecarPath(localPath));
    QString error;
    if (!sidecar.open(QIODevice::WriteOnly) || sidecar.write(bytes) != bytes.size()) {
        emit rawImageCaptureFailed(QStringLiteral("Could not save capture metadata"));
        return false;
    }
    sidecar.close();
    if (!ExifUtils::writeJpegExifFromJsonFile(localPath, sidecar.fileName(), &error)) {
        emit rawImageCaptureFailed(error);
        return false;
    }
    return true;
}

void DeclarativeCameraExtensions::setCaptureMetadata(const QVariantMap &metadata)
{
    m_captureMetadata = ExifUtils::captureContext(metadata);
}

void DeclarativeCameraExtensions::setNextCaptureBracketMetadata(
        int index, int count, qreal ev, const QString &baseShutterNs, bool burst)
{
    if (index == 0 && count >= 2 && !m_rawCaptureProcess
            && !m_rawRenderWatcher && !m_bracketCombineWatcher) m_rawBracketCancelled = false;
    m_bracketIndex = index;
    m_bracketCount = count;
    m_bracketBurst = burst && count >= 2;
    m_bracketEv = ev;
    m_bracketBaseShutterNs = baseShutterNs;
    if (index >= 0 && count > 0) {
        m_captureMetadata.insert(QStringLiteral("bracket_index"), index);
        m_captureMetadata.insert(QStringLiteral("bracket_count"), count);
        m_captureMetadata.insert(QStringLiteral("bracket_ev"), ev);
        m_captureMetadata.insert(QStringLiteral("bracket_base_shutter_ns"), baseShutterNs);
        if (m_bracketBurst) m_captureMetadata.insert(QStringLiteral("burst"), true);
    }

}

void DeclarativeCameraExtensions::disableNotifications(QQuickItem *item, bool disable)
{
    if (QWindow *window = item ? item->window() : 0) {
        QGuiApplication::platformNativeInterface()->setWindowProperty(
                    window->handle(), QLatin1String("NOTIFICATION_PREVIEWS_DISABLED"),
                    QVariant(disable ? 3 : 0));
    }
}

bool DeclarativeCameraExtensions::captureRawImage(const QString &targetPath, const QString &cameraId,
                                                  const QString &rawSize, int timeoutSeconds,
                                                  const QString &focusMode, const QString &focusDistance,
                                     int focusTimeoutSeconds, const QString &focusFailure,
                                     const QString &exposure, int jpegQuality,
                                     int rotationDegrees, const QString &rawSaveFormat,
                                     const QString &rawRenderEngine,
                                     const QString &rawFormat,
                                     const QString &sceneMode, int colorTemperature,
                                     int colorTint, bool progressiveJpeg,
                                     int sensorSensitivity,
                                     const QString &exposureTime,
                                     int aperture,
                                     int noiseReduction,
                                     qreal zoom,
                                     qreal focusX,
                                     qreal focusY)
{
    if (m_rawCaptureProcess || m_rawRenderWatcher || m_bracketCombineWatcher) {
        emit rawImageCaptureFailed(QStringLiteral("RAW image capture is already running"));
        return false;
    }

    QString localTargetPath = targetPath;
    const QUrl targetUrl(targetPath);
    if (targetUrl.isLocalFile()) {
        localTargetPath = targetUrl.toLocalFile();
    }
    if (localTargetPath.isEmpty()) {
        emit rawImageCaptureFailed(QStringLiteral("RAW image capture target is empty"));
        return false;
    }
    if (!rawImageCaptureAvailable()) {
        emit rawImageCaptureFailed(QStringLiteral("RAW image capture helper is not installed"));
        return false;
    }
    loadCamera2Capabilities(cameraId);

    m_rawCaptureDirectory.reset(new QTemporaryDir(
                QDir::tempPath() + QLatin1String("/procam-raw-XXXXXX")));
    if (!m_rawCaptureDirectory->isValid()) {
        clearRawImageCapture();
        emit rawImageCaptureFailed(QStringLiteral("Could not create temporary RAW capture directory"));
        return false;
    }

    m_rawCaptureTargetPath = localTargetPath;
    m_rawCapturePrefix = m_rawCaptureDirectory->path() + QLatin1String("/capture");
    const QFileInfo targetInfo(localTargetPath);
    m_rawCaptureArchivePrefix = targetInfo.absolutePath()
            + QLatin1Char('/') + targetInfo.completeBaseName();
    m_rawCaptureErrors.clear();
    m_rawCaptureStandardOutput.clear();
    m_rawCaptureExposure = exposure.isEmpty() ? QStringLiteral("1.0") : exposure;
    m_rawCaptureSaveFormat = rawSaveFormat.isEmpty()
            ? QStringLiteral("none") : rawSaveFormat;
    m_rawRenderEngine = rawRenderEngineSupported(rawRenderEngine)
            ? rawRenderEngine : QStringLiteral("internal");
    const QString effectiveRawFormat = rawCaptureFormatSupported(rawFormat)
            ? rawFormat : QStringLiteral("raw16");
    if (effectiveRawFormat == QLatin1String("raw10") &&
            m_rawRenderEngine == QLatin1String("fastjpeg")) {
        m_rawRenderEngine = QStringLiteral("internal");
    }
    m_rawCaptureProgressiveJpeg = progressiveJpeg;
    m_rawCaptureJpegQuality = qBound(1, jpegQuality, 100);
    m_rawCaptureRotationDegrees = ((rotationDegrees % 360) + 360) % 360;
    m_rawCaptureColorTemperature = qBound(0, colorTemperature, 50000);
    m_rawCaptureColorTint = qBound(-1000, colorTint, 1000);
    m_rawCaptureTimer.restart();
    qDebug() << "capture-timing app raw request"
             << "t" << m_rawCaptureTimer.elapsed()
             << "size" << rawSize
             << "focus" << focusMode
             << "save" << rawSaveFormat
             << "format" << effectiveRawFormat;

    QString effectiveRawSize = rawSize.isEmpty()
            ? camera2PreferredCaptureSize(cameraId, QStringLiteral("raw"))
            : rawSize;
    const QStringList effectiveRawSizes = effectiveRawFormat == QLatin1String("raw10")
            ? m_camera2Raw10Sizes : m_camera2Raw16Sizes;
    if (!effectiveRawSize.isEmpty() &&
            !effectiveRawSizes.isEmpty() &&
            !effectiveRawSizes.contains(effectiveRawSize)) {
        effectiveRawSize = effectiveRawSizes.first();
    }
    if (effectiveRawSize.isEmpty()) {
        clearRawImageCapture();
        emit rawImageCaptureFailed(QStringLiteral("No Camera2 RAW capture size is available"));
        return false;
    }
    QStringList arguments;
    arguments << QStringLiteral("--capture")
              << QStringLiteral("--camera") << (cameraId.isEmpty() ? QStringLiteral("0") : cameraId)
              << QStringLiteral("--size") << effectiveRawSize
              << QStringLiteral("--timeout") << QString::number(qBound(1, timeoutSeconds, 3600))
              << QStringLiteral("--focus") << (focusMode.isEmpty() ? QStringLiteral("auto") : focusMode)
              << QStringLiteral("--focus-distance") << (focusDistance.isEmpty() ? QStringLiteral("0") : focusDistance)
              << QStringLiteral("--focus-timeout") << QString::number(qBound(1, focusTimeoutSeconds, 3600))
              << QStringLiteral("--focus-failure") << (focusFailure.isEmpty() ? QStringLiteral("capture") : focusFailure);

    const QString effectiveSceneMode = sceneMode.isEmpty()
            ? QStringLiteral("manual") : sceneMode;
    if (effectiveSceneMode != QLatin1String("none") &&
            effectiveSceneMode != QLatin1String("manual")) {
        arguments << QStringLiteral("--scene") << effectiveSceneMode;
    }
    if (colorTemperature > 0 && m_bracketCount != 2) {
        arguments << QStringLiteral("--color-temperature") << QString::number(qBound(0, colorTemperature, 50000));
    }
    if (colorTint != 0 && m_bracketCount != 2) {
        arguments << QStringLiteral("--color-tint") << QString::number(qBound(-1000, colorTint, 1000));
    }
    arguments << QStringLiteral("--ev-steps") << QString::number(m_captureCompensation);
    if (sensorSensitivity > 0) {
        arguments << QStringLiteral("--iso")
                  << QString::number(clampCamera2Iso(m_camera2SelectedCamera,
                                                     sensorSensitivity));
    }
    if (!exposureTime.isEmpty() && exposureTime != QLatin1String("0")) {
        arguments << QStringLiteral("--shutter-ns") << exposureTime;
    }
    if (aperture > 0) {
        arguments << QStringLiteral("--aperture")
                  << QString::number(qBound(0, aperture, 255));
    }
    if (noiseReduction > 0) {
        arguments << QStringLiteral("--noise-reduction")
                  << QString::number(noiseReduction);
    }
    arguments << QStringLiteral("--zoom")
              << QString::number(clampCamera2Zoom(m_camera2SelectedCamera, zoom), 'f', 4);
    if (focusX >= 0.0 && focusX <= 1.0 && focusY >= 0.0 && focusY <= 1.0) {
        arguments << QStringLiteral("--focus-x")
                  << QString::number(focusX, 'f', 4)
                  << QStringLiteral("--focus-y")
                  << QString::number(focusY, 'f', 4);
    }
    arguments << QStringLiteral("--raw-format") << effectiveRawFormat;

    arguments << QStringLiteral("--output") << m_rawCapturePrefix
              << QStringLiteral("--force");

    m_rawCaptureStage = RawCaptureCapturing;
    return startRawImageProcess(rawCaptureProbePath(), arguments,
                                QStringLiteral("Camera2 probe"));
}

bool DeclarativeCameraExtensions::captureJpegImage(const QString &targetPath, const QString &cameraId,
                                                   const QString &jpegSize, int timeoutSeconds,
                                                   int jpegQuality, int rotationDegrees,
                                                   const QString &exposure,
                                                   const QString &sceneMode,
                                                   int sensorSensitivity,
                                                   const QString &exposureTime,
                                                   int aperture,
                                                   int noiseReduction,
                                                   qreal zoom)
{
    if (m_rawCaptureProcess || m_rawRenderWatcher || m_bracketCombineWatcher) {
        emit rawImageCaptureFailed(QStringLiteral("Camera2 image capture is already running"));
        return false;
    }

    QString localTargetPath = targetPath;
    const QUrl targetUrl(targetPath);
    if (targetUrl.isLocalFile()) {
        localTargetPath = targetUrl.toLocalFile();
    }
    if (localTargetPath.isEmpty()) {
        emit rawImageCaptureFailed(QStringLiteral("Camera2 image capture target is empty"));
        return false;
    }
    if (!executableExists(rawCaptureProbePath())) {
        emit rawImageCaptureFailed(QStringLiteral("Camera2 capture helper is not installed"));
        return false;
    }
    loadCamera2Capabilities(cameraId);

    const QFileInfo targetInfo(localTargetPath);
    QDir().mkpath(targetInfo.absolutePath());
    QFile::remove(localTargetPath);

    m_rawCaptureTargetPath = localTargetPath;
    m_rawCaptureErrors.clear();
    m_rawCaptureStandardOutput.clear();
    m_rawCaptureStage = RawCaptureJpegCapturing;
    m_rawCaptureExposure = exposure.isEmpty() ? QStringLiteral("1.0") : exposure;
    m_rawCaptureJpegQuality = qBound(1, jpegQuality, 100);
    m_rawCaptureRotationDegrees = ((rotationDegrees % 360) + 360) % 360;
    m_rawCaptureColorTemperature = 0;
    m_rawCaptureColorTint = 0;
    m_rawCaptureTimer.restart();
    qDebug() << "capture-timing app jpeg request"
             << "t" << m_rawCaptureTimer.elapsed()
             << "size" << jpegSize;
    QString effectiveJpegSize = jpegSize.isEmpty()
            ? camera2PreferredCaptureSize(cameraId, QStringLiteral("jpeg"))
            : jpegSize;
    if (!effectiveJpegSize.isEmpty() &&
            !m_camera2JpegSizes.isEmpty() &&
            !m_camera2JpegSizes.contains(effectiveJpegSize)) {
        effectiveJpegSize = m_camera2JpegSizes.first();
    }
    if (effectiveJpegSize.isEmpty()) {
        clearRawImageCapture();
        emit rawImageCaptureFailed(QStringLiteral("No Camera2 JPEG capture size is available"));
        return false;
    }
    QStringList arguments;
    arguments << QStringLiteral("--capture-jpeg")
              << QStringLiteral("--camera") << (cameraId.isEmpty() ? QStringLiteral("0") : cameraId)
              << QStringLiteral("--size") << effectiveJpegSize
              << QStringLiteral("--timeout") << QString::number(qBound(1, timeoutSeconds, 3600))
              << QStringLiteral("--quality") << QString::number(m_rawCaptureJpegQuality)
              << QStringLiteral("--orientation") << QStringLiteral("0")
              << QStringLiteral("--zoom")
              << QString::number(clampCamera2Zoom(m_camera2SelectedCamera, zoom), 'f', 4)
              << QStringLiteral("--output") << localTargetPath
              << QStringLiteral("--force");
    const QString effectiveSceneMode = sceneMode.isEmpty()
            ? QStringLiteral("manual") : sceneMode;
    if (effectiveSceneMode != QLatin1String("none") &&
            effectiveSceneMode != QLatin1String("manual")) {
        arguments << QStringLiteral("--scene") << effectiveSceneMode;
    }
    arguments << QStringLiteral("--ev-steps") << QString::number(m_captureCompensation);
    if (sensorSensitivity > 0) {
        arguments << QStringLiteral("--iso")
                  << QString::number(clampCamera2Iso(m_camera2SelectedCamera,
                                                     sensorSensitivity));
    }
    if (!exposureTime.isEmpty() && exposureTime != QLatin1String("0")) {
        arguments << QStringLiteral("--shutter-ns") << exposureTime;
    }
    if (aperture > 0) {
        arguments << QStringLiteral("--aperture")
                  << QString::number(qBound(0, aperture, 255));
    }
    if (noiseReduction > 0) {
        arguments << QStringLiteral("--noise-reduction")
                  << QString::number(noiseReduction);
    }

    return startRawImageProcess(rawCaptureProbePath(), arguments,
                                QStringLiteral("Camera2 JPEG probe"));
}

bool DeclarativeCameraExtensions::processRawImage(
        const QString &targetPath, const QString &rawPath,
        const QString &metadataPath, const QString &exposure, int jpegQuality,
        int rotationDegrees, const QString &rawSaveFormat,
        const QString &rawRenderEngine,
        int colorTemperature, int colorTint, bool progressiveJpeg)
{
    if (m_rawCaptureProcess || m_rawRenderWatcher || m_bracketCombineWatcher) {
        emit rawImageCaptureFailed(QStringLiteral("Camera2 image capture is already running"));
        return false;
    }

    QString localTargetPath = targetPath;
    const QUrl targetUrl(targetPath);
    if (targetUrl.isLocalFile()) {
        localTargetPath = targetUrl.toLocalFile();
    }
    if (localTargetPath.isEmpty() || rawPath.isEmpty() ||
            metadataPath.isEmpty()) {
        emit rawImageCaptureFailed(QStringLiteral("RAW image capture target is empty"));
        return false;
    }
    if (!QFileInfo(rawPath).isFile() || !QFileInfo(metadataPath).isFile()) {
        emit rawImageCaptureFailed(QStringLiteral("RAW image capture did not produce usable output"));
        return false;
    }

    const QFileInfo rawInfo(rawPath);
    const QFileInfo targetInfo(localTargetPath);
    m_rawCaptureTargetPath = localTargetPath;
    m_rawCapturePrefix = rawInfo.absolutePath() + QLatin1Char('/')
            + rawInfo.completeBaseName();
    m_rawCaptureArchivePrefix = targetInfo.absolutePath()
            + QLatin1Char('/') + targetInfo.completeBaseName();
    m_rawCaptureErrors.clear();
    m_rawCaptureStandardOutput.clear();
    m_rawCaptureExposure = exposure.isEmpty() ? QStringLiteral("1.0") : exposure;
    m_rawCaptureSaveFormat = rawSaveFormat.isEmpty()
            ? QStringLiteral("none") : rawSaveFormat;
    // A burst keeps the first frame's RAW/DNG, not one file per frame,
    // unless every frame is asked for: then each one as RAW + JSON (frame 0
    // also as DNG if chosen).
    if (m_bracketBurst && m_keepBurstFrames) {
        const bool dng = m_rawCaptureSaveFormat == QLatin1String("dng")
                || m_rawCaptureSaveFormat == QLatin1String("both");
        m_rawCaptureSaveFormat = dng && m_bracketIndex == 0 ? QStringLiteral("both") : QStringLiteral("raw16");
    } else if (m_bracketBurst && m_bracketIndex > 0) {
        m_rawCaptureSaveFormat = QStringLiteral("none");
    }
    m_rawRenderEngine = rawRenderEngineSupported(rawRenderEngine)
            ? rawRenderEngine : QStringLiteral("internal");
    m_rawCaptureProgressiveJpeg = progressiveJpeg;
    m_rawCaptureJpegQuality = qBound(1, jpegQuality, 100);
    m_rawCaptureRotationDegrees = ((rotationDegrees % 360) + 360) % 360;
    m_rawCaptureColorTemperature = qBound(0, colorTemperature, 50000);
    m_rawCaptureColorTint = qBound(-1000, colorTint, 1000);
    m_rawCaptureTimer.restart();

    QString metadataError;
    if (!writeMetadataWithRawPath(metadataPath,
                                  m_rawCapturePrefix + QLatin1String(".json"),
                                  rawPath,
                                  &metadataError)) {
        emit rawImageCaptureFailed(metadataError);
        return false;
    }

    if (!preserveRawCaptureFiles()) {
        const QString error = m_rawCaptureErrors;
        clearRawImageCapture();
        emit rawImageCaptureFailed(error);
        return false;
    }
    if (m_bracketCount >= 2) return stageRawBracketFrame();
    // Render off the GUI thread; finishRawImageRender() completes the capture.
    return startRawImageRender();
}

bool DeclarativeCameraExtensions::startRawImageRender()
{
    const RawPipeline::RenderSettings settings = rawPipelineSettings();
    m_rawRenderStart = m_rawCaptureTimer.elapsed();
    m_rawRenderWatcher.reset(new QFutureWatcher<bool>);
    connect(m_rawRenderWatcher.data(), &QFutureWatcher<bool>::finished,
            this, &DeclarativeCameraExtensions::finishRawImageRender);
    m_rawRenderWatcher->setFuture(QtConcurrent::run([this, settings]() {
        if (m_rawRenderEngine == QLatin1String("fastjpeg"))
            return renderRawImageWithFastJpegConverter();
        if (m_rawRenderEngine == QLatin1String("enhanced"))
            return renderRawImageEnhanced(settings);
        return renderRawImage();
    }));
    return true;
}

void DeclarativeCameraExtensions::discardRawBracket()
{
    m_rawBracketCancelled = true;
    // An active worker owns these files until its completion callback.
    if (!m_bracketCombineWatcher) m_rawBracketDirectory.reset();
}

bool DeclarativeCameraExtensions::combineRawBracket(
        const QString &targetPath, const QVariantList &sourcePaths,
        int jpegQuality)
{
    if (m_rawCaptureProcess || m_rawRenderWatcher || m_bracketCombineWatcher) {
        emit rawImageCaptureFailed(QStringLiteral("Camera2 image capture is already running"));
        return false;
    }

    QStringList paths;
    for (const QVariant &path : sourcePaths) {
        const QString stringPath = path.toString();
        if (!stringPath.isEmpty()) {
            paths.append(stringPath);
        }
    }
    if (targetPath.isEmpty() || paths.size() != 2) {
        emit rawImageCaptureFailed(QStringLiteral("Bracket combine target is incomplete"));
        return false;
    }

    m_bracketCombineTargetPath = targetPath;
    m_bracketCombineSourcePaths = paths;
    m_bracketCombineJpegQuality = qBound(1, jpegQuality, 100);
    m_rawCaptureErrors.clear();
    m_bracketCombineWatcher.reset(new QFutureWatcher<bool>);
    connect(m_bracketCombineWatcher.data(), &QFutureWatcher<bool>::finished,
            this, &DeclarativeCameraExtensions::finishBracketCombine);
    m_bracketCombineWatcher->setFuture(QtConcurrent::run([this]() {
        QString error;
        if (!RawBracket::render(m_bracketCombineTargetPath,
                                 m_bracketCombineSourcePaths,
                                 m_bracketCombineJpegQuality,
                                 &error)) {
            m_rawCaptureErrors = error;
            return false;
        }
        return true;
    }));
    return true;
}

bool DeclarativeCameraExtensions::combineRawBurst(
        const QString &targetPath, const QVariantList &sourcePaths,
        int jpegQuality, qreal targetGain)
{
    // Queued: the capture is complete once its frames are staged, so the
    // next photo can be taken while earlier bursts are still merging.
    QStringList paths;
    for (const QVariant &path : sourcePaths) {
        if (!path.toString().isEmpty()) paths.append(path.toString());
    }
    if (targetPath.isEmpty() || paths.size() < 2) {
        emit rawImageCaptureFailed(QStringLiteral("Burst merge target is incomplete"));
        return false;
    }
    BurstJob job;
    job.target = targetPath;
    job.sources = paths;
    job.quality = qBound(1, jpegQuality, 100);
    job.settings = rawPipelineSettings();
    job.settings.targetGain = std::isfinite(targetGain) && targetGain > 0 ? targetGain : 1.0;
    job.directory = QSharedPointer<QTemporaryDir>(m_rawBracketDirectory.take());
    m_burstJobs.append(job);
    emit pendingRendersChanged();
    startNextBurstRender();
    return true;
}

int DeclarativeCameraExtensions::pendingRenders() const
{
    return m_burstJobs.size();
}

void DeclarativeCameraExtensions::startNextBurstRender()
{
    if (m_burstWatcher || m_burstJobs.isEmpty()) return;
    const BurstJob job = m_burstJobs.first();
    m_burstWatcher.reset(new QFutureWatcher<QString>);
    connect(m_burstWatcher.data(), &QFutureWatcher<QString>::finished,
            this, &DeclarativeCameraExtensions::finishBurstRender);
    m_burstPool.setMaxThreadCount(1);
    m_burstWatcher->setFuture(QtConcurrent::run(&m_burstPool, [job]() {
        // Background merges must not stall the viewfinder or the next
        // capture: lower priority for this thread and the workers it starts
        // (Linux threads inherit their creator's nice value).
        setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 5);
        QString error;
        if (!RawPipeline::renderBurst(job.target, job.sources, job.quality, job.settings, &error))
            return error.isEmpty() ? QStringLiteral("Burst merge failed") : error;
        if (!ExifUtils::writeJpegExifFromJsonFile(job.target, jsonSidecarPath(job.target), &error))
            return error.isEmpty() ? QStringLiteral("Could not write photo metadata") : error;
        return QString();
    }));
}

void DeclarativeCameraExtensions::finishBurstRender()
{
    QFutureWatcher<QString> *watcher = m_burstWatcher.take();
    const QString error = watcher ? watcher->result() : QStringLiteral("Burst merge failed");
    if (watcher) watcher->deleteLater();
    if (m_burstJobs.isEmpty()) return;
    const BurstJob job = m_burstJobs.takeFirst(); // releasing it removes the staged frames
    emit pendingRendersChanged();
    if (error.isEmpty()) {
        emit burstRendered(job.target);
    } else {
        QFile::remove(job.target);
        QFile::remove(jsonSidecarPath(job.target));
        emit burstRenderFailed(error);
    }
    startNextBurstRender();
}

void DeclarativeCameraExtensions::finishRawImageCapture(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_rawCaptureProcess ||
            (!m_rawCaptureDirectory && m_rawCaptureStage != RawCaptureJpegCapturing)) {
        clearRawImageCapture();
        emit rawImageCaptureFailed(QStringLiteral("Camera2 image capture finished without capture state"));
        return;
    }

    const QString rawErrorOutput = QString::fromLocal8Bit(
                m_rawCaptureProcess->readAllStandardError()).trimmed();
    const QString errorOutput = filterCamera2HelperErrors(rawErrorOutput);
    m_rawCaptureStandardOutput = m_rawCaptureProcess->readAllStandardOutput();
    if (!rawErrorOutput.isEmpty()) {
        appendRawCaptureLog(rawErrorOutput);
    }
    if (!m_rawCaptureStandardOutput.trimmed().isEmpty()) {
        appendRawCaptureLog(QString::fromLocal8Bit(
                                m_rawCaptureStandardOutput.trimmed()));
    }
    if (!errorOutput.isEmpty()) {
        if (!m_rawCaptureErrors.isEmpty()) {
            m_rawCaptureErrors.append(QLatin1Char('\n'));
        }
        m_rawCaptureErrors.append(errorOutput);
    }

    QScopedPointer<QProcess> finishedProcess(m_rawCaptureProcess.take());
    qDebug() << "capture-timing app helper finished"
             << "t" << m_rawCaptureTimer.elapsed()
             << "stage" << m_rawCaptureStage
             << "exit" << exitCode;

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        const QString fallback = m_rawCaptureErrors.isEmpty()
                ? QStringLiteral("Camera2 image capture failed")
                : m_rawCaptureErrors;
        const QString error = camera2HelperStatusMessage(
                    m_rawCaptureStandardOutput, fallback);
        clearRawImageCapture();
        emit rawImageCaptureFailed(error);
        return;
    }

    if (m_rawCaptureStage == RawCaptureJpegCapturing) {
        const QString targetPath = m_rawCaptureTargetPath;
        if (!QFileInfo(targetPath).isFile()) {
            const QString error = m_rawCaptureErrors.isEmpty()
                    ? QStringLiteral("Camera2 JPEG capture did not produce usable output")
                    : m_rawCaptureErrors;
            clearRawImageCapture();
            emit rawImageCaptureFailed(error);
            return;
        }
        if (!saveJsonSidecar(targetPath, m_rawCaptureStandardOutput)) {
            const QString error = m_rawCaptureErrors.isEmpty()
                    ? QStringLiteral("Could not save Camera2 JPEG metadata")
                    : m_rawCaptureErrors;
            clearRawImageCapture();
            emit rawImageCaptureFailed(error);
            return;
        }
        // The frame is captured: give feedback now, rotate/adjust/re-encode
        // (seconds for 50 MP) off the GUI thread.
        emit rawImageExposed();
        const QString sidecarPath = jsonSidecarPath(targetPath);
        bool exposureOk = false;
        qreal exposure = m_rawCaptureExposure.toDouble(&exposureOk);
        if (!exposureOk) {
            exposure = 1.0;
        }
        const int rotation = m_rawCaptureRotationDegrees;
        const int temperature = m_rawCaptureColorTemperature, tint = m_rawCaptureColorTint;
        const int quality = m_rawCaptureJpegQuality;
        m_rawRenderWatcher.reset(new QFutureWatcher<bool>);
        connect(m_rawRenderWatcher.data(), &QFutureWatcher<bool>::finished,
                this, &DeclarativeCameraExtensions::finishJpegRewrite);
        m_rawRenderWatcher->setFuture(QtConcurrent::run([=]() {
            if (rotation != 0 || ImageAdjustments::requested(exposure, temperature, tint)) {
                QImage image(targetPath);
                if (image.isNull()) {
                    m_rawCaptureErrors = QStringLiteral("Could not load Camera2 JPEG for adjustment");
                    return false;
                }
                if (rotation != 0) {
                    image = image.transformed(QTransform().rotate(rotation));
                }
                ImageAdjustments::apply(&image, exposure, temperature, tint);
                QImageWriter writer(targetPath, "JPG");
                writer.setQuality(quality);
                if (!writer.write(image)) {
                    m_rawCaptureErrors = QStringLiteral("Could not save adjusted Camera2 JPEG");
                    return false;
                }
            }
            QString exifError;
            if (!ExifUtils::writeJpegExifFromJsonFile(targetPath, sidecarPath, &exifError)) {
                m_rawCaptureErrors = exifError;
                return false;
            }
            return true;
        }));
        return;
    }

    if (m_rawCaptureStage == RawCaptureCapturing && m_bracketCount >= 2 && m_rawBracketCancelled) {
        clearRawImageCapture();
        return;
    }
    if (m_rawCaptureStage == RawCaptureCapturing) {
        QString metadataError;
        const QString rawPath = rawPathFromMetadata(
                    m_rawCapturePrefix + QLatin1String(".json"), &metadataError);
        if (!metadataError.isEmpty() || !QFileInfo(rawPath).isFile()
                || !QFileInfo(m_rawCapturePrefix + QLatin1String(".json")).isFile()) {
            const QString error = m_rawCaptureErrors.isEmpty()
                    ? QStringLiteral("RAW image capture did not produce usable output")
                    : m_rawCaptureErrors;
            clearRawImageCapture();
            emit rawImageCaptureFailed(error);
            return;
        }
        const qint64 preserveStart = m_rawCaptureTimer.elapsed();
        if (!preserveRawCaptureFiles()) {
            const QString error = m_rawCaptureErrors;
            clearRawImageCapture();
            emit rawImageCaptureFailed(error);
            return;
        }
        qDebug() << "capture-timing app raw preserve"
                 << "start" << preserveStart
                 << "end" << m_rawCaptureTimer.elapsed();
        if (m_bracketCount >= 2) {
            stageRawBracketFrame();
            return;
        }
        emit rawImageExposed();
        startRawImageRender();
        return;
    }

    clearRawImageCapture();
    emit rawImageCaptureFailed(QStringLiteral("RAW image capture finished in an unknown state"));
}

void DeclarativeCameraExtensions::finishJpegRewrite()
{
    QFutureWatcher<bool> *watcher = m_rawRenderWatcher.take();
    const bool ok = watcher && watcher->result();
    if (watcher) {
        watcher->deleteLater();
    }
    const QString targetPath = m_rawCaptureTargetPath;
    const QString error = m_rawCaptureErrors.isEmpty()
            ? QStringLiteral("Could not finish Camera2 JPEG") : m_rawCaptureErrors;
    qDebug() << "capture-timing app jpeg complete" << "t" << m_rawCaptureTimer.elapsed();
    clearRawImageCapture();
    if (!ok) {
        emit rawImageCaptureFailed(error);
        return;
    }
    emit rawImageCaptured(targetPath, QStringLiteral("image/jpeg"));
}

void DeclarativeCameraExtensions::finishRawImageRender()
{
    QFutureWatcher<bool> *watcher = m_rawRenderWatcher.take();
    const bool renderOk = watcher && watcher->result();
    if (watcher) {
        watcher->deleteLater();
    }
    if (!renderOk) {
        const QString error = m_rawCaptureErrors.isEmpty()
                ? QStringLiteral("RAW image conversion failed")
                : m_rawCaptureErrors;
        clearRawImageCapture();
        emit rawImageCaptureFailed(error);
        return;
    }

    qDebug() << "capture-timing app raw render"
             << "start" << m_rawRenderStart
             << "end" << m_rawCaptureTimer.elapsed();
    const QString targetPath = m_rawCaptureTargetPath;
    QString imageError;
    if (!readableImageExists(targetPath, &imageError)) {
        clearRawImageCapture();
        emit rawImageCaptureFailed(imageError);
        return;
    }
    if (!copyJsonSidecar(m_rawCapturePrefix + QLatin1String(".json"),
                         targetPath)) {
        const QString error = m_rawCaptureErrors.isEmpty()
                ? QStringLiteral("Could not save RAW metadata sidecar")
                : m_rawCaptureErrors;
        clearRawImageCapture();
        emit rawImageCaptureFailed(error);
        return;
    }
    QString exifError;
    if (!ExifUtils::writeJpegExifFromJsonFile(targetPath,
                                              jsonSidecarPath(targetPath),
                                              &exifError)) {
        clearRawImageCapture();
        emit rawImageCaptureFailed(exifError);
        return;
    }
    qDebug() << "capture-timing app raw complete"
             << "t" << m_rawCaptureTimer.elapsed();
    clearRawImageCapture();
    emit rawImageCaptured(targetPath, QStringLiteral("image/jpeg"));
}

void DeclarativeCameraExtensions::finishBracketCombine()
{
    QFutureWatcher<bool> *watcher = m_bracketCombineWatcher.take();
    const bool combineOk = watcher && watcher->result();
    if (watcher) {
        watcher->deleteLater();
    }

    const QString targetPath = m_bracketCombineTargetPath;
    m_bracketCombineTargetPath.clear();
    m_bracketCombineSourcePaths.clear();
    m_bracketCombineJpegQuality = 92;

    if (m_rawBracketCancelled) {
        m_rawBracketDirectory.reset();
        QFile::remove(targetPath);
        QFile::remove(jsonSidecarPath(targetPath));
        return;
    }
    if (!combineOk) {
        m_rawBracketDirectory.reset();
        const QString error = m_rawCaptureErrors.isEmpty()
                ? QStringLiteral("Bracket combine failed")
                : m_rawCaptureErrors;
        m_rawCaptureErrors.clear();
        emit rawImageCaptureFailed(error);
        return;
    }

    m_rawCaptureErrors.clear();
    const QString sidecarPath = jsonSidecarPath(targetPath);
    m_rawBracketDirectory.reset();
    QString exifError;
    if (!ExifUtils::writeJpegExifFromJsonFile(targetPath, sidecarPath, &exifError)) {
        emit rawImageCaptureFailed(exifError);
        return;
    }
    emit rawImageCaptured(targetPath, QStringLiteral("image/jpeg"));
}

bool DeclarativeCameraExtensions::startRawImageProcess(const QString &program,
                                                       const QStringList &arguments,
                                                       const QString &errorContext,
                                                       const QString &standardOutputPath)
{
    m_rawCaptureProcess.reset(new QProcess);
    m_rawCaptureProcess->setProgram(program);
    m_rawCaptureProcess->setArguments(arguments);
    if (!standardOutputPath.isEmpty()) {
        m_rawCaptureProcess->setStandardOutputFile(standardOutputPath);
    }
    connect(m_rawCaptureProcess.data(),
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this,
            &DeclarativeCameraExtensions::finishRawImageCapture);
    m_rawCaptureProcess->start();
    if (m_rawCaptureProcess->waitForStarted(1000)) {
        qDebug() << "capture-timing app helper started"
                 << "t" << m_rawCaptureTimer.elapsed()
                 << "program" << program;
        return true;
    }

    m_rawCaptureErrors = QStringLiteral("%1 could not start %2: %3")
            .arg(errorContext, program, m_rawCaptureProcess->errorString());
    m_rawCaptureProcess.reset();
    return false;
}

namespace {

enum PixelColor {
    PixelRed,
    PixelGreen,
    PixelBlue
};

struct RawRenderConfig {
    int width = 0;
    int height = 0;
    int rowStride = 0;
    int whiteLevel = 0;
    int bitsPerSample = 16;
    bool packed = false;
    int iso = 0;
    int blackLevel[4] = {0, 0, 0, 0};
    float gains[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float matrix[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    double noiseProfile[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    int noiseProfileCount = 0;
    float exposure = 1.0f;
    int colorTemperature = 0;
    int cfaMap = 3;
    QString cfa;
    QString rawPath;
};

float jsonRationalFloat(const QJsonValue &value, float fallback = 0.0f)
{
    const QJsonArray rational = value.toArray();
    if (rational.size() < 2) {
        return fallback;
    }
    const double denominator = rational.at(1).toDouble(1.0);
    return denominator == 0.0
            ? fallback
            : float(rational.at(0).toDouble() / denominator);
}

bool readRationalFloats(const QJsonArray &array, float *values, int count)
{
    if (array.size() < count) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        values[index] = jsonRationalFloat(array.at(index));
    }
    return true;
}

QByteArray cfaPatternBytes(const QString &cfa)
{
    if (cfa == QLatin1String("RGGB")) {
        return QByteArray::fromRawData("\000\001\001\002", 4);
    } else if (cfa == QLatin1String("GRBG")) {
        return QByteArray::fromRawData("\001\000\002\001", 4);
    } else if (cfa == QLatin1String("GBRG")) {
        return QByteArray::fromRawData("\001\002\000\001", 4);
    }
    return QByteArray::fromRawData("\002\001\001\000", 4);
}

bool writeTiffDng(const QString &metadataPath, const QString &dngPath,
                  QString *error)
{
    QFile metadataFile(metadataPath);
    if (!metadataFile.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read RAW metadata: %1").arg(metadataPath);
        return false;
    }
    const QJsonObject metadata =
            QJsonDocument::fromJson(metadataFile.readAll()).object();
    const int width = metadata.value(QStringLiteral("width")).toInt();
    const int height = metadata.value(QStringLiteral("height")).toInt();
    const int rowStride = metadata.value(QStringLiteral("row_stride")).toInt();
    const int whiteLevel = metadata.value(QStringLiteral("white_level")).toInt();
    const QString rawPath = metadata.value(QStringLiteral("raw_path")).toString();
    const QString cfa = metadata.value(QStringLiteral("cfa")).toString();
    if (metadata.value(QStringLiteral("format")).toString() == QLatin1String("RAW10")) {
        *error = QStringLiteral("DNG export supports RAW16 only");
        return false;
    }
    if (width <= 0 || height <= 0 || rowStride < width * 2 ||
            whiteLevel <= 0 || rawPath.isEmpty()) {
        *error = QStringLiteral("RAW metadata is incomplete for DNG");
        return false;
    }

    QFile raw(rawPath);
    if (!raw.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read RAW16 file: %1").arg(rawPath);
        return false;
    }

    QDir().mkpath(QFileInfo(dngPath).absolutePath());
    QFile::remove(dngPath);

    const QByteArray dngPathBytes = dngPath.toLocal8Bit();
    TIFF *tiff = TIFFOpen(dngPathBytes.constData(), "w");
    if (!tiff) {
        *error = QStringLiteral("Cannot create DNG: %1").arg(dngPath);
        return false;
    }

    // Register DNG-private tags this libtiff build doesn't know about by
    // default; see the comment on dngPrivateFields above.
    TIFFMergeFieldInfo(tiff, dngPrivateFields,
                        sizeof(dngPrivateFields) / sizeof(dngPrivateFields[0]));

    const QByteArray software = QByteArrayLiteral("ProCam Camera2");
    const QByteArray uniqueModel = QStringLiteral("Sailfish Camera2 camera %1")
            .arg(metadata.value(QStringLiteral("camera_id")).toString())
            .toUtf8();
    const QByteArray cfaPattern = cfaPatternBytes(cfa);
    const uint8_t dngVersion[4] = { 1, 4, 0, 0 };
    const uint8_t dngBackwardVersion[4] = { 1, 1, 0, 0 };
    const uint8_t cfaPlaneColor[3] = { 0, 1, 2 };
    const uint16_t cfaRepeatPatternDim[2] = { 2, 2 };
    const uint16_t blackLevelRepeatDim[2] = { 2, 2 };
    const uint32_t whiteLevelValue = uint32_t(whiteLevel);
    float blackLevel[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const QJsonArray black = metadata.value(QStringLiteral("black_level_pattern")).toArray();
    for (int index = 0; index < 4; ++index) {
        blackLevel[index] = float(index < black.size() ? black.at(index).toDouble() : 0.0);
    }
    const uint32_t activeArea[4] = { 0, 0, uint32_t(height), uint32_t(width) };
    uint32_t activeAreaFromMetadata[4];
    const QJsonArray active = metadata.value(QStringLiteral("active_array")).toArray();
    const uint32_t *effectiveActiveArea = activeArea;
    if (active.size() >= 4) {
        activeAreaFromMetadata[0] = uint32_t(active.at(1).toInt());
        activeAreaFromMetadata[1] = uint32_t(active.at(0).toInt());
        activeAreaFromMetadata[2] = uint32_t(active.at(3).toInt());
        activeAreaFromMetadata[3] = uint32_t(active.at(2).toInt());
        effectiveActiveArea = activeAreaFromMetadata;
    }
    const float defaultCropOrigin[2] = { 0.0f, 0.0f };
    const float defaultCropSize[2] = { float(width), float(height) };

    TIFFSetField(tiff, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, uint32_t(width));
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, uint32_t(height));
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(tiff, TIFFTAG_ORIENTATION, ExifUtils::orientation(metadata, true));
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tiff, 0));
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tiff, TIFFTAG_SOFTWARE, software.constData());
    // Standard DNG/EXIF Make+Model, as any camera DNG carries. Used by
    // tools/calibration/generate_lens_shading.py to name calibration files,
    // and by DngLensShading::buildOpcodeList2() (see below) to look one up.
    static const DeviceInfo deviceInfo;
    const QByteArray make = deviceInfo.manufacturer().toUtf8();
    const QByteArray model = deviceInfo.prettyName().toUtf8();
    if (!make.isEmpty()) {
        TIFFSetField(tiff, TIFFTAG_MAKE, make.constData());
    }
    if (!model.isEmpty()) {
        TIFFSetField(tiff, TIFFTAG_MODEL, model.constData());
    }
    TIFFSetField(tiff, TIFFTAG_CFAREPEATPATTERNDIM, cfaRepeatPatternDim);
    TIFFSetField(tiff, TIFFTAG_CFAPATTERN, 4, cfaPattern.constData());
    TIFFSetField(tiff, TIFFTAG_DNGVERSION, dngVersion);
    TIFFSetField(tiff, TIFFTAG_DNGBACKWARDVERSION, dngBackwardVersion);
    TIFFSetField(tiff, TIFFTAG_UNIQUECAMERAMODEL, uniqueModel.constData());
    TIFFSetField(tiff, TIFFTAG_CFAPLANECOLOR, 3, cfaPlaneColor);
    TIFFSetField(tiff, TIFFTAG_CFALAYOUT, 1);
    TIFFSetField(tiff, TIFFTAG_BLACKLEVELREPEATDIM, blackLevelRepeatDim);
    TIFFSetField(tiff, TIFFTAG_BLACKLEVEL, 4, blackLevel);
    TIFFSetField(tiff, TIFFTAG_WHITELEVEL, 1, &whiteLevelValue);
    TIFFSetField(tiff, TIFFTAG_ACTIVEAREA, effectiveActiveArea);
    TIFFSetField(tiff, TIFFTAG_DEFAULTCROPORIGIN, defaultCropOrigin);
    TIFFSetField(tiff, TIFFTAG_DEFAULTCROPSIZE, defaultCropSize);
    TIFFSetField(tiff, TIFFTAG_CALIBRATIONILLUMINANT1, 21);

    QJsonArray matrix = metadata.value(QStringLiteral("color_transform1")).toArray();
    if (matrix.size() < 9) {
        matrix = metadata.value(QStringLiteral("capture_color_transform")).toArray();
    }
    float colorMatrix[9];
    if (matrix.size() >= 9 && readRationalFloats(matrix, colorMatrix, 9)) {
        TIFFSetField(tiff, TIFFTAG_COLORMATRIX1, 9, colorMatrix);
    }
    const QJsonArray neutral = metadata.value(QStringLiteral("neutral_color_point")).toArray();
    float asShotNeutral[3];
    if (neutral.size() >= 3) {
        asShotNeutral[0] = jsonRationalFloat(neutral.at(0), 1.0f);
        asShotNeutral[1] = jsonRationalFloat(neutral.at(1), 1.0f);
        asShotNeutral[2] = jsonRationalFloat(neutral.at(2), 1.0f);
        TIFFSetField(tiff, TIFFTAG_ASSHOTNEUTRAL, 3, asShotNeutral);
    }
    const QJsonArray noiseProfileJson = metadata.value(QStringLiteral("noise_profile")).toArray();
    double noiseProfile[8];
    if (noiseProfileJson.size() >= 8) {
        for (int index = 0; index < 8; ++index) {
            noiseProfile[index] = noiseProfileJson.at(index).toDouble();
        }
        TIFFSetField(tiff, TIFFTAG_NOISEPROFILE, 8, noiseProfile);
    }

    // Per-channel vignetting/color-shading correction, if a calibration
    // exists for this device/camera/resolution (see
    // tools/calibration/generate_lens_shading.py and
    // src/calibration/README.md). A calibration in the user's device-profiles
    // directory overrides the one bundled with the app, without rebuilding or
    // repackaging ProCam. If neither has a matching file, no correction is
    // written.
    QStringList calibrationDirs;
    const QString profileDir = procamUserDeviceProfileDir();
    if (!profileDir.isEmpty()) {
        const QString userCalibrationDir =
                QDir(profileDir).filePath(QStringLiteral("lens-shading"));
        // ProCam only reads from this directory; it is created (idempotent)
        // so users can see where an override is expected.
        QDir().mkpath(userCalibrationDir);
        calibrationDirs.append(userCalibrationDir);
    }
    calibrationDirs.append(QStringLiteral(DEPLOYMENT_PATH "calibration"));
    const QString cameraId = metadata.value(QStringLiteral("camera_id")).toString();
    QByteArray opcodeList2;
    for (const QString &calibrationDir : calibrationDirs) {
        QString lensShadingWarning;
        opcodeList2 = DngLensShading::buildOpcodeList2(
                calibrationDir, QString::fromUtf8(model), cameraId, cfa, width, height,
                &lensShadingWarning);
        if (!opcodeList2.isEmpty()) {
            break;
        }
        if (!lensShadingWarning.isEmpty()) {
            // The file exists but is broken/mismatched: report it and stop,
            // rather than silently falling through to the bundled default
            // and masking what could be a mistake in the user's own override.
            qWarning() << lensShadingWarning;
            break;
        }
        // No warning and no data: no file at this path, try the next one.
    }
    if (!opcodeList2.isEmpty()) {
        TIFFSetField(tiff, TIFFTAG_OPCODELIST2, opcodeList2.size(), opcodeList2.constData());
    }

    QByteArray row(rowStride, Qt::Uninitialized);
    for (int y = 0; y < height; ++y) {
        if (raw.read(row.data(), row.size()) != row.size()) {
            TIFFClose(tiff);
            QFile::remove(dngPath);
            *error = QStringLiteral("RAW16 file ended at row %1").arg(y);
            return false;
        }
        if (TIFFWriteScanline(tiff, row.data(), uint32_t(y), 0) < 0) {
            TIFFClose(tiff);
            QFile::remove(dngPath);
            *error = QStringLiteral("Cannot write DNG row %1").arg(y);
            return false;
        }
    }

    const bool metadataOk = ExifUtils::writeDngMetadata(tiff, metadata, error);
    TIFFClose(tiff);
    return metadataOk;
}

float jsonArrayFloat(const QJsonArray &array, int index, float fallback)
{
    return index >= 0 && index < array.size() ? float(array.at(index).toDouble(fallback)) : fallback;
}

bool readRationalMatrix(const QJsonArray &array, float *matrix)
{
    if (array.size() < 9) {
        return false;
    }
    for (int index = 0; index < 9; ++index) {
        const QJsonArray rational = array.at(index).toArray();
        const double denominator = rational.size() > 1 ? rational.at(1).toDouble() : 0.0;
        if (rational.size() < 2 || denominator == 0.0) {
            return false;
        }
        matrix[index] = float(rational.at(0).toDouble() / denominator);
    }
    return true;
}

}

bool DeclarativeCameraExtensions::saveJsonSidecar(const QString &targetPath,
                                                  const QByteArray &json)
{
    if (json.trimmed().isEmpty()) {
        m_rawCaptureErrors = QStringLiteral("Camera2 capture did not return metadata");
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || document.isNull()) {
        m_rawCaptureErrors = QStringLiteral("Camera2 metadata is not valid JSON");
        return false;
    }
    QJsonObject object = ExifUtils::mergeCaptureMetadata(document.object(), m_captureMetadata);
    const QString format = object.value(QStringLiteral("format")).toString();
    const bool pixelsRotated =
            format == QLatin1String("RAW16") ||
            format == QLatin1String("RAW10") ||
            format == QLatin1String("I420") ||
            m_rawCaptureStage == RawCaptureJpegCapturing;
    const int captureOrientation = m_rawCaptureRotationDegrees;
    object.insert(QStringLiteral("capture_orientation"), captureOrientation);
    object.insert(QStringLiteral("pixels_rotated"), pixelsRotated);
    object.insert(QStringLiteral("exif_orientation"), ExifUtils::orientation(object, false));
    object.insert(QStringLiteral("render_engine"), m_rawRenderEngine);
    object.insert(QStringLiteral("render_exposure"), m_rawCaptureExposure);
    object.insert(QStringLiteral("color_temperature_requested_kelvin"), m_rawCaptureColorTemperature);
    object.insert(QStringLiteral("color_tint_requested"), m_rawCaptureColorTint);
    object.insert(QStringLiteral("jpeg_quality"), m_rawCaptureJpegQuality);
    object.insert(QStringLiteral("progressive_jpeg"), m_rawCaptureProgressiveJpeg);
    const QByteArray originalExif = ExifUtils::originalJpegExif(targetPath);
    if (!originalExif.isEmpty()) object.insert(QStringLiteral("_original_exif"), QString::fromLatin1(originalExif.toBase64()));
    if (m_bracketIndex >= 0 && m_bracketCount > 0) {
        object.insert(QStringLiteral("bracket_index"), m_bracketIndex);
        object.insert(QStringLiteral("bracket_count"), m_bracketCount);
        object.insert(QStringLiteral("bracket_ev"), m_bracketEv);
        if (!m_bracketBaseShutterNs.isEmpty()) {
            object.insert(QStringLiteral("bracket_base_shutter_ns"),
                          m_bracketBaseShutterNs);
        }
    }
    if (m_rawCaptureStage == RawCaptureJpegCapturing) {
        object.insert(QStringLiteral("jpeg_orientation"), captureOrientation);
        if (m_rawCaptureRotationDegrees == 90 ||
                m_rawCaptureRotationDegrees == 270) {
            const int width = object.value(QStringLiteral("width")).toInt();
            const int height = object.value(QStringLiteral("height")).toInt();
            if (width > 0 && height > 0) {
                object.insert(QStringLiteral("width"), height);
                object.insert(QStringLiteral("height"), width);
            }
        }
    }
    document.setObject(object);

    const QString sidecarPath = jsonSidecarPath(targetPath);
    QDir().mkpath(QFileInfo(sidecarPath).absolutePath());
    QFile::remove(sidecarPath);
    QFile file(sidecarPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_rawCaptureErrors = QStringLiteral("Cannot write metadata: %1")
                .arg(sidecarPath);
        return false;
    }
    file.write(document.toJson(QJsonDocument::Indented));
    return true;
}

bool DeclarativeCameraExtensions::copyJsonSidecar(const QString &sourcePath,
                                                  const QString &targetPath)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        m_rawCaptureErrors = QStringLiteral("Cannot read metadata: %1")
                .arg(sourcePath);
        return false;
    }
    return saveJsonSidecar(targetPath, source.readAll());
}

bool DeclarativeCameraExtensions::writeDngSidecar(const QString &metadataPath,
                                                  const QString &targetPath)
{
    QString error;
    if (!writeTiffDng(metadataPath, targetPath, &error)) {
        m_rawCaptureErrors = error;
        return false;
    }
    return true;
}

namespace {

bool loadRawRenderConfig(const QString &metadataPath, const QString &exposure,
                         RawRenderConfig *config, QString *error)
{
    QFile file(metadataPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read RAW metadata: %1").arg(metadataPath);
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject object = document.object();
    if (object.isEmpty()) {
        *error = QStringLiteral("RAW metadata is not valid JSON");
        return false;
    }

    config->width = object.value(QStringLiteral("width")).toInt();
    config->height = object.value(QStringLiteral("height")).toInt();
    config->rowStride = object.value(QStringLiteral("row_stride")).toInt();
    config->whiteLevel = object.value(QStringLiteral("white_level")).toInt();
    config->bitsPerSample = object.value(QStringLiteral("bits_per_sample")).toInt(16);
    config->packed = object.value(QStringLiteral("packed")).toBool(false);
    config->iso = object.value(QStringLiteral("iso")).toInt();
    config->cfa = object.value(QStringLiteral("cfa")).toString();
    config->rawPath = object.value(QStringLiteral("raw_path")).toString();
    config->colorTemperature = object.value(QStringLiteral("color_temperature_requested_kelvin")).toInt();

    bool exposureOk = false;
    config->exposure = exposure.toFloat(&exposureOk);
    if (!exposureOk || config->exposure <= 0.0f || config->exposure > 32.0f) {
        *error = QStringLiteral("RAW exposure must be greater than 0 and at most 32");
        return false;
    }

    const QJsonArray black = object.value(QStringLiteral("black_level_pattern")).toArray();
    const QJsonArray gains = object.value(QStringLiteral("color_correction_gains")).toArray();
    const QJsonArray noiseProfile = object.value(QStringLiteral("noise_profile")).toArray();
    for (int index = 0; index < 4; ++index) {
        config->blackLevel[index] = int(jsonArrayFloat(black, index, 0.0f));
        config->gains[index] = jsonArrayFloat(gains, index, 1.0f);
    }
    config->noiseProfileCount = qMin(noiseProfile.size(), 8);
    for (int index = 0; index < config->noiseProfileCount; ++index) {
        config->noiseProfile[index] = noiseProfile.at(index).toDouble();
    }
    readRationalMatrix(object.value(QStringLiteral("capture_color_transform")).toArray(),
                       config->matrix);

    config->cfaMap = config->cfa == QLatin1String("RGGB") ? 0 :
                     config->cfa == QLatin1String("GRBG") ? 1 :
                     config->cfa == QLatin1String("GBRG") ? 2 : 3;
    if (config->width <= 1 || config->height <= 1 ||
            ((config->bitsPerSample == 16 && config->rowStride < config->width * 2) ||
             (config->bitsPerSample == 10 && config->rowStride < ((config->width + 3) / 4) * 5)) ||
            config->whiteLevel <= 0 || config->rawPath.isEmpty() ||
            (config->bitsPerSample != 16 && config->bitsPerSample != 10) ||
            (config->cfa != QLatin1String("RGGB") &&
             config->cfa != QLatin1String("GRBG") &&
             config->cfa != QLatin1String("GBRG") &&
             config->cfa != QLatin1String("BGGR"))) {
        *error = QStringLiteral("RAW metadata is incomplete or unsupported");
        return false;
    }

    return true;
}

PixelColor pixelColorAt(const RawRenderConfig &config, int x, int y)
{
    static const PixelColor maps[4][4] = {
        { PixelRed, PixelGreen, PixelGreen, PixelBlue },
        { PixelGreen, PixelRed, PixelBlue, PixelGreen },
        { PixelGreen, PixelBlue, PixelRed, PixelGreen },
        { PixelBlue, PixelGreen, PixelGreen, PixelRed },
    };
    return maps[config.cfaMap][(y & 1) * 2 + (x & 1)];
}

float correctedSample(const RawRenderConfig &config, const QVector<quint16> &pixels,
                      int x, int y)
{
    const int patternIndex = (y & 1) * 2 + (x & 1);
    const int black = config.blackLevel[patternIndex];
    const int value = pixels.at(y * config.width + x);
    const float normalized = value > black
            ? float(value - black) / float(config.whiteLevel - black)
            : 0.0f;
    const PixelColor color = pixelColorAt(config, x, y);
    const int gainIndex = color == PixelRed ? 0 : color == PixelBlue ? 3 : ((y & 1) ? 2 : 1);
    return normalized * config.gains[gainIndex];
}

float demosaicChannel(const RawRenderConfig &config, const QVector<quint16> &pixels,
                      int x, int y, PixelColor wanted)
{
    if (pixelColorAt(config, x, y) == wanted) {
        return correctedSample(config, pixels, x, y);
    }
    float sum = 0.0f;
    int count = 0;
    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
        const int sampleY = y + offsetY;
        if (sampleY < 0 || sampleY >= config.height) {
            continue;
        }
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            const int sampleX = x + offsetX;
            if (sampleX < 0 || sampleX >= config.width ||
                    pixelColorAt(config, sampleX, sampleY) != wanted) {
                continue;
            }
            sum += correctedSample(config, pixels, sampleX, sampleY);
            ++count;
        }
    }
    return count ? sum / float(count) : 0.0f;
}

uchar srgbByte(float linear)
{
    linear = qBound(0.0f, linear, 1.0f);
    const float encoded = linear <= 0.0031308f
            ? 12.92f * linear
            : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return uchar(qBound(0, int(encoded * 255.0f + 0.5f), 255));
}

bool readRawPixels(const RawRenderConfig &config, QVector<quint16> *pixels,
                   QString *error)
{
    QFile file(config.rawPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read RAW file: %1").arg(config.rawPath);
        return false;
    }
    pixels->resize(config.width * config.height);
    QByteArray row;
    row.resize(config.rowStride);
    for (int y = 0; y < config.height; ++y) {
        if (file.read(row.data(), row.size()) != row.size()) {
            *error = QStringLiteral("RAW file ended at row %1").arg(y);
            return false;
        }
        const uchar *bytes = reinterpret_cast<const uchar *>(row.constData());
        if (config.bitsPerSample == 10) {
            for (int x = 0; x < config.width; ++x) {
                const int group = x / 4;
                const int offset = x % 4;
                const uchar *packed = bytes + group * 5;
                const quint16 high = packed[offset];
                const quint16 low = quint16((packed[4] >> (offset * 2)) & 0x03);
                (*pixels)[y * config.width + x] = quint16((high << 2) | low);
            }
        } else {
            for (int x = 0; x < config.width; ++x) {
                (*pixels)[y * config.width + x] =
                        quint16(bytes[x * 2]) | quint16(bytes[x * 2 + 1] << 8);
            }
        }
    }
    return true;
}

bool renderRawToImage(const RawRenderConfig &config, QImage *image,
                      QString *error)
{
    QVector<quint16> pixels;
    if (!readRawPixels(config, &pixels, error)) {
        return false;
    }
    *image = QImage(config.width, config.height, QImage::Format_RGB888);
    if (image->isNull()) {
        *error = QStringLiteral("Not enough memory for RAW image");
        return false;
    }
    for (int y = 0; y < config.height; ++y) {
        uchar *row = image->scanLine(y);
        for (int x = 0; x < config.width; ++x) {
            const float sensor[3] = {
                demosaicChannel(config, pixels, x, y, PixelRed),
                demosaicChannel(config, pixels, x, y, PixelGreen),
                demosaicChannel(config, pixels, x, y, PixelBlue),
            };
            for (int channel = 0; channel < 3; ++channel) {
                const float output = config.exposure *
                        (config.matrix[channel * 3] * sensor[0] +
                         config.matrix[channel * 3 + 1] * sensor[1] +
                         config.matrix[channel * 3 + 2] * sensor[2]);
                row[x * 3 + channel] = srgbByte(output);
            }
        }
    }
    return true;
}

void copyFileTimes(const QString &sourcePath, const QString &targetPath)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists()) {
        return;
    }

    struct utimbuf times;
    const uint modified = sourceInfo.lastModified().toTime_t();
    times.actime = modified;
    times.modtime = modified;
    utime(QFile::encodeName(targetPath).constData(), &times);
}

}

bool DeclarativeCameraExtensions::renderRawImage()
{
    QString error;
    RawRenderConfig config;
    if (!loadRawRenderConfig(m_rawCapturePrefix + QLatin1String(".json"),
                             m_rawCaptureExposure, &config, &error)) {
        m_rawCaptureErrors = error;
        return false;
    }

    QImage image;
    if (!renderRawToImage(config, &image, &error)) {
        m_rawCaptureErrors = error;
        return false;
    }
    if (m_rawCaptureRotationDegrees != 0) {
        image = image.transformed(QTransform().rotate(m_rawCaptureRotationDegrees));
    }
    ImageAdjustments::apply(&image, 1.0, m_rawCaptureColorTemperature,
                            m_rawCaptureColorTint);

    const QFileInfo targetInfo(m_rawCaptureTargetPath);
    QDir().mkpath(targetInfo.absolutePath());
    QFile::remove(m_rawCaptureTargetPath);
    QImageWriter writer(m_rawCaptureTargetPath, "JPG");
    writer.setQuality(m_rawCaptureJpegQuality);
    if (m_rawCaptureProgressiveJpeg) {
        writer.setProgressiveScanWrite(true);
    }
    if (!writer.write(image)) {
        m_rawCaptureErrors = writer.errorString().isEmpty()
                ? QStringLiteral("Could not save converted RAW image to final path")
                : QStringLiteral("Could not save converted RAW image: %1").arg(writer.errorString());
        return false;
    }
    copyFileTimes(config.rawPath, m_rawCaptureTargetPath);
    return true;
}

bool DeclarativeCameraExtensions::renderRawImageEnhanced(const RawPipeline::RenderSettings &base)
{
    // Worker thread: everything else it needs is in the sidecar.
    RawPipeline::RenderSettings settings = base;
    bool exposureOk = false;
    settings.exposure = m_rawCaptureExposure.toDouble(&exposureOk);
    if (!exposureOk || settings.exposure <= 0) settings.exposure = 1.0;
    const QString metadataPath = m_rawCapturePrefix + QLatin1String(".json");
    QImage image;
    QJsonObject info;
    QString error;
    if (!RawPipeline::renderImage({metadataPath}, settings, &image, &info, &error)) {
        m_rawCaptureErrors = error;
        return false;
    }
    if (m_rawCaptureRotationDegrees != 0) {
        image = image.transformed(QTransform().rotate(m_rawCaptureRotationDegrees));
    }
    ImageAdjustments::apply(&image, 1.0, m_rawCaptureColorTemperature,
                            m_rawCaptureColorTint);
    QJsonObject context;
    context.insert(QStringLiteral("raw_pipeline"), info);
    if (!ExifUtils::enrichSidecar(metadataPath, context, &error)) {
        m_rawCaptureErrors = error;
        return false;
    }
    const QFileInfo targetInfo(m_rawCaptureTargetPath);
    QDir().mkpath(targetInfo.absolutePath());
    QFile::remove(m_rawCaptureTargetPath);
    QImageWriter writer(m_rawCaptureTargetPath, "JPG");
    writer.setQuality(m_rawCaptureJpegQuality);
    writer.setProgressiveScanWrite(m_rawCaptureProgressiveJpeg);
    if (!writer.write(image)) {
        m_rawCaptureErrors = QStringLiteral("Could not save converted RAW image: %1")
                .arg(writer.errorString());
        return false;
    }
    QString rawError;
    const QString rawPath = rawPathFromMetadata(metadataPath, &rawError);
    if (rawError.isEmpty()) copyFileTimes(rawPath, m_rawCaptureTargetPath);
    return true;
}

bool DeclarativeCameraExtensions::renderRawImageWithFastJpegConverter()
{
    const QString jpegConverter = rawFastJpegConverterPath();
    if (!executableExists(jpegConverter)) {
        m_rawCaptureErrors = QStringLiteral("RAW JPEG converter is not installed: %1")
                .arg(jpegConverter);
        appendRawCaptureLog(m_rawCaptureErrors);
        return false;
    }

    const QString metadataPath = m_rawCapturePrefix + QLatin1String(".json");
    QString metadataError;
    const QString rawPath = rawPathFromMetadata(metadataPath, &metadataError);
    if (!metadataError.isEmpty()) {
        m_rawCaptureErrors = metadataError;
        appendRawCaptureLog(m_rawCaptureErrors);
        return false;
    }
    QFile metadataFile(metadataPath);
    const QString metadataFormat = metadataFile.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(metadataFile.readAll()).object()
                  .value(QStringLiteral("format")).toString()
            : QString();
    if (metadataFormat == QLatin1String("RAW10")) {
        m_rawCaptureErrors = QStringLiteral("Fast JPEG converter supports RAW16 only");
        appendRawCaptureLog(QStringLiteral("%1 metadata=%2 raw=%3")
                            .arg(m_rawCaptureErrors)
                            .arg(metadataPath)
                            .arg(rawPath));
        return false;
    }
    if (!QFileInfo(rawPath).isFile()) {
        m_rawCaptureErrors = QStringLiteral("RAW file is missing: %1").arg(rawPath);
        appendRawCaptureLog(QStringLiteral("%1 metadata=%2 converter=%3")
                            .arg(m_rawCaptureErrors)
                            .arg(metadataPath)
                            .arg(jpegConverter));
        return false;
    }

    const QFileInfo targetInfo(m_rawCaptureTargetPath);
    QDir().mkpath(targetInfo.absolutePath());
    QFile::remove(m_rawCaptureTargetPath);

    const bool helperHandlesRotation = (m_rawCaptureRotationDegrees % 90) == 0;
    QStringList arguments;
    arguments << metadataPath
              << m_rawCaptureTargetPath
              << m_rawCaptureExposure
              << QString::number(m_rawCaptureJpegQuality)
              << (m_rawCaptureProgressiveJpeg ? QStringLiteral("1")
                                              : QStringLiteral("0"))
              << QString::number(helperHandlesRotation
                                  ? m_rawCaptureRotationDegrees : 0);

    QProcess process;
    process.setProgram(jpegConverter);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(3000)) {
        m_rawCaptureErrors = QStringLiteral("Could not start RAW JPEG converter: %1")
                .arg(process.errorString());
        appendRawCaptureLog(QStringLiteral("%1 command=%2 metadata=%3 raw=%4 output=%5")
                            .arg(m_rawCaptureErrors)
                            .arg(jpegConverter)
                            .arg(metadataPath)
                            .arg(rawPath)
                            .arg(m_rawCaptureTargetPath));
        return false;
    }
    if (!process.waitForFinished(-1) ||
            process.exitStatus() != QProcess::NormalExit ||
            process.exitCode() != 0) {
        const QString errors = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        m_rawCaptureErrors = errors.isEmpty()
                ? QStringLiteral("RAW JPEG converter failed: %1").arg(jpegConverter)
                : QStringLiteral("%1 failed:\n%2").arg(jpegConverter, errors);
        appendRawCaptureLog(QStringLiteral("%1 metadata=%2 raw=%3 output=%4")
                            .arg(m_rawCaptureErrors)
                            .arg(metadataPath)
                            .arg(rawPath)
                            .arg(m_rawCaptureTargetPath));
        return false;
    }
    const QString converterOutput = QString::fromLocal8Bit(
                process.readAllStandardError()).trimmed();
    if (!converterOutput.isEmpty()) {
        appendRawCaptureLog(converterOutput);
    }

    if (!QFileInfo(m_rawCaptureTargetPath).isFile() ||
            QFileInfo(m_rawCaptureTargetPath).size() <= 0) {
        m_rawCaptureErrors = QStringLiteral("RAW JPEG converter did not create usable output");
        appendRawCaptureLog(QStringLiteral("%1 output=%2 metadata=%3 raw=%4")
                            .arg(m_rawCaptureErrors)
                            .arg(m_rawCaptureTargetPath)
                            .arg(metadataPath)
                            .arg(rawPath));
        return false;
    }

    if ((!helperHandlesRotation && m_rawCaptureRotationDegrees != 0) ||
            m_rawCaptureColorTint != 0) {
        QImage image(m_rawCaptureTargetPath);
        if (image.isNull()) {
            m_rawCaptureErrors = QStringLiteral("RAW JPEG converter output is not usable");
            appendRawCaptureLog(QStringLiteral("%1 output=%2 metadata=%3 raw=%4")
                                .arg(m_rawCaptureErrors)
                                .arg(m_rawCaptureTargetPath)
                                .arg(metadataPath)
                                .arg(rawPath));
            return false;
        }
        if (!helperHandlesRotation && m_rawCaptureRotationDegrees != 0) {
            image = image.transformed(QTransform().rotate(m_rawCaptureRotationDegrees));
        }
        ImageAdjustments::apply(&image, 1.0, 5500, m_rawCaptureColorTint);

        QImageWriter writer(m_rawCaptureTargetPath, "JPG");
        writer.setQuality(m_rawCaptureJpegQuality);
        if (m_rawCaptureProgressiveJpeg) {
            writer.setProgressiveScanWrite(true);
        }
        if (!writer.write(image)) {
            m_rawCaptureErrors = writer.errorString().isEmpty()
                    ? QStringLiteral("Could not save adjusted RAW JPEG")
                    : QStringLiteral("Could not save adjusted RAW JPEG: %1").arg(writer.errorString());
            return false;
        }
    }

    copyFileTimes(rawPath, m_rawCaptureTargetPath);
    return true;
}

bool DeclarativeCameraExtensions::preserveRawCaptureFiles()
{
    QString metadataError;
    QJsonObject context = m_captureMetadata;
    context.insert(QStringLiteral("render_engine"), m_rawRenderEngine);
    context.insert(QStringLiteral("render_exposure"), m_rawCaptureExposure);
    context.insert(QStringLiteral("color_temperature_requested_kelvin"), m_rawCaptureColorTemperature);
    context.insert(QStringLiteral("color_tint_requested"), m_rawCaptureColorTint);
    context.insert(QStringLiteral("jpeg_quality"), m_rawCaptureJpegQuality);
    context.insert(QStringLiteral("progressive_jpeg"), m_rawCaptureProgressiveJpeg);
    context.insert(QStringLiteral("raw_save_format"), m_rawCaptureSaveFormat);
    if (!ExifUtils::enrichSidecar(m_rawCapturePrefix + QLatin1String(".json"),
                                 context, &metadataError)) {
        m_rawCaptureErrors = metadataError;
        return false;
    }
    // Cameras without RAW stream YUV (I420) or, as a fallback, JPEG frames:
    // only kept when every burst frame is asked for, never as DNG.
    QString frameFormat;
    {
        QFile sidecar(m_rawCapturePrefix + QLatin1String(".json"));
        if (sidecar.open(QIODevice::ReadOnly))
            frameFormat = QJsonDocument::fromJson(sidecar.readAll()).object()
                    .value(QStringLiteral("format")).toString();
    }
    const bool yuvFrame = frameFormat == QLatin1String("I420");
    const bool processedFrame = yuvFrame || frameFormat == QLatin1String("JPEG");
    const bool saveRaw16 = (m_rawCaptureSaveFormat == QLatin1String("raw16")
            || m_rawCaptureSaveFormat == QLatin1String("both"))
            && (!processedFrame || (yuvFrame && m_bracketBurst && m_keepBurstFrames));
    const bool saveDng = (m_rawCaptureSaveFormat == QLatin1String("dng")
            || m_rawCaptureSaveFormat == QLatin1String("both")) && !processedFrame;
    if (!saveRaw16 && !saveDng) {
        return true;
    }
    if (m_rawCaptureArchivePrefix.isEmpty()) {
        return true;
    }

    const QFileInfo archiveInfo(m_rawCaptureArchivePrefix);
    QDir().mkpath(archiveInfo.absolutePath());

    if (saveRaw16) {
        QString metadataError;
        const QString rawSource = rawPathFromMetadata(
                    m_rawCapturePrefix + QLatin1String(".json"), &metadataError);
        const QString rawDestination = m_rawCaptureArchivePrefix
                + (yuvFrame ? QLatin1String(".yuv")
                   : rawSource.endsWith(QLatin1String(".raw10"))
                   ? QLatin1String(".raw10") : QLatin1String(".raw16"));
        if (!metadataError.isEmpty()) {
            m_rawCaptureErrors = metadataError;
            return false;
        } else if (QFileInfo(rawSource).absoluteFilePath() !=
                QFileInfo(rawDestination).absoluteFilePath()) {
            QFile::remove(rawDestination);
            if (!QFile::copy(rawSource, rawDestination)) {
                m_rawCaptureErrors = QStringLiteral("Could not preserve RAW file: %1").arg(rawDestination);
                return false;
            }
        }

        const QString metadataSource = m_rawCapturePrefix + QLatin1String(".json");
        const QString metadataDestination = m_rawCaptureArchivePrefix + QLatin1String(".json");
        if (metadataError.isEmpty() &&
                !writeMetadataWithRawPath(metadataSource, metadataDestination,
                                      rawDestination, &metadataError)) {
            m_rawCaptureErrors = metadataError;
            return false;
        }
    }
    if (saveDng) {
        const QString dngPath = m_rawCaptureArchivePrefix + QLatin1String(".dng");
        if (!writeDngSidecar(m_rawCapturePrefix + QLatin1String(".json"),
                             dngPath)) {
            return false;
        }
    }
    return true;
}

bool DeclarativeCameraExtensions::stageRawBracketFrame()
{
    if (m_rawBracketCancelled) { clearRawImageCapture(); return false; }
    QString error;
    const QString source = rawPathFromMetadata(m_rawCapturePrefix + QStringLiteral(".json"), &error);
    if (m_bracketIndex == 0) {
        // Next to the frames, so staging is a rename rather than a 25 MB copy
        // per frame; /tmp is often another filesystem.
        m_rawBracketDirectory.reset(new QTemporaryDir(
                QFileInfo(source).absolutePath() + QStringLiteral("/.procam-frames-XXXXXX")));
        if (!m_rawBracketDirectory->isValid()) m_rawBracketDirectory.reset(new QTemporaryDir);
    }
    const QString prefix = m_rawBracketDirectory && m_rawBracketDirectory->isValid()
            ? m_rawBracketDirectory->path() + QStringLiteral("/frame%1").arg(m_bracketIndex) : QString();
    const QString raw = prefix + QStringLiteral(".raw");
    const QString metadata = prefix + QStringLiteral(".json");
    const bool ownsSource = !m_rawCaptureDirectory && source.endsWith(QStringLiteral(".warm.raw16"));
    if (prefix.isEmpty() || !error.isEmpty()
            || !((ownsSource && QFile::rename(source, raw)) || QFile::copy(source, raw))
            || !writeMetadataWithRawPath(m_rawCapturePrefix + QStringLiteral(".json"), metadata, raw, &error)) {
        clearRawImageCapture();
        m_rawBracketDirectory.reset();
        emit rawImageCaptureFailed(error.isEmpty() ? QStringLiteral("Could not retain RAW bracket frame") : error);
        return false;
    }
    QJsonObject context;
    context.insert(QStringLiteral("bracket_rotation"), m_rawCaptureRotationDegrees);
    if (!ExifUtils::enrichSidecar(metadata, context, &error)) {
        clearRawImageCapture();
        m_rawBracketDirectory.reset();
        emit rawImageCaptureFailed(error);
        return false;
    }
    if (ownsSource) {
        QFile::remove(source);
        QFile::remove(m_rawCapturePrefix + QStringLiteral(".json"));
    }
    clearRawImageCapture();
    // Queue the signal so processing the second warm frame does not recurse.
    QTimer::singleShot(0, this, [this, metadata]() {
        if (m_rawBracketDirectory && QFileInfo::exists(metadata)) emit rawBracketFrameReady(metadata);
    });
    return true;
}

void DeclarativeCameraExtensions::clearRawImageCapture()
{
    QProcess *process = m_rawCaptureProcess.take();
    if (process) {
        process->deleteLater();
    }
    m_rawCaptureDirectory.reset();
    m_rawCaptureStage = RawCaptureIdle;
    m_rawCaptureTargetPath.clear();
    m_rawCapturePrefix.clear();
    m_rawCaptureArchivePrefix.clear();
    m_rawCaptureErrors.clear();
    m_rawCaptureExposure.clear();
    m_rawCaptureStandardOutput.clear();
    m_rawCaptureSaveFormat = QStringLiteral("raw16");
    m_rawRenderEngine = QStringLiteral("internal");
    m_rawCaptureProgressiveJpeg = false;
    m_rawCaptureJpegQuality = 92;
    m_rawCaptureRotationDegrees = 0;
    m_rawCaptureColorTemperature = 0;
    m_rawCaptureColorTint = 0;
    m_bracketIndex = -1;
    m_bracketCount = 0;
    m_bracketBurst = false;
    m_bracketEv = 0.0;
    m_bracketBaseShutterNs.clear();
    m_rawRenderStart = 0;
}
