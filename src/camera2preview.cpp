#include "camera2preview.h"
#include "exifutils.h"
#include "imageadjustments.h"
#include "previewaids.h"
#include "previewnode.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QUrl>
#include <cstring>
#include <unistd.h>
#include <QStandardPaths>
#include <QStringList>
#include <QTransform>
#include <QVariantMap>
#include <QtGlobal>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <climits>
#include <cmath>
#include <thread>

static QRectF photoAspectRect(const QSizeF &size, const QString &aspect);

// Milliseconds since this process started (from /proc), for start-up timing.
qint64 procamMsSinceProcessStart()
{
    QFile stat(QStringLiteral("/proc/self/stat")), uptime(QStringLiteral("/proc/uptime"));
    if (!stat.open(QIODevice::ReadOnly) || !uptime.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray line = stat.readAll();
    // Field 22 (start time in clock ticks); fields after the ")" of the name.
    const QList<QByteArray> fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
    if (fields.size() < 20)
        return -1;
    const double startSeconds = fields.at(19).toDouble() / double(sysconf(_SC_CLK_TCK));
    const double upSeconds = uptime.readAll().split(' ').value(0).toDouble();
    return qint64((upSeconds - startSeconds) * 1000.0);
}

namespace {

const int HistogramBins = 32;

bool isBenignCamera2HelperWarning(const QString &line)
{
    return line.contains(QStringLiteral("not accessible for the namespace")) ||
            line.contains(QStringLiteral("libandroidicu.so")) ||
            line.contains(QStringLiteral("libisu.so"));
}

void appendRawCaptureLog(const QString &message)
{
    QString picturesPath = QStandardPaths::writableLocation(
                QStandardPaths::PicturesLocation);
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

quint32 readLe32(const char *data)
{
    const uchar *bytes = reinterpret_cast<const uchar *>(data);
    return quint32(bytes[0])
            | (quint32(bytes[1]) << 8)
            | (quint32(bytes[2]) << 16)
            | (quint32(bytes[3]) << 24);
}

QString jsonSidecarPath(const QString &targetPath)
{
    const QFileInfo targetInfo(targetPath);
    return targetInfo.absolutePath() + QLatin1Char('/')
            + targetInfo.completeBaseName() + QLatin1String(".json");
}

QSize sizeFromString(const QString &value)
{
    const QStringList parts = value.split(QLatin1Char('x'));
    if (parts.count() != 2) {
        return QSize();
    }
    bool widthOk = false;
    bool heightOk = false;
    const int width = parts.at(0).toInt(&widthOk);
    const int height = parts.at(1).toInt(&heightOk);
    return widthOk && heightOk ? QSize(width, height) : QSize();
}

bool rewriteJpegPixels(const QString &path, int rotationDegrees, bool mirror, qreal exposure,
                       int colorTemperature, int colorTint, int quality,
                       QString *error)
{
    const int rotation = ((rotationDegrees % 360) + 360) % 360;
    if (rotation == 0 && !mirror &&
            !ImageAdjustments::requested(exposure, colorTemperature, colorTint)) {
        return true;
    }

    QImage image(path);
    if (image.isNull()) {
        if (error) {
            *error = QStringLiteral("Could not load Camera2 JPEG for adjustment");
        }
        return false;
    }
    if (rotation != 0) {
        image = image.transformed(QTransform().rotate(rotation));
    }
    // A selfie kept as the viewfinder showed it: flipped once upright.
    if (mirror) {
        image = image.mirrored(true, false);
    }
    ImageAdjustments::apply(&image, exposure, colorTemperature, colorTint);

    QImageWriter writer(path, "JPG");
    writer.setQuality(quality);
    if (!writer.write(image)) {
        if (error) {
            *error = QStringLiteral("Could not save adjusted Camera2 JPEG");
        }
        return false;
    }
    return true;
}

}

Camera2Preview::Camera2Preview(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_captureTimer.setInterval(50);
    connect(&m_captureTimer, &QTimer::timeout,
            this, &Camera2Preview::checkCaptureResult);
    // The helper finishes a file within about 1.5 s of record-stop.
    m_recordingStopTimer.setSingleShot(true);
    m_recordingStopTimer.setInterval(5000);
    connect(&m_recordingStopTimer, &QTimer::timeout, this, [this]() {
        finishRecording(false, QStringLiteral("Recording did not finish"));
    });
}

Camera2Preview::~Camera2Preview()
{
    stop();
}

bool Camera2Preview::active() const
{
    return m_active;
}

void Camera2Preview::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    emit activeChanged();
    restart();
}

bool Camera2Preview::running() const
{
    return m_running;
}

bool Camera2Preview::jpegCaptureReady() const
{
    return m_running && m_jpegCaptureEnabled && m_process &&
            m_process->state() == QProcess::Running;
}

bool Camera2Preview::rawCaptureReady() const
{
    return m_running && m_rawCaptureEnabled && m_rawStreamActive && m_process &&
            m_process->state() == QProcess::Running;
}

QString Camera2Preview::cameraId() const
{
    return m_cameraId;
}

void Camera2Preview::setCameraId(const QString &cameraId)
{
    const QString effectiveId = cameraId.isEmpty() ? QStringLiteral("0") : cameraId;
    if (m_cameraId == effectiveId) {
        return;
    }
    m_cameraId = effectiveId;
    stop();
    // Keep a blurred, dimmed last frame until the new camera delivers one,
    // instead of a blank or jumping viewfinder.
    // Stored as shown (the next camera may be mounted the other way round).
    if (hasFrame()) {
        const QImage shown = displayFrame();
        const QSize size = shown.size();
        QImage small = shown.scaled(qMax(1, size.width() / 24), qMax(1, size.height() / 24),
                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QImage blurred = small.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_RGB888);
        for (int y = 0; y < blurred.height(); ++y) {
            uchar *line = blurred.scanLine(y);
            for (int x = 0; x < blurred.width() * 3; ++x) line[x] = uchar(line[x] * 3 / 4);
        }
        m_frame = blurred;
        m_yuvFrame.clear();
        m_yuvSize = QSize();
        m_frameDisplayOriented = true;
        ++m_frameSerial;
    }
    update();
    emit cameraIdChanged();
    restart();
}

QSize Camera2Preview::previewSize() const
{
    return m_previewSize;
}

void Camera2Preview::setPreviewSize(const QSize &previewSize)
{
    if (m_previewSize == previewSize) {
        return;
    }
    m_previewSize = previewSize;
    emit previewSizeChanged();
    restart();
}

QString Camera2Preview::captureSize() const
{
    return m_captureSize;
}

void Camera2Preview::setCaptureSize(const QString &captureSize)
{
    const QString effectiveSize = captureSize.isEmpty()
            ? QStringLiteral("4096x3072") : captureSize;
    if (m_captureSize == effectiveSize) {
        return;
    }
    m_captureSize = effectiveSize;
    emit captureSizeChanged();
    restart();
}

QString Camera2Preview::rawCaptureSize() const
{
    return m_rawCaptureSize.isEmpty() ? m_captureSize : m_rawCaptureSize;
}

void Camera2Preview::setRawCaptureSize(const QString &rawCaptureSize)
{
    if (m_rawCaptureSize == rawCaptureSize) {
        return;
    }
    m_rawCaptureSize = rawCaptureSize;
    emit rawCaptureSizeChanged();
    if (m_rawCaptureEnabled) {
        restart();
    }
}

QString Camera2Preview::saveThumbnail(int captureRotation)
{
    if (!hasFrame()) {
        return QString();
    }
    // The frame is in sensor orientation and unmirrored, like the photo
    // before its rotation; turn it upright the same way.
    QImage image = rgbFrame().transformed(QTransform().rotate(captureRotation));
    if (m_captureMirror)
        image = image.mirrored(true, false);
    image = image.copy(photoAspectRect(image.size(), m_photoAspect).toRect());
    const qreal zoom = qMax<qreal>(1.0, m_zoom);
    if (zoom > 1.01) {
        const QSize size(qMax(1, int(image.width() / zoom)), qMax(1, int(image.height() / zoom)));
        image = image.copy(QRect(QPoint((image.width() - size.width()) / 2,
                                        (image.height() - size.height()) / 2), size));
    }
    image = image.scaled(256, 256, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        return QString();
    }
    // A fresh name each time so QML never shows a cached older thumbnail.
    static int counter = 0;
    counter = (counter + 1) % 8;
    const QString path = directory + QStringLiteral("/pending-thumbnail-%1.jpg").arg(counter);
    if (!image.save(path, "JPG", 85)) {
        return QString();
    }
    return QUrl::fromLocalFile(path).toString();
}

QString Camera2Preview::rawStreamStatus() const
{
    return m_rawStreamStatus;
}

QString Camera2Preview::rawCaptureFormat() const
{
    return m_rawCaptureFormat;
}

void Camera2Preview::setRawCaptureFormat(const QString &rawCaptureFormat)
{
    const QString format = rawCaptureFormat == QLatin1String("yuv")
            ? QStringLiteral("yuv") : QStringLiteral("raw16");
    if (m_rawCaptureFormat == format) {
        return;
    }
    m_rawCaptureFormat = format;
    emit rawCaptureFormatChanged();
    if (m_rawCaptureEnabled) {
        restart();
    }
}

int Camera2Preview::captureTimeout() const
{
    return m_captureTimeout;
}

void Camera2Preview::setCaptureTimeout(int captureTimeout)
{
    const int timeout = qBound(1, captureTimeout, 3600);
    if (m_captureTimeout == timeout) {
        return;
    }
    m_captureTimeout = timeout;
    emit captureTimeoutChanged();
}

bool Camera2Preview::jpegCaptureEnabled() const
{
    return m_jpegCaptureEnabled;
}

void Camera2Preview::setJpegCaptureEnabled(bool jpegCaptureEnabled)
{
    if (m_jpegCaptureEnabled == jpegCaptureEnabled) {
        return;
    }
    m_jpegCaptureEnabled = jpegCaptureEnabled;
    emit jpegCaptureEnabledChanged();
    restart();
}

bool Camera2Preview::rawCaptureEnabled() const
{
    return m_rawCaptureEnabled;
}

void Camera2Preview::setVideoSize(const QSize &videoSize)
{
    if (m_videoSize == videoSize)
        return;
    m_videoSize = videoSize;
    emit videoSizeChanged();
    restart();
}

void Camera2Preview::setVideoFrameRate(int videoFrameRate)
{
    if (m_videoFrameRate == videoFrameRate || videoFrameRate <= 0)
        return;
    m_videoFrameRate = videoFrameRate;
    emit videoFrameRateChanged();
    if (!m_videoSize.isEmpty())
        restart();
}

void Camera2Preview::setVideoBitrate(int videoBitrate)
{
    if (m_videoBitrate == videoBitrate || videoBitrate <= 0)
        return;
    m_videoBitrate = videoBitrate;
    emit videoBitrateChanged();
    if (!m_videoSize.isEmpty())
        restart();
}

void Camera2Preview::setVideoStabilization(bool videoStabilization)
{
    if (m_videoStabilization == videoStabilization)
        return;
    m_videoStabilization = videoStabilization;
    emit videoStabilizationChanged();
    if (!m_videoSize.isEmpty())
        restart();
}

void Camera2Preview::setVideoCodec(const QString &videoCodec)
{
    const QString codec = videoCodec == QLatin1String("hevc") ? videoCodec : QStringLiteral("avc");
    if (m_videoCodec == codec)
        return;
    m_videoCodec = codec;
    emit videoCodecChanged();
    if (!m_videoSize.isEmpty())
        restart();
}

bool Camera2Preview::startRecording(const QString &path, int orientation)
{
    if (!m_process || !m_running || !m_videoReady || m_recording || path.isEmpty()
            || !QFileInfo(path).isAbsolute() || !prepareCaptureFolders({path})) {
        qWarning() << "record-start refused" << path << "running" << m_running
                   << "video" << m_videoReady << "recording" << m_recording;
        return false;
    }
    const int degrees = ((orientation % 360) + 360) % 360 / 90 * 90;
    const QByteArray command = "record-start " + QByteArray::number(degrees) + ' '
            + QFile::encodeName(path) + '\n';
    if (m_process->write(command) < 0)
        return false;
    m_recording = true;
    m_recordingPath = path;
    emit recordingChanged();
    return true;
}

void Camera2Preview::stopRecording()
{
    if (!m_recording || m_recordingStopTimer.isActive())
        return;
    if (m_process)
        m_process->write("record-stop\n");
    m_recordingStopTimer.start();
}

void Camera2Preview::finishRecording(bool success, const QString &error)
{
    if (!m_recording)
        return;
    m_recordingStopTimer.stop();
    const QString path = m_recordingPath;
    m_recording = false;
    m_recordingPath.clear();
    qDebug() << "record-finished" << path << success << error;
    emit recordingChanged();
    emit recordingFinished(path, success, error);
}

// record-status=started|ok|error code=N path=... (the path is the rest of
// the payload and may contain spaces).
void Camera2Preview::parseRecordStatus(const QByteArray &payload,
                                       const QMap<QString, QString> &fields)
{
    const QString status = fields.value(QStringLiteral("record-status"));
    const int pathAt = payload.indexOf(" path=");
    const QString path = pathAt >= 0 ? QFile::decodeName(payload.mid(pathAt + 6).trimmed())
                                     : QString();
    if (!m_recording || (path != m_recordingPath && path != QLatin1String("-")))
        return;
    if (status == QLatin1String("started")) {
        emit recordingStarted(path);
    } else if (status == QLatin1String("ok")) {
        finishRecording(true, QString());
    } else {
        finishRecording(false, QStringLiteral("Recording failed (%1)")
                        .arg(fields.value(QStringLiteral("code"))));
    }
}

void Camera2Preview::setRawCaptureEnabled(bool rawCaptureEnabled)
{
    if (m_rawCaptureEnabled == rawCaptureEnabled) {
        return;
    }
    m_rawCaptureEnabled = rawCaptureEnabled;
    emit rawCaptureEnabledChanged();
    emit rawCaptureReadyChanged();
    restart();
}

int Camera2Preview::jpegQuality() const
{
    return m_jpegQuality;
}

void Camera2Preview::setJpegQuality(int jpegQuality)
{
    const int quality = qBound(1, jpegQuality, 100);
    if (m_jpegQuality == quality) {
        return;
    }
    m_jpegQuality = quality;
    emit jpegQualityChanged();
    restart();
}

int Camera2Preview::jpegOrientation() const
{
    return m_jpegOrientation;
}

void Camera2Preview::setJpegOrientation(int jpegOrientation)
{
    const int orientation = ((jpegOrientation % 360) + 360) % 360;
    if (m_jpegOrientation == orientation) {
        return;
    }
    m_jpegOrientation = orientation;
    emit jpegOrientationChanged();
    // Applied when the photo is saved; the helper always gets 0, so turning
    // the phone needs no camera restart.
}

QVariant Camera2Preview::source() const
{
    return m_source;
}

void Camera2Preview::setSource(const QVariant &source)
{
    if (m_source == source) {
        return;
    }
    m_source = source;
    emit sourceChanged();
}

int Camera2Preview::orientation() const
{
    return m_orientation;
}

void Camera2Preview::setOrientation(int orientation)
{
    const int normalized = ((orientation % 360) + 360) % 360;
    if (m_orientation == normalized) {
        return;
    }
    m_orientation = normalized;
    emit orientationChanged();
    update();
}

bool Camera2Preview::mirror() const
{
    return m_mirror;
}

void Camera2Preview::setMirror(bool mirror)
{
    if (m_mirror == mirror) {
        return;
    }
    m_mirror = mirror;
    emit mirrorChanged();
    update();
}

bool Camera2Preview::fill() const
{
    return m_fill;
}

void Camera2Preview::setFill(bool fill)
{
    if (m_fill == fill) {
        return;
    }
    m_fill = fill;
    emit fillChanged();
    emit contentRectChanged();
    update();
}

QString Camera2Preview::photoAspect() const
{
    return m_photoAspect;
}

void Camera2Preview::setPhotoAspect(const QString &aspect)
{
    if (m_photoAspect == aspect) {
        return;
    }
    m_photoAspect = aspect;
    emit photoAspectChanged();
    emit contentRectChanged();
    update();
}

// Centre part of a frame of `size` the photo keeps: "16:9" crops the short
// side (as RawPipeline does before rotating), anything else keeps it all.
static QRectF photoAspectRect(const QSizeF &size, const QString &aspect)
{
    QRectF rect(QPointF(0, 0), size);
    if (aspect != QLatin1String("16:9")) {
        return rect;
    }
    if (size.width() >= size.height()) {
        const qreal height = qMin(size.height(), size.width() * 9 / 16);
        rect = QRectF(0, (size.height() - height) / 2, size.width(), height);
    } else {
        const qreal width = qMin(size.width(), size.height() * 9 / 16);
        rect = QRectF((size.width() - width) / 2, 0, width, size.height());
    }
    return rect;
}

qreal Camera2Preview::zoom() const
{
    return m_zoom;
}

void Camera2Preview::setZoom(qreal zoom)
{
    const qreal clamped = qMax<qreal>(1.0, zoom);
    if (qFuzzyCompare(m_zoom, clamped)) {
        return;
    }
    m_zoom = clamped;
    emit zoomChanged();
    emit contentRectChanged();
    update();
    sendSettings();
}

QString Camera2Preview::focusMode() const
{
    return m_focusMode;
}

void Camera2Preview::setFocusMode(const QString &focusMode)
{
    const QString mode = focusMode.isEmpty() ? QStringLiteral("continuous")
                                             : focusMode;
    if (m_focusMode == mode) {
        return;
    }
    m_focusMode = mode;
    emit focusModeChanged();
    sendSettings();
}

qreal Camera2Preview::focusDistance() const
{
    return m_focusDistance;
}

void Camera2Preview::setFocusDistance(qreal focusDistance)
{
    const qreal distance = qMax<qreal>(0.0, focusDistance);
    if (qFuzzyCompare(m_focusDistance, distance)) {
        return;
    }
    m_focusDistance = distance;
    emit focusDistanceChanged();
    sendSettings();
}

int Camera2Preview::exposureCompensation() const
{
    return m_exposureCompensation;
}

void Camera2Preview::setExposureCompensation(int exposureCompensation)
{
    const int compensation = qBound(-24, exposureCompensation, 24);
    if (m_exposureCompensation == compensation) {
        return;
    }
    m_exposureCompensation = compensation;
    emit exposureCompensationChanged();
    sendSettings();
}

void Camera2Preview::setExposurePair(int iso, const QString &shutter, int compensationSteps)
{
    if ((iso > 0) != (shutter.toLongLong() > 0)) return;
    m_sensorSensitivity = qMax(0, iso);
    m_exposureTime = shutter;
    m_exposureCompensation = compensationSteps;
    emit sensorSensitivityChanged();
    emit exposureTimeChanged();
    emit exposureCompensationChanged();
    sendSettings();
}

int Camera2Preview::sensorSensitivity() const
{
    return m_sensorSensitivity;
}

void Camera2Preview::setSensorSensitivity(int sensorSensitivity)
{
    const int sensitivity = qBound(0, sensorSensitivity, 102400);
    if (m_sensorSensitivity == sensitivity) {
        return;
    }
    m_sensorSensitivity = sensitivity;
    emit sensorSensitivityChanged();
    sendSettings();
}

QString Camera2Preview::exposureTime() const
{
    return m_exposureTime;
}

void Camera2Preview::setExposureTime(const QString &exposureTime)
{
    const QString time = exposureTime.isEmpty() ? QStringLiteral("0")
                                                : exposureTime;
    if (m_exposureTime == time) {
        return;
    }
    m_exposureTime = time;
    emit exposureTimeChanged();
    sendSettings();
}

int Camera2Preview::aperture() const
{
    return m_aperture;
}

void Camera2Preview::setAperture(int aperture)
{
    const int value = qBound(0, aperture, 255);
    if (m_aperture == value) {
        return;
    }
    m_aperture = value;
    emit apertureChanged();
    sendSettings();
}

int Camera2Preview::noiseReduction() const
{
    return m_noiseReduction;
}

void Camera2Preview::setNoiseReduction(int noiseReduction)
{
    const int reduction = qMax(0, noiseReduction);
    if (m_noiseReduction == reduction) {
        return;
    }
    m_noiseReduction = reduction;
    emit noiseReductionChanged();
    sendSettings();
}

void Camera2Preview::setFocusPeaking(bool enabled)
{
    if (m_focusPeaking == enabled) return;
    m_focusPeaking = enabled;
    emit focusPeakingChanged();
    update();
}

void Camera2Preview::setExposureZebras(bool enabled)
{
    if (m_exposureZebras == enabled) return;
    m_exposureZebras = enabled;
    emit exposureZebrasChanged();
    update();
}

qreal Camera2Preview::renderExposure() const
{
    return m_renderExposure;
}

void Camera2Preview::setRenderExposure(qreal renderExposure)
{
    const qreal exposure = qBound<qreal>(0.25, renderExposure, 4.0);
    if (qFuzzyCompare(m_renderExposure, exposure)) {
        return;
    }
    m_renderExposure = exposure;
    emit renderExposureChanged();
    update();
}

QString Camera2Preview::sceneMode() const
{
    return m_sceneMode;
}

void Camera2Preview::setSceneMode(const QString &sceneMode)
{
    const QString mode = sceneMode.isEmpty() ? QStringLiteral("manual")
                                             : sceneMode;
    if (m_sceneMode == mode) {
        return;
    }
    m_sceneMode = mode;
    emit sceneModeChanged();
    sendSettings();
}

int Camera2Preview::colorTemperature() const
{
    return m_colorTemperature;
}

void Camera2Preview::setColorTemperature(int colorTemperature)
{
    const int temperature = qBound(0, colorTemperature, 50000);
    if (m_colorTemperature == temperature) {
        return;
    }
    m_colorTemperature = temperature;
    emit colorTemperatureChanged();
    update();
    sendSettings();
}

int Camera2Preview::colorTint() const
{
    return m_colorTint;
}

void Camera2Preview::setColorTint(int colorTint)
{
    const int tint = qBound(-1000, colorTint, 1000);
    if (m_colorTint == tint) {
        return;
    }
    m_colorTint = tint;
    emit colorTintChanged();
    update();
    sendSettings();
}

qreal Camera2Preview::focalLength() const
{
    return m_focalLength;
}

int Camera2Preview::liveSensorSensitivity() const
{
    return m_liveSensorSensitivity;
}

QString Camera2Preview::liveExposureTime() const
{
    return m_liveExposureTime;
}

QVariantList Camera2Preview::histogram() const
{
    return m_histogram;
}

QString Camera2Preview::errorString() const
{
    return m_errorString;
}

/**
 * @brief Hands the latest bridge RGB frame to the scene graph.
 *
 * The bridge already converts YUV420 to RGB888. PreviewNode uploads it into
 * one reused texture and rotates, mirrors and lightly sharpens it on the GPU,
 * so the GUI thread does no per-frame image work beyond one copy.
 */
QSGNode *Camera2Preview::updatePaintNode(QSGNode *oldNode,
                                         UpdatePaintNodeData *)
{
    if (!hasFrame() || width() <= 0 || height() <= 0 || !window()) {
        delete oldNode;
        m_drawnSerial = 0;
        return nullptr;
    }

    PreviewNode *node = static_cast<PreviewNode *>(oldNode);
    if (!node) {
        node = new PreviewNode;
        m_drawnSerial = 0;
    }
    if (m_drawnSerial != m_frameSerial) {
        const bool adjusted = ImageAdjustments::requested(m_renderExposure, m_colorTemperature,
                                                          m_colorTint);
        if (!m_yuvFrame.isEmpty() && !adjusted && !m_focusPeaking && !m_exposureZebras) {
            // The usual case: the YUV planes go to the GPU as they came.
            node->setYuvFrame(m_yuvFrame, m_yuvSize);
        } else {
            // Pro's colour adjustments and preview aids work on RGB.
            QImage textureImage = rgbFrame();
            if (adjusted) {
                textureImage = textureImage.copy();
                ImageAdjustments::apply(&textureImage, m_renderExposure,
                                        m_colorTemperature, m_colorTint);
            }
            // Display-only copy: never feed overlays into metering, histogram or capture.
            textureImage = PreviewAids::apply(textureImage, m_focusPeaking, m_exposureZebras);
            if (textureImage.format() != QImage::Format_RGB888)
                textureImage = textureImage.convertToFormat(QImage::Format_RGB888);
            node->setFrame(textureImage);
        }
        m_drawnSerial = m_frameSerial;
    }

    QRectF drawRect, sourceRect;
    const QSize shown = displaySize();
    frameGeometry(shown, &drawRect, &sourceRect);
    node->setPlacement(drawRect,
                      QRectF(sourceRect.x() / shown.width(), sourceRect.y() / shown.height(),
                             sourceRect.width() / shown.width(), sourceRect.height() / shown.height()),
                      frameRotation(), frameMirror());
    return node;
}

int Camera2Preview::frameRotation() const
{
    return m_frameDisplayOriented ? 0 : ((m_orientation % 360) + 360) % 360;
}

bool Camera2Preview::frameMirror() const
{
    return !m_frameDisplayOriented && m_mirror;
}

QSize Camera2Preview::displaySize() const
{
    const int rotation = frameRotation();
    return rotation == 90 || rotation == 270 ? frameSize().transposed() : frameSize();
}

QImage Camera2Preview::displayFrame() const
{
    QImage frame = rgbFrame();
    if (frameRotation())
        frame = frame.transformed(QTransform().rotate(frameRotation()));
    if (frameMirror())
        frame = frame.mirrored(true, false);
    return frame;
}

bool Camera2Preview::frameGeometry(const QSizeF &imageSize, QRectF *drawRect,
                                   QRectF *sourceRect) const
{
    if (imageSize.isEmpty() || width() <= 0 || height() <= 0) {
        return false;
    }
    const QRectF target(0, 0, width(), height());
    const qreal zoom = qMax<qreal>(1.0, m_zoom);
    const QRectF photo = photoAspectRect(imageSize, m_photoAspect);
    const QSizeF sourceSize(photo.width() / zoom, photo.height() / zoom);
    *sourceRect = QRectF(photo.center().x() - sourceSize.width() / 2,
                         photo.center().y() - sourceSize.height() / 2,
                         sourceSize.width(), sourceSize.height());
    const qreal scale = m_fill
            ? qMax(target.width() / sourceSize.width(),
                   target.height() / sourceSize.height())
            : qMin(target.width() / sourceSize.width(),
                   target.height() / sourceSize.height());
    const QSizeF drawSize(sourceSize.width() * scale,
                          sourceSize.height() * scale);
    *drawRect = QRectF(target.center().x() - drawSize.width() / 2,
                       target.center().y() - drawSize.height() / 2,
                       drawSize.width(), drawSize.height());
    return true;
}

QRectF Camera2Preview::contentRect() const
{
    QRectF drawRect, sourceRect;
    return frameGeometry(displaySize(), &drawRect, &sourceRect)
            ? drawRect : QRectF(0, 0, width(), height());
}

QPointF Camera2Preview::mapToSensor(qreal x, qreal y) const
{
    QRectF drawRect, sourceRect;
    const QSize shown = displaySize();
    if (!frameGeometry(shown, &drawRect, &sourceRect)
            || !drawRect.contains(QPointF(x, y))) {
        return QPointF(-1, -1);
    }
    // Item -> displayed frame (normalised), through the zoom crop.
    qreal u = (sourceRect.x() + (x - drawRect.x()) / drawRect.width() * sourceRect.width())
            / shown.width();
    qreal v = (sourceRect.y() + (y - drawRect.y()) / drawRect.height() * sourceRect.height())
            / shown.height();
    // The display rotates the frame by m_orientation, then mirrors it.
    if (m_mirror) {
        u = 1.0 - u;
    }
    qreal sensorX = u, sensorY = v;
    switch (((m_orientation % 360) + 360) % 360) {
    case 90: sensorX = v; sensorY = 1.0 - u; break;
    case 180: sensorX = 1.0 - u; sensorY = 1.0 - v; break;
    case 270: sensorX = 1.0 - v; sensorY = u; break;
    default: break;
    }
    return QPointF(qBound<qreal>(0.0, sensorX, 1.0), qBound<qreal>(0.0, sensorY, 1.0));
}

void Camera2Preview::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChanged(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        emit contentRectChanged();
    }
}

void Camera2Preview::restartPreview()
{
    restart();
}

void Camera2Preview::setFocusPoint(qreal x, qreal y)
{
    if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
        return;
    }
    m_focusX = x;
    m_focusY = y;
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->write(QStringLiteral("focus %1 %2\n")
                         .arg(m_focusX, 0, 'f', 4)
                         .arg(m_focusY, 0, 'f', 4)
                         .toLocal8Bit());
    }
}

void Camera2Preview::clearFocusPoint()
{
    m_focusX = -1.0;
    m_focusY = -1.0;
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->write("focus-reset\n");
    }
}

void Camera2Preview::holdFocus()
{
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->write("focus-hold\n");
    }
}

void Camera2Preview::releaseFocusHold()
{
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->write("focus-hold-release\n");
    }
}

void Camera2Preview::setCaptureMetadata(const QVariantMap &metadata)
{
    m_captureMetadata = ExifUtils::captureContext(metadata);
    m_originalJpegExif.clear();
}

// Creates the folders of the files a capture writes. A folder that cannot
// be created or written fails the capture up front, with a message that
// names it, instead of the bridge failing to write a frame later.
bool Camera2Preview::prepareCaptureFolders(const QStringList &paths)
{
    m_captureStartError.clear();
    for (const QString &path : paths) {
        const QString directory = QFileInfo(path).absolutePath();
        if (QDir().mkpath(directory) && QFileInfo(directory).isWritable()) continue;
        m_captureStartError = QStringLiteral("Cannot write to folder %1").arg(directory);
        appendRawCaptureLog(m_captureStartError);
        setErrorString(m_captureStartError);
        return false;
    }
    return true;
}

bool Camera2Preview::captureJpeg(const QString &path)
{
    if (path.isEmpty()) {
        setErrorString(QStringLiteral("Camera2 JPEG capture path is empty"));
        return false;
    }
    if (!m_process || m_process->state() != QProcess::Running) {
        setErrorString(QStringLiteral("Camera2 preview process is not running"));
        return false;
    }
    if (m_previewCaptureRunning) {
        setErrorString(QStringLiteral("Camera2 JPEG capture is already running"));
        return false;
    }

    if (!m_jpegCaptureEnabled && !hasFrame()) {
        setErrorString(QStringLiteral("Camera2 preview frame is not ready"));
        return false;
    }

    if (!prepareCaptureFolders({path})) return false;

    m_lastCaptureResultFields.clear();
    m_previewCaptureRunning = true;
    if (m_jpegCaptureEnabled) {
        QFile::remove(path);
        m_pendingCapturePath = path;
        m_pendingCaptureFinalPath.clear();
        m_pendingRawCapture = false;
        m_pendingCaptureSize = -1;
        m_pendingCaptureStablePolls = 0;
        m_capturePollsRemaining = qMax(1, (m_captureTimeout * 1000)
                                          / m_captureTimer.interval());
        const QByteArray command = QByteArrayLiteral("capture-jpeg ")
                + QFile::encodeName(path) + QByteArrayLiteral("\n");
        if (m_exposureOnlyCapture) {
            sendExposureSettings(true);
        } else {
            sendSettings(true);
        }
        m_restorePreviewSettingsAfterCapture = true;
        m_restoreExposureOnlySettingsAfterCapture = m_exposureOnlyCapture;
        if (m_process->write(command) >= 0) {
            m_captureTimer.start();
            return true;
        }
        restorePreviewSettingsAfterCapture();
        m_pendingCapturePath.clear();
        m_pendingCaptureFinalPath.clear();
        m_pendingRawCapture = false;
        m_previewCaptureRunning = false;
        setErrorString(QStringLiteral("Could not write Camera2 JPEG capture command"));
        return false;
    }

    savePreviewJpegAsync(path);
    return true;
}

bool Camera2Preview::captureJpegWithExposure(const QString &path,
                                             int sensorSensitivity,
                                             const QString &exposureTime)
{
    const int previousSensitivity = m_sensorSensitivity;
    const QString previousExposureTime = m_exposureTime;
    const bool previousExposureOnlyCapture = m_exposureOnlyCapture;
    m_sensorSensitivity = qMax(0, sensorSensitivity);
    m_exposureTime = exposureTime.isEmpty() ? QStringLiteral("0")
                                            : exposureTime;
    m_exposureOnlyCapture = true;
    const bool started = captureJpeg(path);
    m_sensorSensitivity = previousSensitivity;
    m_exposureTime = previousExposureTime;
    m_exposureOnlyCapture = previousExposureOnlyCapture;
    return started;
}

bool Camera2Preview::captureRaw(const QString &rawPath,
                                const QString &metadataPath)
{
    if (rawPath.isEmpty() || metadataPath.isEmpty()) {
        setErrorString(QStringLiteral("Camera2 RAW capture path is empty"));
        return false;
    }
    if (!rawCaptureReady()) {
        setErrorString(QStringLiteral("Camera2 RAW preview is not running"));
        return false;
    }
    if (m_previewCaptureRunning) {
        setErrorString(QStringLiteral("Camera2 RAW capture is already running"));
        return false;
    }

    if (!prepareCaptureFolders({rawPath, metadataPath})) return false;
    QFile::remove(rawPath);
    QFile::remove(metadataPath);

    m_previewCaptureRunning = true;
    m_captureClock.start();
    m_pendingCapturePath = metadataPath;
    m_pendingCaptureFinalPath = rawPath;
    m_pendingRawCapture = true;
    m_pendingCaptureSize = -1;
    m_pendingCaptureStablePolls = 0;
    m_capturePollsRemaining = qMax(1, (m_captureTimeout * 1000)
                                      / m_captureTimer.interval());
    const QByteArray command = QByteArrayLiteral("capture-raw ")
            + QFile::encodeName(rawPath) + QByteArrayLiteral(" ")
            + QFile::encodeName(metadataPath) + QByteArrayLiteral("\n");
    if (m_exposureOnlyCapture) {
        sendExposureSettings(true);
    } else {
        sendSettings(true);
    }
    m_restorePreviewSettingsAfterCapture = true;
    m_restoreExposureOnlySettingsAfterCapture = m_exposureOnlyCapture;
    if (m_process->write(command) >= 0) {
        m_captureTimer.start();
        return true;
    }
    restorePreviewSettingsAfterCapture();

    m_pendingCapturePath.clear();
    m_pendingCaptureFinalPath.clear();
    m_pendingRawCapture = false;
    m_previewCaptureRunning = false;
    setErrorString(QStringLiteral("Could not write Camera2 RAW capture command"));
    return false;
}

bool Camera2Preview::captureRawWithExposure(const QString &rawPath,
                                            const QString &metadataPath,
                                            int sensorSensitivity,
                                            const QString &exposureTime)
{
    const int previousSensitivity = m_sensorSensitivity;
    const QString previousExposureTime = m_exposureTime;
    const bool previousExposureOnlyCapture = m_exposureOnlyCapture;
    m_sensorSensitivity = qMax(0, sensorSensitivity);
    m_exposureTime = exposureTime.isEmpty() ? QStringLiteral("0")
                                            : exposureTime;
    m_exposureOnlyCapture = true;
    const bool started = captureRaw(rawPath, metadataPath);
    m_sensorSensitivity = previousSensitivity;
    m_exposureTime = previousExposureTime;
    m_exposureOnlyCapture = previousExposureOnlyCapture;
    return started;
}

bool Camera2Preview::captureRawBracket(const QVariantList &rawPaths,
                                       const QVariantList &metadataPaths,
                                       int sensorSensitivity,
                                       const QVariantList &exposureTimes)
{
    const int count = rawPaths.count();
    if (count != 2 || count != metadataPaths.count() ||
            count != exposureTimes.count()) {
        setErrorString(QStringLiteral("Camera2 RAW bracket is incomplete"));
        return false;
    }

    QByteArray command = QByteArrayLiteral("capture-raw-bracket ")
            + QByteArray::number(count);
    for (int index = 0; index < count; ++index) {
        const QString rawPath = rawPaths.at(index).toString();
        const QString metadataPath = metadataPaths.at(index).toString();
        const QString exposureTime = exposureTimes.at(index).toString();
        if (rawPath.isEmpty() || metadataPath.isEmpty() ||
                exposureTime.isEmpty()) {
            setErrorString(QStringLiteral("Camera2 RAW bracket path is empty"));
            return false;
        }
        command += QByteArrayLiteral(" ") + QFile::encodeName(rawPath)
                + QByteArrayLiteral(" ") + QFile::encodeName(metadataPath)
                + QByteArrayLiteral(" ")
                + QByteArray::number(qMax(0, sensorSensitivity))
                + QByteArrayLiteral(" ")
                + exposureTime.toLatin1();
    }
    command += QByteArrayLiteral("\n");
    return startRawFrameSet(command, rawPaths, metadataPaths);
}

bool Camera2Preview::captureRawBurst(const QVariantList &rawPaths,
                                     const QVariantList &metadataPaths,
                                     int sensorSensitivity,
                                     const QString &exposureTime)
{
    const int count = rawPaths.count();
    // Bridge limit: PREVIEW_MAX_RAW_BRACKET in preview_commands.h.
    if (count < 2 || count > 16 || count != metadataPaths.count()
            || sensorSensitivity <= 0 || exposureTime.toLongLong() <= 0) {
        setErrorString(QStringLiteral("Camera2 RAW burst is incomplete"));
        return false;
    }
    QByteArray command = QByteArrayLiteral("capture-raw-burst ")
            + QByteArray::number(count) + QByteArrayLiteral(" ")
            + QByteArray::number(sensorSensitivity) + QByteArrayLiteral(" ")
            + QByteArray::number(exposureTime.toLongLong());
    for (int index = 0; index < count; ++index) {
        const QString rawPath = rawPaths.at(index).toString();
        const QString metadataPath = metadataPaths.at(index).toString();
        if (rawPath.isEmpty() || metadataPath.isEmpty()) {
            setErrorString(QStringLiteral("Camera2 RAW burst path is empty"));
            return false;
        }
        command += QByteArrayLiteral(" ") + QFile::encodeName(rawPath)
                + QByteArrayLiteral(" ") + QFile::encodeName(metadataPath);
    }
    command += QByteArrayLiteral("\n");
    return startRawFrameSet(command, rawPaths, metadataPaths);
}

bool Camera2Preview::startRawFrameSet(const QByteArray &command,
                                      const QVariantList &rawPaths,
                                      const QVariantList &metadataPaths)
{
    const int count = rawPaths.count();
    if (!rawCaptureReady()) {
        setErrorString(QStringLiteral("Camera2 RAW preview is not running"));
        return false;
    }
    if (m_previewCaptureRunning) {
        setErrorString(QStringLiteral("Camera2 RAW capture is already running"));
        return false;
    }
    QStringList folders;
    for (int index = 0; index < count; ++index)
        folders << rawPaths.at(index).toString() << metadataPaths.at(index).toString();
    if (!prepareCaptureFolders(folders)) return false;
    for (int index = 0; index < count; ++index) {
        const QString rawPath = rawPaths.at(index).toString();
        const QString metadataPath = metadataPaths.at(index).toString();
        QFile::remove(rawPath);
        QFile::remove(metadataPath);
    }

    m_previewCaptureRunning = true;
    m_captureClock.start();
    m_pendingRawCapture = true;
    m_pendingRawBracketCapture = true;
    m_pendingRawBracketCount = count;
    m_pendingRawBracketRawPaths = rawPaths;
    m_pendingRawBracketMetadataPaths = metadataPaths;
    m_completedRawBracketRawPaths.clear();
    m_completedRawBracketMetadataPaths.clear();
    for (int index = 0; index < count; ++index) {
        m_completedRawBracketRawPaths.append(QVariant());
        m_completedRawBracketMetadataPaths.append(QVariant());
    }
    m_pendingCapturePath = metadataPaths.first().toString();
    m_pendingCaptureFinalPath = rawPaths.first().toString();
    m_pendingCaptureSize = -1;
    m_pendingCaptureStablePolls = 0;
    // Every frame is written to storage by the bridge; allow for that.
    m_capturePollsRemaining = qMax(1, (m_captureTimeout * 1000 * qMax(1, count / 2))
                                      / m_captureTimer.interval());
    sendExposureSettings(true);
    m_restorePreviewSettingsAfterCapture = true;
    m_restoreExposureOnlySettingsAfterCapture = true;
    if (m_process->write(command) >= 0) {
        m_captureTimer.start();
        return true;
    }

    restorePreviewSettingsAfterCapture();
    m_pendingCapturePath.clear();
    m_pendingCaptureFinalPath.clear();
    m_pendingRawCapture = false;
    m_pendingRawBracketCapture = false;
    m_pendingRawBracketCount = 0;
    m_pendingRawBracketRawPaths.clear();
    m_pendingRawBracketMetadataPaths.clear();
    m_completedRawBracketRawPaths.clear();
    m_completedRawBracketMetadataPaths.clear();
    m_previewCaptureRunning = false;
    setErrorString(QStringLiteral("Could not write Camera2 RAW frame command"));
    return false;
}

void Camera2Preview::savePreviewJpegAsync(const QString &path)
{
    QImage image = displayFrame();
    image.detach();
    const qreal exposure = m_renderExposure;
    const int colorTemperature = m_colorTemperature;
    const int colorTint = m_colorTint;
    const int quality = m_jpegQuality;
    QPointer<Camera2Preview> self(this);

    std::thread([self, path, image, exposure, colorTemperature, colorTint,
                 quality]() mutable {
        QString error;
        if (ImageAdjustments::requested(exposure, colorTemperature, colorTint)) {
            ImageAdjustments::apply(&image, exposure, colorTemperature,
                                    colorTint);
        }
        QImageWriter writer(path, "JPG");
        writer.setQuality(quality);
        const bool success = writer.write(image);
        if (!success) {
            error = QStringLiteral("Could not save Camera2 preview JPEG");
        }
        if (self) {
            QMetaObject::invokeMethod(
                self, "finishPreviewCapture", Qt::QueuedConnection,
                Q_ARG(QString, path), Q_ARG(bool, success),
                Q_ARG(QString, error));
        }
    }).detach();
}

void Camera2Preview::finishPreviewCapture(const QString &path, bool success,
                                          const QString &error)
{
    m_previewCaptureRunning = false;
    restorePreviewSettingsAfterCapture();
    if (success) {
        if (!savePreviewMetadata(path, displaySize())) {
            emit imageCaptureFailed(errorString());
            return;
        }
        emit imageCaptured(path, QStringLiteral("image/jpeg"));
    } else {
        emit imageCaptureFailed(error);
    }
}

bool Camera2Preview::savePreviewMetadata(const QString &path,
                                         const QSize &imageSize)
{
    QSize finalImageSize = imageSize;
    if (m_jpegCaptureEnabled &&
            (m_jpegOrientation == 90 || m_jpegOrientation == 270)) {
        finalImageSize.transpose();
    }
    QJsonObject metadata = m_captureMetadata;
    metadata.insert(QStringLiteral("_original_exif"), QString::fromLatin1(m_originalJpegExif.toBase64()));
    metadata.insert(QStringLiteral("status"), QStringLiteral("ok"));
    metadata.insert(QStringLiteral("capture_source"),
                    m_jpegCaptureEnabled
                    ? QStringLiteral("camera2_preview_jpeg")
                    : QStringLiteral("camera2_preview_frame"));
    metadata.insert(QStringLiteral("camera_id"), m_cameraId);
    metadata.insert(QStringLiteral("jpeg_path"), path);
    metadata.insert(QStringLiteral("width"), finalImageSize.width());
    metadata.insert(QStringLiteral("height"), finalImageSize.height());
    metadata.insert(QStringLiteral("jpeg_quality"), m_jpegQuality);
    metadata.insert(QStringLiteral("jpeg_orientation"), m_jpegOrientation);
    const bool pixelsRotated = true;
    metadata.insert(QStringLiteral("capture_orientation"), m_jpegOrientation);
    metadata.insert(QStringLiteral("pixels_rotated"), pixelsRotated);
    metadata.insert(QStringLiteral("exif_orientation"),
                    pixelsRotated ? 1 : m_jpegOrientation);
    metadata.insert(QStringLiteral("zoom_ratio"), m_zoom);
    metadata.insert(QStringLiteral("focus_mode_requested"), m_focusMode);
    metadata.insert(QStringLiteral("focus_distance_requested_diopters"),
                    m_focusDistance);
    metadata.insert(QStringLiteral("exposure_compensation_requested"),
                    m_exposureCompensation);
    metadata.insert(QStringLiteral("sensor_sensitivity_requested"),
                    m_sensorSensitivity);
    metadata.insert(QStringLiteral("exposure_time_requested_ns"),
                    m_exposureTime);
    metadata.insert(QStringLiteral("aperture_requested"), m_aperture);
    metadata.insert(QStringLiteral("noise_reduction_requested"),
                    m_noiseReduction);
    metadata.insert(QStringLiteral("scene_mode_requested"), m_sceneMode);
    metadata.insert(QStringLiteral("scene_mode_requested_original"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("scene-requested-original"), m_sceneMode));
    metadata.insert(QStringLiteral("scene_mode_supported"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("scene-supported"), QStringLiteral("true")) ==
                    QLatin1String("true"));
    metadata.insert(QStringLiteral("hdr_scene_supported"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("hdr-scene-supported"), QStringLiteral("false")) ==
                    QLatin1String("true"));
    metadata.insert(QStringLiteral("hdr_scene_requested"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("hdr-scene-requested"),
                        m_sceneMode == QLatin1String("hdr") ?
                            QStringLiteral("true") : QStringLiteral("false")) ==
                    QLatin1String("true"));
    metadata.insert(QStringLiteral("hdr_scene_applied"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("hdr-scene-applied"), QStringLiteral("false")) ==
                    QLatin1String("true"));
    metadata.insert(QStringLiteral("dol_supported"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("dol-supported"), QStringLiteral("false")) ==
                    QLatin1String("true"));
    metadata.insert(QStringLiteral("dol_source"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("dol-source"), QStringLiteral("none")));
    metadata.insert(QStringLiteral("dol_requested"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("dol-requested"),
                        m_sceneMode == QLatin1String("hdr") ?
                            QStringLiteral("true") : QStringLiteral("false")) ==
                    QLatin1String("true"));
    metadata.insert(QStringLiteral("dol_applied"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("dol-applied"), QStringLiteral("false")) ==
                    QLatin1String("true"));
    metadata.insert(QStringLiteral("control_mode_applied"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("control-mode-applied"),
                        QStringLiteral("unknown")));
    metadata.insert(QStringLiteral("control_mode_applied_value"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("control-mode-applied-value"),
                        QStringLiteral("-1")).toInt());
    metadata.insert(QStringLiteral("scene_mode_applied"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("scene-mode-applied"),
                        QStringLiteral("unknown")));
    metadata.insert(QStringLiteral("scene_mode_applied_value"),
                    m_lastCaptureResultFields.value(
                        QStringLiteral("scene-mode-applied-value"),
                        QStringLiteral("-1")).toInt());
    metadata.insert(QStringLiteral("render_exposure"), m_renderExposure);
    metadata.insert(QStringLiteral("color_temperature_requested_kelvin"),
                    m_colorTemperature);
    metadata.insert(QStringLiteral("color_tint_requested"), m_colorTint);
    metadata.insert(QStringLiteral("live_sensor_sensitivity"),
                    m_liveSensorSensitivity);
    metadata.insert(QStringLiteral("live_exposure_time_ns"),
                    m_liveExposureTime);
    metadata.insert(QStringLiteral("focal_length_mm"), m_focalLength);
    QJsonObject captureResult;
    for (auto it = m_lastCaptureResultFields.begin(); it != m_lastCaptureResultFields.end(); ++it)
        captureResult.insert(it.key(), it.value());
    metadata.insert(QStringLiteral("capture_result"), captureResult);
    const int capturedIso = m_lastCaptureResultFields.value(QStringLiteral("iso")).toInt();
    if (capturedIso > 0) metadata.insert(QStringLiteral("iso"), capturedIso);
    const QString capturedExposure = m_lastCaptureResultFields.value(QStringLiteral("shutter"));
    if (capturedExposure.toLongLong() > 0) metadata.insert(QStringLiteral("exposure_time_ns"), capturedExposure);


    const QString sidecarPath = jsonSidecarPath(path);
    QDir().mkpath(QFileInfo(sidecarPath).absolutePath());
    QFile file(sidecarPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setErrorString(QStringLiteral("Could not save Camera2 JPEG metadata"));
        return false;
    }
    if (file.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented)) < 0) {
        setErrorString(QStringLiteral("Could not write Camera2 JPEG metadata"));
        return false;
    }
    file.close();
    QString exifError;
    if (!ExifUtils::writeJpegExifFromJsonFile(path, sidecarPath, &exifError)) {
        setErrorString(exifError);
        return false;
    }
    return true;
}

void Camera2Preview::readFrames()
{
    if (!m_process) {
        return;
    }
    m_buffer.append(m_process->readAllStandardOutput());
    parseFrames();
}

void Camera2Preview::readErrors()
{
    if (!m_process) {
        return;
    }
    const QString errors = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
    if (!errors.isEmpty()) {
        const QStringList lines = errors.split(QLatin1Char('\n'),
                                               QString::SkipEmptyParts);
        QStringList realErrors;
        for (const QString &line : lines) {
            if (line.startsWith(QStringLiteral("capture-timing "))) {
                qDebug().noquote() << line;
            } else if (line.startsWith(QStringLiteral("capture-exposure ")) ||
                       line.startsWith(QStringLiteral("capture-effective ")) ||
                       // Recording reports; failures reach the app as record-status.
                       line.startsWith(QStringLiteral("record-")) ||
                       line.startsWith(QStringLiteral("timing ")) ||
                       line.startsWith(QStringLiteral("video-stream="))) {
                qDebug().noquote() << line;
                appendRawCaptureLog(line);
            } else if (isBenignCamera2HelperWarning(line)) {
                qDebug().noquote() << line;
            } else {
                realErrors.append(line);
            }
        }
        for (const QString &line : realErrors)
            appendRawCaptureLog(QStringLiteral("helper ") + line);
        if (!realErrors.isEmpty()) {
            setErrorString(realErrors.join(QLatin1Char('\n')));
        }
    }
}

void Camera2Preview::processFinished(int, QProcess::ExitStatus)
{
    if (!m_process) {
        return;
    }
    m_process->deleteLater();
    m_process = nullptr;
    finishRecording(false, QStringLiteral("Camera stopped while recording"));
    if (m_videoReady) {
        m_videoReady = false;
        emit videoReadyChanged();
    }
    if (m_running) {
        m_running = false;
        emit runningChanged();
        emit jpegCaptureReadyChanged();
        emit rawCaptureReadyChanged();
    }
    if (m_captureTimer.isActive()) {
        m_captureTimer.stop();
        m_pendingCapturePath.clear();
        m_pendingCaptureFinalPath.clear();
        m_pendingRawCapture = false;
        m_pendingRawBracketCapture = false;
        m_pendingRawBracketCount = 0;
        m_pendingRawBracketRawPaths.clear();
        m_pendingRawBracketMetadataPaths.clear();
        m_completedRawBracketRawPaths.clear();
        m_completedRawBracketMetadataPaths.clear();
        m_pendingCaptureSize = -1;
        m_pendingCaptureStablePolls = 0;
        m_previewCaptureRunning = false;
        m_restorePreviewSettingsAfterCapture = false;
        m_restoreExposureOnlySettingsAfterCapture = false;
        emit imageCaptureFailed(QStringLiteral("Camera2 preview stopped during capture"));
    }
    if (m_active) {
        restart();
    }
}

void Camera2Preview::checkCaptureResult()
{
    if (m_pendingCapturePath.isEmpty()) {
        m_captureTimer.stop();
        return;
    }

    if (m_jpegCaptureEnabled || m_pendingRawCapture) {
        --m_capturePollsRemaining;
        if (m_capturePollsRemaining <= 0) {
            int received = 0;
            for (const QVariant &frame : m_completedRawBracketMetadataPaths)
                if (!frame.toString().isEmpty()) ++received;
            const QString timeoutError = m_pendingRawBracketCapture
                    ? QStringLiteral("Camera2 RAW bracket timed out (%1/%2 frames received)")
                        .arg(received).arg(m_pendingRawBracketCount)
                    : QStringLiteral("Camera2 capture timed out");
            qWarning() << timeoutError << "pending" << m_pendingCapturePath;
            m_captureTimer.stop();
            m_pendingCapturePath.clear();
            m_pendingCaptureFinalPath.clear();
            m_pendingRawCapture = false;
            m_pendingRawBracketCapture = false;
            m_pendingRawBracketCount = 0;
            m_pendingRawBracketRawPaths.clear();
            m_pendingRawBracketMetadataPaths.clear();
            m_completedRawBracketRawPaths.clear();
            m_completedRawBracketMetadataPaths.clear();
            m_pendingCaptureSize = -1;
            m_pendingCaptureStablePolls = 0;
            m_previewCaptureRunning = false;
            restorePreviewSettingsAfterCapture();
            emit imageCaptureFailed(timeoutError);
        }
        return;
    }

    const QFileInfo fileInfo(m_pendingCapturePath);
    if (fileInfo.exists() && fileInfo.size() > 0) {
        if (fileInfo.size() != m_pendingCaptureSize) {
            m_pendingCaptureSize = fileInfo.size();
            m_pendingCaptureStablePolls = 0;
            return;
        }
        if (++m_pendingCaptureStablePolls < 1) {
            return;
        }
        QString path = m_pendingCapturePath;
        if (!m_pendingCaptureFinalPath.isEmpty()) {
            QFile::remove(m_pendingCaptureFinalPath);
            if (!QFile::rename(m_pendingCapturePath,
                               m_pendingCaptureFinalPath)) {
                m_pendingCapturePath.clear();
                m_pendingCaptureFinalPath.clear();
                m_pendingRawCapture = false;
                m_pendingCaptureSize = -1;
                m_pendingCaptureStablePolls = 0;
                m_previewCaptureRunning = false;
                m_captureTimer.stop();
                emit imageCaptureFailed(QStringLiteral(
                    "Could not move Camera2 JPEG into place"));
                return;
            }
            path = m_pendingCaptureFinalPath;
        }
        m_pendingCapturePath.clear();
        m_pendingCaptureFinalPath.clear();
        m_pendingRawCapture = false;
        m_pendingCaptureSize = -1;
        m_pendingCaptureStablePolls = 0;
        m_previewCaptureRunning = false;
        m_captureTimer.stop();
        finishJpegCapture(path);
        return;
    }

    --m_capturePollsRemaining;
    if (m_capturePollsRemaining <= 0) {
        m_captureTimer.stop();
        m_pendingCapturePath.clear();
        m_pendingCaptureFinalPath.clear();
        m_pendingCaptureSize = -1;
        m_pendingCaptureStablePolls = 0;
        m_previewCaptureRunning = false;
        restorePreviewSettingsAfterCapture();
        emit imageCaptureFailed(QStringLiteral("Camera2 JPEG capture timed out"));
    }
}

void Camera2Preview::restart()
{
    stop();
    if (m_previewSize.width() <= 0 || m_previewSize.height() <= 0) {
        m_frame = QImage();
        m_yuvFrame.clear();
        m_yuvSize = QSize();
        update();
        setErrorString(QStringLiteral("Preview unavailable"));
    }
    if (m_restartPending)
        return;
    m_restartPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_restartPending = false;
        startPreview();
    });
}

void Camera2Preview::startPreview()
{
    if (!m_active) {
        return;
    }
    if (m_previewSize.width() <= 0 || m_previewSize.height() <= 0)
        return;

    qDebug() << "preview-size" << m_cameraId << m_previewSize;

    const QString program = helperPath();
    if (!QFileInfo(program).isExecutable()) {
        setErrorString(QStringLiteral("Camera2 preview helper is not installed"));
        return;
    }

    m_buffer.clear();
    // Rates cover streaming only, not the camera start-up; a new session
    // (or camera) starts its viewfinder analysis afresh.
    m_motionMeter.reset();
    m_faces.clear();
    m_lastFaces.clear();
    m_subjectFaces.clear();
    m_faceSeen.invalidate();
    m_faceExposure = PreviewAnalysis::FaceExposure();
    m_statsTimer.invalidate();
    m_statsFrames = 0;
    m_statsParseNs = 0;
    const bool videoMode = m_videoSize.isValid() && !m_videoSize.isEmpty();
    if (m_videoReady) {
        m_videoReady = false;
        emit videoReadyChanged();
    }
    m_process = new QProcess(this);
    m_process->setProgram(program);
    QStringList arguments;
    arguments << QStringLiteral("--preview")
              << QStringLiteral("--camera") << m_cameraId
              << QStringLiteral("--size")
              << QStringLiteral("%1x%2").arg(m_previewSize.width()).arg(m_previewSize.height())
              // A recording keeps the viewfinder for hours, beyond 10000 frames.
              << QStringLiteral("--frames") << QLatin1String(videoMode ? "100000000" : "10000")
              << QStringLiteral("--timeout") << QLatin1String(videoMode ? "86400" : "3600")
              << QStringLiteral("--zoom") << QString::number(m_zoom, 'f', 4)
              // I420 frames: half the bytes of RGB, converted on the GPU.
              << QStringLiteral("--preview-format") << QStringLiteral("yuv");
    if (videoMode) {
        arguments << QStringLiteral("--video-size")
                  << QStringLiteral("%1x%2").arg(m_videoSize.width()).arg(m_videoSize.height())
                  << QStringLiteral("--video-fps") << QString::number(m_videoFrameRate)
                  << QStringLiteral("--video-bitrate") << QString::number(m_videoBitrate)
                  << QStringLiteral("--stabilization")
                  << QLatin1String(m_videoStabilization ? "on" : "off")
                  << QStringLiteral("--codec") << m_videoCodec;
    } else if (m_jpegCaptureEnabled) {
        arguments << QStringLiteral("--jpeg-size") << m_captureSize
                  << QStringLiteral("--quality") << QString::number(m_jpegQuality)
                  << QStringLiteral("--orientation") << QStringLiteral("0");
    }
    if (m_rawCaptureEnabled && !videoMode) {
        arguments << QStringLiteral("--raw-size") << rawCaptureSize()
                  << QStringLiteral("--raw-format") << m_rawCaptureFormat;
    }
    if (m_focusX >= 0.0 && m_focusY >= 0.0) {
        arguments << QStringLiteral("--focus-x") << QString::number(m_focusX, 'f', 4)
                  << QStringLiteral("--focus-y") << QString::number(m_focusY, 'f', 4);
    }
    m_process->setArguments(arguments);
    // The helper camera.cpp launched (paused) at process start with the last
    // run's arguments: the same ones now, so it is adopted with its camera
    // already open. Any other arguments: it goes, before this one starts.
    bool adopted = false;
    if (QProcess *early = qobject_cast<QProcess *>(
                qApp->property("procamPreviewPrelaunch").value<QObject *>())) {
        qApp->setProperty("procamPreviewPrelaunch", QVariant());
        if (early->state() != QProcess::NotRunning && early->program() == program
                && early->arguments() == QStringList(arguments) << QStringLiteral("--start-paused")) {
            delete m_process;
            m_process = early;
            m_process->setParent(this);
            adopted = true;
        } else {
            early->kill();
            early->waitForFinished(1000);
            early->deleteLater();
        }
    }
    {
        // For the next launch's early start.
        QSaveFile launch(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                         + QStringLiteral("/preview-launch.args"));
        if (QDir().mkpath(QFileInfo(launch.fileName()).absolutePath())
                && launch.open(QIODevice::WriteOnly)) {
            launch.write((QStringList(program) + arguments).join(QLatin1Char('\n')).toUtf8());
            launch.commit();
        }
    }
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &Camera2Preview::readFrames);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &Camera2Preview::readErrors);
    connect(m_process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &Camera2Preview::processFinished);
    m_rawStreamActive = false;
    m_firstFrameLogged = false;
    m_helperClock.start();
    appendRawCaptureLog(QStringLiteral("timing preview-start camera=%1 since_app_start_ms=%2")
                        .arg(m_cameraId).arg(procamMsSinceProcessStart()));
    if (adopted) {
        m_process->write("resume\n");
        appendRawCaptureLog(QStringLiteral("timing preview-adopted camera=%1 since_app_start_ms=%2")
                            .arg(m_cameraId).arg(procamMsSinceProcessStart()));
        // Status messages it sent while waiting (streams, video stream).
        QTimer::singleShot(0, this, &Camera2Preview::readFrames);
        QTimer::singleShot(0, this, &Camera2Preview::readErrors);
    } else {
        m_process->start();
    }
    if (adopted || m_process->waitForStarted(1000)) {
        m_running = true;
        emit runningChanged();
        emit jpegCaptureReadyChanged();
        emit rawCaptureReadyChanged();
        setErrorString(QString());
        sendSettings();
    } else {
        setErrorString(m_process->errorString());
        m_process->deleteLater();
        m_process = nullptr;
        emit jpegCaptureReadyChanged();
        emit rawCaptureReadyChanged();
    }
}

void Camera2Preview::stop()
{
    if (!m_process) {
        m_buffer.clear();
        return;
    }
    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    if (m_recording && process->state() != QProcess::NotRunning) {
        // quit makes the helper finish the file (its index is written last);
        // killing it would leave an unplayable one.
        process->write(m_recordingStopTimer.isActive() ? "quit\n" : "record-stop\nquit\n");
        const bool finished = process->waitForFinished(4000);
        const QString path = m_recordingPath;
        finishRecording(finished && process->exitStatus() == QProcess::NormalExit
                            && QFileInfo(path).size() > 0,
                        finished ? QString() : QStringLiteral("Recording did not finish"));
    }
    if (m_videoReady) {
        m_videoReady = false;
        emit videoReadyChanged();
    }
    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(500)) {
            process->kill();
            process->waitForFinished(500);
        }
    }
    process->deleteLater();
    m_buffer.clear();
    m_captureTimer.stop();
    m_pendingCapturePath.clear();
    m_pendingCaptureFinalPath.clear();
    m_pendingRawCapture = false;
    m_pendingRawBracketCapture = false;
    m_pendingRawBracketCount = 0;
    m_pendingRawBracketRawPaths.clear();
    m_pendingRawBracketMetadataPaths.clear();
    m_completedRawBracketRawPaths.clear();
    m_completedRawBracketMetadataPaths.clear();
    m_pendingCaptureSize = -1;
    m_pendingCaptureStablePolls = 0;
    m_previewCaptureRunning = false;
    if (m_running) {
        m_running = false;
        emit runningChanged();
        emit jpegCaptureReadyChanged();
        emit rawCaptureReadyChanged();
    }
}

void Camera2Preview::sendSettings()
{
    sendSettings(false);
}

void Camera2Preview::sendSettings(bool captureExposure)
{
    if (!m_process || m_process->state() != QProcess::Running) {
        return;
    }

    Q_UNUSED(captureExposure)
    const int sensorSensitivity = m_sensorSensitivity;
    const QString exposureTime = m_exposureTime;

    m_process->write(QStringLiteral("settings %1 %2 %3 %4 %5 %6 %7 %8 %9 %10 %11\n")
                     .arg(m_focusMode)
                     .arg(m_focusDistance, 0, 'f', 4)
                     .arg(m_exposureCompensation)
                     .arg(m_sceneMode)
                     .arg(m_colorTemperature)
                     .arg(m_colorTint)
                     .arg(sensorSensitivity)
                     .arg(exposureTime)
                     .arg(m_aperture)
                     .arg(m_noiseReduction)
                     .arg(m_zoom, 0, 'f', 4)
                     .toLocal8Bit());
}

void Camera2Preview::sendExposureSettings(bool captureExposure)
{
    if (!m_process || m_process->state() != QProcess::Running) {
        return;
    }

    Q_UNUSED(captureExposure)
    const int sensorSensitivity = m_sensorSensitivity;
    const QString exposureTime = m_exposureTime;

    m_process->write(QStringLiteral("exposure-settings %1 %2 %3 %4 %5\n")
                     .arg(sensorSensitivity)
                     .arg(exposureTime)
                     .arg(m_aperture)
                     .arg(m_noiseReduction)
                     .arg(m_zoom, 0, 'f', 4)
                     .toLocal8Bit());
}

void Camera2Preview::restorePreviewSettingsAfterCapture()
{
    if (!m_restorePreviewSettingsAfterCapture) {
        return;
    }

    const bool exposureOnly = m_restoreExposureOnlySettingsAfterCapture;
    m_restorePreviewSettingsAfterCapture = false;
    m_restoreExposureOnlySettingsAfterCapture = false;
    if (exposureOnly) {
        sendExposureSettings(false);
    } else {
        sendSettings(false);
    }
}

void Camera2Preview::parseFrames()
{
    // Walks the buffer once: every metadata packet is applied, but only the
    // newest complete frame is decoded. After the GUI thread stalls (screen
    // rotation relayout), older buffered frames are skipped instead of being
    // histogrammed one by one while the viewfinder lags behind.
    // Frames come as SF2Y (I420, the default) or SF2P (RGB888).
    static const QByteArray rgbMagic("SF2P", 4);
    static const QByteArray yuvMagic("SF2Y", 4);
    static const QByteArray metadataMagic("SF2M", 4);
    int pos = 0;
    int latestOffset = -1, latestWidth = 0, latestHeight = 0, latestBytes = 0;
    bool latestYuv = false;
    auto startsWithAt = [this](const QByteArray &magic, int at) {
        return m_buffer.size() - at >= magic.size()
                && memcmp(m_buffer.constData() + at, magic.constData(), size_t(magic.size())) == 0;
    };
    while (true) {
        if (!startsWithAt(rgbMagic, pos) && !startsWithAt(yuvMagic, pos)
                && !startsWithAt(metadataMagic, pos)) {
            int marker = -1;
            for (const QByteArray *magic : {&rgbMagic, &yuvMagic, &metadataMagic}) {
                const int found = m_buffer.indexOf(*magic, pos);
                if (found >= 0 && (marker < 0 || found < marker)) marker = found;
            }
            if (marker < 0) {
                // Keep a possible partial magic at the end.
                pos = qMax(pos, m_buffer.size() - (rgbMagic.size() - 1));
                break;
            }
            pos = marker;
        }

        if (startsWithAt(metadataMagic, pos)) {
            const int headerBytes = 8;
            if (m_buffer.size() - pos < headerBytes) {
                break;
            }
            const quint32 payloadBytesValue = readLe32(m_buffer.constData() + pos + 4);
            if (payloadBytesValue == 0 || payloadBytesValue > 8192) {
                pos += metadataMagic.size();
                continue;
            }
            const int payloadBytes = int(payloadBytesValue);
            if (m_buffer.size() - pos < headerBytes + payloadBytes) {
                break;
            }
            parseMetadata(m_buffer.mid(pos + headerBytes, payloadBytes));
            pos += headerBytes + payloadBytes;
            if (m_buffer.size() < pos) {
                // A signal handler restarted the bridge and cleared the buffer.
                return;
            }
            continue;
        }

        const int headerBytes = 16;
        if (m_buffer.size() - pos < headerBytes) {
            break;
        }

        const bool yuv = startsWithAt(yuvMagic, pos);
        const quint32 widthValue = readLe32(m_buffer.constData() + pos + 4);
        const quint32 heightValue = readLe32(m_buffer.constData() + pos + 8);
        const quint32 frameBytesValue = readLe32(m_buffer.constData() + pos + 12);
        const quint64 pixels = quint64(widthValue) * quint64(heightValue);
        const quint64 expectedBytes = yuv
                ? pixels + 2 * (quint64(widthValue + 1) / 2) * (quint64(heightValue + 1) / 2)
                : pixels * 3;
        if (widthValue == 0 || heightValue == 0 || frameBytesValue == 0 ||
                widthValue > 16384 || heightValue > 16384 ||
                expectedBytes > quint64(INT_MAX) ||
                frameBytesValue != expectedBytes) {
            pos += rgbMagic.size();
            continue;
        }
        const int frameBytes = int(frameBytesValue);
        if (m_buffer.size() - pos < headerBytes + frameBytes) {
            break;
        }
        latestOffset = pos + headerBytes;
        latestWidth = int(widthValue);
        latestHeight = int(heightValue);
        latestBytes = frameBytes;
        latestYuv = yuv;
        pos += headerBytes + frameBytes;
    }

    const bool newFrame = latestOffset >= 0;
    if (newFrame) {
        QElapsedTimer parseTimer;
        parseTimer.start();
        const QSize previous = frameSize();
        const bool wasPlaceholder = m_frameDisplayOriented;
        // Kept in sensor orientation: the GPU rotates, mirrors and (for
        // YUV) converts it. m_buffer is only compacted below, so the offset
        // is still valid.
        if (latestYuv) {
            m_yuvFrame = m_buffer.mid(latestOffset, latestBytes);
            m_yuvSize = QSize(latestWidth, latestHeight);
            m_frame = QImage();
        } else {
            m_frame = QImage(reinterpret_cast<const uchar *>(m_buffer.constData() + latestOffset),
                             latestWidth, latestHeight, latestWidth * 3,
                             QImage::Format_RGB888).copy();
            m_yuvFrame.clear();
            m_yuvSize = QSize();
        }
        m_frameDisplayOriented = false;
        ++m_frameSerial;
        // Viewfinder analysis for the burst plan, on luma in sensor
        // orientation (the Y plane, or green of RGB).
        PreviewAnalysis::LumaView luma;
        if (latestYuv) {
            luma = PreviewAnalysis::LumaView{reinterpret_cast<const uint8_t *>(m_yuvFrame.constData()),
                                             latestWidth, latestHeight, latestWidth, 1};
        } else {
            luma = PreviewAnalysis::LumaView{m_frame.constBits() + 1, m_frame.width(), m_frame.height(),
                                             int(m_frame.bytesPerLine()), 3};
        }
        if (!m_motionClock.isValid()) m_motionClock.start();
        m_motionMeter.update(luma, m_motionClock.elapsed());
        // The histogram (exposure readouts, burst planning) need not follow
        // every frame.
        if (!m_histogramTimer.isValid() || m_histogramTimer.elapsed() >= 150) {
            if (latestYuv) {
                updateHistogramYuv();
            } else {
                updateHistogram(m_frame);
            }
            std::array<float, 4> subject;
            m_subjectFaces.clear();
            if (m_selfieCamera && PreviewAnalysis::backlitSubject(luma, &subject))
                m_subjectFaces.push_back(subject);
            m_faceExposure = PreviewAnalysis::faceExposure(luma, effectiveFaces());
            m_histogramTimer.restart();
            emit sceneChanged();
        }
        if (frameSize() != previous || wasPlaceholder) {
            emit contentRectChanged();
        }
        if (!m_firstFrameLogged && m_helperClock.isValid()) {
            m_firstFrameLogged = true;
            appendRawCaptureLog(QStringLiteral("timing first-frame camera=%1 since_preview_start_ms=%2 since_app_start_ms=%3")
                                .arg(m_cameraId).arg(m_helperClock.elapsed())
                                .arg(procamMsSinceProcessStart()));
        }
        ++m_statsFrames;
        m_statsParseNs += parseTimer.nsecsElapsed();
        if (!m_statsTimer.isValid()) {
            m_statsTimer.start();
        } else if (m_statsTimer.elapsed() >= 15000) {
            appendRawCaptureLog(QStringLiteral("preview-stats camera=%1 size=%2x%3 format=%4 fps=%5 parse_ms=%6")
                                .arg(m_cameraId).arg(latestWidth).arg(latestHeight)
                                .arg(latestYuv ? QStringLiteral("yuv") : QStringLiteral("rgb"))
                                .arg(m_statsFrames * 1000.0 / m_statsTimer.elapsed(), 0, 'f', 1)
                                .arg(m_statsParseNs / 1e6 / qMax(1, m_statsFrames), 0, 'f', 1));
            m_statsTimer.restart();
            m_statsFrames = 0;
            m_statsParseNs = 0;
        }
    }
    if (pos > 0) {
        m_buffer.remove(0, qMin(pos, m_buffer.size()));
    }
    if (newFrame) {
        update();
    }
}

// Full-range BT.601, as the bridge's own RGB conversion and the GPU shader.
static inline void yuvToRgb(int y, int u, int v, uchar *rgb)
{
    u -= 128;
    v -= 128;
    rgb[0] = uchar(qBound(0, y + ((91881 * v) >> 16), 255));
    rgb[1] = uchar(qBound(0, y - ((22554 * u + 46802 * v) >> 16), 255));
    rgb[2] = uchar(qBound(0, y + ((116130 * u) >> 16), 255));
}

std::vector<std::array<float, 4>> Camera2Preview::effectiveFaces(QString *source) const
{
    if (!m_faces.empty()) {
        if (source) *source = QStringLiteral("camera");
        return m_faces;
    }
    if (!m_lastFaces.empty() && m_faceSeen.isValid() && m_faceSeen.elapsed() < 1500) {
        if (source) *source = QStringLiteral("recent");
        return m_lastFaces;
    }
    if (!m_subjectFaces.empty()) {
        if (source) *source = QStringLiteral("backlit");
        return m_subjectFaces;
    }
    if (source) *source = QStringLiteral("none");
    return {};
}

QVariantList Camera2Preview::faces() const
{
    QVariantList list;
    for (const std::array<float, 4> &face : effectiveFaces())
        list.append(QVariant(QVariantList{face[0], face[1], face[2], face[3]}));
    return list;
}

QVariantMap Camera2Preview::sceneInfo() const
{
    QString source;
    const bool face = !effectiveFaces(&source).empty();
    QVariantMap info;
    info.insert(QStringLiteral("motion"), m_motionMeter.motion());
    info.insert(QStringLiteral("subjectMotion"), m_motionMeter.subjectMotion());
    info.insert(QStringLiteral("faceCount"), face ? int(effectiveFaces().size()) : 0);
    info.insert(QStringLiteral("faceSource"), source);
    info.insert(QStringLiteral("faceLuma"), face ? m_faceExposure.luma : -1.0);
    info.insert(QStringLiteral("faceClipped"), face ? m_faceExposure.clipped : 0.0);
    return info;
}

bool Camera2Preview::hasFrame() const
{
    return !m_frame.isNull() || !m_yuvFrame.isEmpty();
}

QSize Camera2Preview::frameSize() const
{
    return m_yuvFrame.isEmpty() ? m_frame.size() : m_yuvSize;
}

QImage Camera2Preview::rgbFrame() const
{
    if (m_yuvFrame.isEmpty()) {
        return m_frame;
    }
    // Only on demand (thumbnail, preview capture, Pro's preview aids): the
    // viewfinder itself converts on the GPU.
    const int width = m_yuvSize.width(), height = m_yuvSize.height();
    const int chromaWidth = (width + 1) / 2, chromaHeight = (height + 1) / 2;
    const uchar *luma = reinterpret_cast<const uchar *>(m_yuvFrame.constData());
    const uchar *u = luma + width * height;
    const uchar *v = u + chromaWidth * chromaHeight;
    QImage image(width, height, QImage::Format_RGB888);
    if (image.isNull()) {
        return image;
    }
    for (int y = 0; y < height; ++y) {
        uchar *line = image.scanLine(y);
        const uchar *lumaRow = luma + y * width;
        const uchar *uRow = u + (y / 2) * chromaWidth, *vRow = v + (y / 2) * chromaWidth;
        for (int x = 0; x < width; ++x)
            yuvToRgb(lumaRow[x], uRow[x / 2], vRow[x / 2], line + 3 * x);
    }
    return image;
}

void Camera2Preview::updateHistogramYuv()
{
    // A sparse grid converted to RGB, as updateHistogram() samples.
    const int width = m_yuvSize.width(), height = m_yuvSize.height();
    const int chromaWidth = (width + 1) / 2, chromaHeight = (height + 1) / 2;
    const uchar *luma = reinterpret_cast<const uchar *>(m_yuvFrame.constData());
    const uchar *u = luma + width * height;
    const uchar *v = u + chromaWidth * chromaHeight;
    const int step = qMax(1, qMin(width, height) / 160);
    QImage samples((width + step - 1) / step, (height + step - 1) / step, QImage::Format_RGB888);
    if (samples.isNull()) {
        return;
    }
    for (int y = 0, sy = 0; y < height; y += step, ++sy) {
        uchar *line = samples.scanLine(sy);
        for (int x = 0, sx = 0; x < width; x += step, ++sx)
            yuvToRgb(luma[y * width + x], u[(y / 2) * chromaWidth + x / 2],
                     v[(y / 2) * chromaWidth + x / 2], line + 3 * sx);
    }
    updateHistogram(samples);
}

void Camera2Preview::updateHistogram(const QImage &frame)
{
    if (frame.isNull()) {
        return;
    }

    int redBins[HistogramBins] = {};
    int greenBins[HistogramBins] = {};
    int blueBins[HistogramBins] = {};
    const qreal gain = 1.0; // Histogram describes the actual preview frame.
    const int step = qMax(1, qMin(frame.width(), frame.height()) / 160);
    for (int y = 0; y < frame.height(); y += step) {
        const uchar *line = frame.constScanLine(y);
        for (int x = 0; x < frame.width(); x += step) {
            const uchar *pixel = line + x * 3;
            const int red = qBound(0, int(std::round(pixel[0] * gain)), 255);
            const int green = qBound(0, int(std::round(pixel[1] * gain)), 255);
            const int blue = qBound(0, int(std::round(pixel[2] * gain)), 255);
            ++redBins[qBound(0, red * HistogramBins / 256, HistogramBins - 1)];
            ++greenBins[qBound(0, green * HistogramBins / 256, HistogramBins - 1)];
            ++blueBins[qBound(0, blue * HistogramBins / 256, HistogramBins - 1)];
        }
    }

    QVariantList histogram;
    histogram.reserve(HistogramBins);
    for (int index = 0; index < HistogramBins; ++index) {
        QVariantMap bin;
        bin.insert(QStringLiteral("r"), redBins[index]);
        bin.insert(QStringLiteral("g"), greenBins[index]);
        bin.insert(QStringLiteral("b"), blueBins[index]);
        histogram.append(bin);
    }

    if (m_histogram != histogram) {
        m_histogram = histogram;
        emit histogramChanged();
    }
}

void Camera2Preview::parseMetadata(const QByteArray &payload)
{
    const QStringList fields = QString::fromLatin1(payload).simplified().split(QLatin1Char(' '));
    QMap<QString, QString> parsedFields;
    for (const QString &field : fields) {
        const int separator = field.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            continue;
        }

        const QString key = field.left(separator);
        const QString value = field.mid(separator + 1);
        parsedFields.insert(key, value);
        if (key == QLatin1String("focal")) {
            bool ok = false;
            const qreal focal = value.toDouble(&ok);
            if (ok && !qFuzzyCompare(m_focalLength, focal)) {
                m_focalLength = focal;
                emit focalLengthChanged();
            }
        } else if (key == QLatin1String("iso")) {
            bool ok = false;
            const int iso = value.toInt(&ok);
            if (ok && m_liveSensorSensitivity != iso) {
                m_liveSensorSensitivity = iso;
                emit liveSensorSensitivityChanged();
            }
        } else if (key == QLatin1String("shutter")) {
            bool ok = false;
            const qlonglong shutter = value.toLongLong(&ok);
            const QString shutterText = ok && shutter > 0
                    ? QString::number(shutter) : QStringLiteral("0");
            if (m_liveExposureTime != shutterText) {
                m_liveExposureTime = shutterText;
                emit liveExposureTimeChanged();
            }
        }
    }
    if (parsedFields.value(QStringLiteral("streams")) == QLatin1String("1")) {
        const bool raw = parsedFields.value(QStringLiteral("raw")) == QLatin1String("1");
        const QString status = QStringLiteral("camera=%1 raw=%2 raw_error=%3 raw_size=%4 raw_format=%5")
                .arg(m_cameraId, raw ? QStringLiteral("1") : QStringLiteral("0"),
                     parsedFields.value(QStringLiteral("raw_error"), QStringLiteral("unknown")),
                     parsedFields.value(QStringLiteral("raw_size")),
                     parsedFields.value(QStringLiteral("raw_format")));
        if (m_rawCaptureEnabled)
            appendRawCaptureLog(QStringLiteral("preview-streams ") + status);
        if (m_rawStreamActive != raw || m_rawStreamStatus != status) {
            m_rawStreamActive = raw;
            m_rawStreamStatus = status;
            emit rawCaptureReadyChanged();
        }
    }
    if (parsedFields.contains(QStringLiteral("faces"))) {
        // rects=l,t,r,b,score;... normalised on the full sensor frame, or "-".
        std::vector<std::array<float, 4>> faces;
        const QString rects = parsedFields.value(QStringLiteral("rects"));
        for (const QString &face : rects.split(QLatin1Char(';'), QString::SkipEmptyParts)) {
            const QStringList values = face.split(QLatin1Char(','));
            if (values.size() < 4) continue;
            std::array<float, 4> box;
            for (int k = 0; k < 4; ++k) box[k] = qBound(0.0f, values[k].toFloat(), 1.0f);
            if (box[2] > box[0] && box[3] > box[1]) faces.push_back(box);
        }
        if (!faces.empty()) {
            m_lastFaces = faces;
            m_faceSeen.restart();
        }
        if (faces != m_faces) {
            m_faces = faces;
            emit sceneChanged();
        }
    }
    if (parsedFields.value(QStringLiteral("meter")) == QLatin1String("1")) {
        QVariantMap sample;
        for (auto it = parsedFields.constBegin(); it != parsedFields.constEnd(); ++it)
            sample.insert(it.key(), it.value());
        emit exposureSample(sample);
    }
    if (parsedFields.contains(QStringLiteral("capture-status"))) {
        parseCaptureResult(parsedFields);
    }
    if (parsedFields.contains(QStringLiteral("capture-started"))
            && (m_pendingRawCapture || m_pendingRawBracketCapture)) {
        appendRawCaptureLog(QStringLiteral("timing capture-started since_command_ms=%1")
                            .arg(m_captureClock.isValid() ? m_captureClock.elapsed() : -1));
        emit captureStarted();
    }
    // The last RAW frame of a capture is exposed; its files come later.
    if (parsedFields.contains(QStringLiteral("capture-exposed"))
            && (m_pendingRawCapture || m_pendingRawBracketCapture)) {
        appendRawCaptureLog(QStringLiteral("timing capture-exposed frames=%1 since_command_ms=%2")
                            .arg(parsedFields.value(QStringLiteral("frames")))
                            .arg(m_captureClock.isValid() ? m_captureClock.elapsed() : -1));
        emit captureExposed();
    }
    if (parsedFields.contains(QStringLiteral("video-stream"))) {
        const bool ready = parsedFields.value(QStringLiteral("video-stream")) == QLatin1String("1");
        qDebug() << "video-stream" << QString::fromLatin1(payload).trimmed();
        if (m_videoReady != ready) {
            m_videoReady = ready;
            emit videoReadyChanged();
        }
        if (!ready) {
            appendRawCaptureLog(QStringLiteral("video-stream unavailable camera=%1 error=%2")
                                .arg(m_cameraId, parsedFields.value(QStringLiteral("error"))));
            emit videoStreamUnavailable(parsedFields.value(QStringLiteral("error")));
        }
    }
    if (parsedFields.contains(QStringLiteral("record-status"))) {
        parseRecordStatus(payload, parsedFields);
    }
}

void Camera2Preview::parseCaptureResult(const QMap<QString, QString> &fields)
{
    if (!m_previewCaptureRunning) {
        return;
    }

    const QString status = fields.value(QStringLiteral("capture-status"));
    const bool commandError = status == QLatin1String("command-error");
    const QString path = commandError ? m_pendingCapturePath
                                     : fields.value(QStringLiteral("path"), m_pendingCapturePath);
    const bool rawCapture = m_pendingRawCapture;
    const bool rawBracketCapture = m_pendingRawBracketCapture;
    const QString rawPath = m_pendingCaptureFinalPath;
    m_lastCaptureResultFields = fields;
    if (status != QLatin1String("ok")) {
        QStringList details;
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
            details.append(it.key() + QLatin1Char('=') + it.value());
        appendRawCaptureLog(QStringLiteral("capture-result ") + details.join(QLatin1Char(' ')));
    }

    if (rawBracketCapture) {
        const int metadataIndex = m_pendingRawBracketMetadataPaths.indexOf(path);
        if (metadataIndex < 0) return; // Stale event from a different capture.
        if (status != QLatin1String("ok")) {
            m_captureTimer.stop();
            m_pendingCapturePath.clear();
            m_pendingCaptureFinalPath.clear();
            m_pendingRawCapture = false;
            m_pendingRawBracketCapture = false;
            m_pendingRawBracketCount = 0;
            m_pendingRawBracketRawPaths.clear();
            m_pendingRawBracketMetadataPaths.clear();
            m_completedRawBracketRawPaths.clear();
            m_completedRawBracketMetadataPaths.clear();
            m_pendingCaptureSize = -1;
            m_pendingCaptureStablePolls = 0;
            m_previewCaptureRunning = false;
            restorePreviewSettingsAfterCapture();
            emit imageCaptureFailed(commandError
                    ? QStringLiteral("Camera2 rejected capture command (code %1)").arg(fields.value(QStringLiteral("code")))
                    : QStringLiteral("Camera2 warm RAW bracket failed"));
            return;
        }

        QJsonObject frameContext = m_captureMetadata;
        frameContext.insert(QStringLiteral("bracket_index"), metadataIndex);
        frameContext.insert(QStringLiteral("bracket_count"), m_pendingRawBracketCount);
        QString metadataError;
        if (!ExifUtils::enrichSidecar(path, frameContext, &metadataError)) {
            stop();
            emit imageCaptureFailed(metadataError);
            return;
        }
        if (metadataIndex >= 0 &&
                metadataIndex < m_pendingRawBracketRawPaths.count()) {
            m_completedRawBracketRawPaths.replace(
                        metadataIndex,
                        m_pendingRawBracketRawPaths.at(metadataIndex));
            m_completedRawBracketMetadataPaths.replace(metadataIndex, path);
        }
        int completedCount = 0;
        for (const QVariant &metadataPath : m_completedRawBracketMetadataPaths) {
            if (!metadataPath.toString().isEmpty()) {
                ++completedCount;
            }
        }
        if (completedCount < m_pendingRawBracketCount) {
            return;
        }

        const QVariantList rawPaths = m_completedRawBracketRawPaths;
        const QVariantList metadataPaths = m_completedRawBracketMetadataPaths;
        m_captureTimer.stop();
        m_pendingCapturePath.clear();
        m_pendingCaptureFinalPath.clear();
        m_pendingRawCapture = false;
        m_pendingRawBracketCapture = false;
        m_pendingRawBracketCount = 0;
        m_pendingRawBracketRawPaths.clear();
        m_pendingRawBracketMetadataPaths.clear();
        m_completedRawBracketRawPaths.clear();
        m_completedRawBracketMetadataPaths.clear();
        m_pendingCaptureSize = -1;
        m_pendingCaptureStablePolls = 0;
        m_previewCaptureRunning = false;
        restorePreviewSettingsAfterCapture();
        appendRawCaptureLog(QStringLiteral("timing capture-files frames=%1 since_command_ms=%2")
                            .arg(rawPaths.size())
                            .arg(m_captureClock.isValid() ? m_captureClock.elapsed() : -1));
        emit rawBracketReady(rawPaths, metadataPaths);
        return;
    }

    m_captureTimer.stop();
    m_pendingCapturePath.clear();
    m_pendingCaptureFinalPath.clear();
    m_pendingRawCapture = false;
    m_pendingRawBracketCapture = false;
    m_pendingRawBracketCount = 0;
    m_pendingRawBracketRawPaths.clear();
    m_pendingRawBracketMetadataPaths.clear();
    m_completedRawBracketRawPaths.clear();
    m_completedRawBracketMetadataPaths.clear();
    m_pendingCaptureSize = -1;
    m_pendingCaptureStablePolls = 0;
    m_previewCaptureRunning = false;
    restorePreviewSettingsAfterCapture();

    if (status == QLatin1String("ok")) {
        if (rawCapture) {
            QString metadataError;
            if (!ExifUtils::enrichSidecar(path, m_captureMetadata, &metadataError)) {
                emit imageCaptureFailed(metadataError);
                return;
            }
            emit rawImageReady(rawPath, path);
            return;
        }
        finishJpegCapture(path);
    } else {
        emit imageCaptureFailed(commandError
                ? QStringLiteral("Camera2 rejected capture command (code %1)").arg(fields.value(QStringLiteral("code")))
                : QStringLiteral("Camera2 warm capture failed"));
    }
}

void Camera2Preview::finishJpegCapture(const QString &path)
{
    // The JPEG is on disk: feedback now, then rotate/adjust/re-encode
    // (seconds for large sizes) off the GUI thread.
    emit captureExposed();
    m_originalJpegExif = ExifUtils::originalJpegExif(path);
    const int orientation = m_jpegOrientation, quality = m_jpegQuality;
    const bool mirror = m_captureMirror;
    const qreal exposure = m_renderExposure;
    const int temperature = m_colorTemperature, tint = m_colorTint;
    const QString size = m_captureSize;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, path, size]() {
        const QString rewriteError = watcher->result();
        watcher->deleteLater();
        if (!rewriteError.isEmpty()) {
            emit imageCaptureFailed(rewriteError);
            return;
        }
        if (!savePreviewMetadata(path, sizeFromString(size))) {
            emit imageCaptureFailed(errorString());
            return;
        }
        emit imageCaptured(path, QStringLiteral("image/jpeg"));
    });
    watcher->setFuture(QtConcurrent::run([=]() {
        QString error;
        if (!rewriteJpegPixels(path, orientation, mirror, exposure, temperature, tint, quality, &error))
            return error.isEmpty() ? QStringLiteral("Could not rewrite Camera2 JPEG") : error;
        return QString();
    }));
}

void Camera2Preview::setErrorString(const QString &errorString)
{
    if (m_errorString == errorString) {
        return;
    }
    m_errorString = errorString;
    emit errorStringChanged();
}

QString Camera2Preview::helperPath() const
{
    const QString override = QString::fromLocal8Bit(qgetenv("SFOS_CAMERA2_PROBE"));
    return override.isEmpty()
            ? QStringLiteral("/usr/libexec/procam/sfos-camera2-probe")
            : override;
}
