/*
 * SPDX-FileCopyrightText: 2013 - 2014 Jolla Ltd.
 * SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef DECLARATIVECAMERAEXTENSIONS_H
#define DECLARATIVECAMERAEXTENSIONS_H

#include <QProcess>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QTemporaryDir>
#include <QList>
#include <QThreadPool>
#include "rawpipeline.h"
#include <QStringList>
#include <QVariantList>
#include <QJsonObject>
#include <QSize>

class QTemporaryDir;

namespace RawPipeline { struct RenderSettings; }

class DeclarativeCameraExtensions : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool rawImageCaptureAvailable READ rawImageCaptureAvailable CONSTANT)
    // Bursts captured but still being merged (queued renders).
    Q_PROPERTY(int pendingRenders READ pendingRenders NOTIFY pendingRendersChanged)
    // Keep every frame of a burst (RAW16 or YUV + JSON sidecar), not just
    // the first: for diagnostics and training data.
    Q_PROPERTY(bool keepBurstFrames MEMBER m_keepBurstFrames NOTIFY keepBurstFramesChanged)

public:
    DeclarativeCameraExtensions(QObject *parent = nullptr);
    ~DeclarativeCameraExtensions();

    bool rawImageCaptureAvailable() const;

    Q_INVOKABLE void disableNotifications(QQuickItem *item, bool disable);
    Q_INVOKABLE QString camera2DeviceConfigDir() const;
    Q_INVOKABLE QString ensureCamera2GeneratedHalConfig(const QString &cameraId);
    Q_INVOKABLE QStringList camera2RawSizeModel(const QString &cameraId,
                                                const QString &rawFormat);
    Q_INVOKABLE QStringList camera2JpegSizeModel(const QString &cameraId);
    Q_INVOKABLE QSize camera2PreferredPreviewSize(const QString &cameraId);
    // A 16:9 viewfinder for video recorded from the Camera2 preview.
    Q_INVOKABLE QSize camera2VideoPreviewSize(const QString &cameraId);
    // Video sizes ("1080p", "1440p", "2160p") that both the camera (at
    // 30 fps) and the hardware H.264 encoder take; 1080p at least.
    Q_INVOKABLE QStringList camera2VideoResolutions(const QString &cameraId);
    Q_INVOKABLE int camera2PreviewOrientation(const QString &cameraId, int fallback);
    Q_INVOKABLE QVariantMap camera2SimpleModeOverrides(const QString &cameraId);
    Q_INVOKABLE bool camera2PreviewMirror(const QString &cameraId, bool fallback);
    Q_INVOKABLE QStringList camera2CaptureFormatModel(const QString &cameraId);
    Q_INVOKABLE QStringList camera2LensModel(const QString &cameraId);
    Q_INVOKABLE QString camera2LensLabel(const QString &cameraId);
    // "front", "back", "external" or "unknown" (older bridge / HAL).
    Q_INVOKABLE QString camera2LensFacing(const QString &cameraId);
    // Camera2 ids by role: {front, main, ultrawide, tele}; "" where there is
    // none. teleZoom: the telephoto's magnification over the main camera.
    Q_INVOKABLE QVariantMap camera2CameraRoles();
    // Burst input for HDR: "raw16", "yuv" (cameras without RAW) or "".
    Q_INVOKABLE QString camera2BurstFormat(const QString &cameraId);
    Q_INVOKABLE QStringList camera2FocusModeModel(const QString &cameraId);
    Q_INVOKABLE QStringList camera2SceneModel(const QString &cameraId);
    Q_INVOKABLE QVariantList camera2NoiseReductionModel(const QString &cameraId);
    Q_INVOKABLE QVariantList camera2IsoModel(const QString &cameraId);
    Q_INVOKABLE QStringList camera2BracketModel(const QString &cameraId);
    // Burst exposure plan for the metered exposure; keys frames, shutterNs,
    // iso, targetGain, underexposureEv (empty map when unsupported).
    // HDR burst needs manual ISO and shutter and a RAW16 burst size.
    Q_INVOKABLE bool camera2BurstSupported(const QString &cameraId);
    // RAW16 size for the burst stream: `preferred` if it can stream next to
    // the preview, else the largest such size (empty if none).
    Q_INVOKABLE QString camera2BurstRawSize(const QString &cameraId, const QString &preferred);
    // Adds a line to Pictures/ProCam/procam-capture.log (capture diagnostics
    // from QML: refused or stalled shutter presses).
    Q_INVOKABLE void logCaptureEvent(const QString &message);
    // True when gst-droid's quirk file turns on Android video stabilization
    // (/etc/gst-droid/gstdroidcamsrcquirks.conf, see README).
    Q_INVOKABLE bool videoStabilizationConfigured() const;
    // night: night mode (long frames held still, see RawPipeline::planBurst).
    // scene: Camera2Preview::sceneInfo() (motion, faces) or empty.
    Q_INVOKABLE QVariantMap camera2BurstPlan(const QString &cameraId,
                                             const QString &meteredShutterNs,
                                             int meteredIso,
                                             const QVariantList &histogram,
                                             bool night = false,
                                             const QVariantMap &scene = QVariantMap());
    Q_INVOKABLE QString camera2MaxShutterNs(const QString &cameraId);
    Q_INVOKABLE QVariantMap camera2ExposureCapabilities(const QString &cameraId);
    Q_INVOKABLE void setCaptureCompensation(int steps) { m_captureCompensation = steps; }
    Q_INVOKABLE QString camera2BracketShutterNs(const QString &cameraId, const QString &base, int ev);
    Q_INVOKABLE QStringList camera2ShutterModel(const QString &cameraId);
    Q_INVOKABLE QStringList camera2FocusDistanceModel(const QString &cameraId);
    Q_INVOKABLE qreal camera2MaximumZoom(const QString &cameraId);
    Q_INVOKABLE QStringList rawCaptureExposureModel(const QString &cameraId);
    Q_INVOKABLE QVariantList rawCaptureRotationModel(const QString &cameraId,
                                                     const QString &mode);
    Q_INVOKABLE QVariantList rawCaptureFocusTimeoutModel(const QString &cameraId);
    Q_INVOKABLE QStringList rawCaptureRawFormatModel(const QString &cameraId);
    Q_INVOKABLE bool raw10CaptureAvailable(const QString &cameraId);
    Q_INVOKABLE QString camera2PreferredCaptureSize(const QString &cameraId,
                                                    const QString &format);
    Q_INVOKABLE QString camera2WarmCaptureSize(const QString &cameraId);
    // Whether RAW16 at `size` may stream next to the preview (warm RAW and
    // bursts). Independent of the JPEG warm size.
    Q_INVOKABLE bool camera2WarmRawCaptureSupported(const QString &cameraId,
                                                    const QString &size);
    // Whether JPEG at `size` can be captured inside the running preview.
    Q_INVOKABLE bool camera2WarmJpegCaptureSupported(const QString &cameraId,
                                                     const QString &size);
    Q_INVOKABLE QString preferredCamera2CameraId(const QString &cameraId);
    Q_INVOKABLE QString camera2CompatibilityLevel(const QString &cameraId);
    Q_INVOKABLE QString camera2CompatibilitySummary(const QString &cameraId);
    Q_INVOKABLE QString exportCamera2CompatibilityReport(const QString &cameraId);
    Q_INVOKABLE bool finalizeImageMetadata(const QString &path);
    Q_INVOKABLE void setCaptureMetadata(const QVariantMap &metadata);
    Q_INVOKABLE void setNextCaptureBracketMetadata(int index, int count,
                                                   qreal ev,
                                                   const QString &baseShutterNs,
                                                   bool burst = false);
    Q_INVOKABLE bool captureRawImage(const QString &targetPath, const QString &cameraId,
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
                                     qreal focusY);
    Q_INVOKABLE bool captureJpegImage(const QString &targetPath, const QString &cameraId,
                                      const QString &jpegSize, int timeoutSeconds,
                                      int jpegQuality, int rotationDegrees,
                                      const QString &exposure,
                                      const QString &sceneMode,
                                      int sensorSensitivity,
                                      const QString &exposureTime,
                                      int aperture,
                                      int noiseReduction,
                                      qreal zoom);
    Q_INVOKABLE bool processRawImage(const QString &targetPath,
                                     const QString &rawPath,
                                     const QString &metadataPath,
                                     const QString &exposure,
                                     int jpegQuality,
                                     int rotationDegrees,
                                     const QString &rawSaveFormat,
                                     const QString &rawRenderEngine,
                                     int colorTemperature,
                                     int colorTint,
                                     bool progressiveJpeg);
    Q_INVOKABLE void discardRawBracket();
    Q_INVOKABLE bool combineRawBracket(const QString &targetPath,
                                          const QVariantList &sourcePaths,
                                          int jpegQuality);
    // Aligns and merges an equal-exposure burst (RawPipeline) into one JPEG.
    Q_INVOKABLE bool combineRawBurst(const QString &targetPath,
                                     const QVariantList &sourcePaths,
                                     int jpegQuality, qreal targetGain);
signals:
    void rawBracketFrameReady(const QString &path);
    // The sensor capture of a cold single RAW finished; rendering follows.
    void rawImageExposed();
    void rawImageCaptured(const QString &path, const QString &mimeType);
    void rawImageCaptureFailed(const QString &error);
    // Queued burst merges finish after their capture has completed.
    void burstRendered(const QString &path);
    void burstRenderFailed(const QString &error);
    void pendingRendersChanged();
    void keepBurstFramesChanged();

public:
    int pendingRenders() const;

private:
    // A captured burst waiting for (or in) its merge; owns the staged frames.
    struct BurstJob {
        QString target;
        QStringList sources;
        int quality = 92;
        RawPipeline::RenderSettings settings;
        QSharedPointer<QTemporaryDir> directory;
    };
    QList<BurstJob> m_burstJobs; // the first one is rendering while m_burstWatcher is set
    QScopedPointer<QFutureWatcher<QString> > m_burstWatcher;
    // One background thread for merges, at a lower priority than the UI.
    QThreadPool m_burstPool;
    void startNextBurstRender();
    void finishBurstRender();

    int m_captureCompensation = 0;
    void finishRawImageCapture(int exitCode, QProcess::ExitStatus exitStatus);
    void finishRawImageRender();
    void finishJpegRewrite();
    void finishBracketCombine();
    bool startRawImageProcess(const QString &program, const QStringList &arguments,
                              const QString &errorContext,
                              const QString &standardOutputPath = QString());
    bool renderRawImage();
    bool renderRawImageWithFastJpegConverter();
    bool renderRawImageEnhanced(const RawPipeline::RenderSettings &settings);
    bool startRawImageRender();
    bool saveJsonSidecar(const QString &targetPath, const QByteArray &json);
    bool copyJsonSidecar(const QString &sourcePath, const QString &targetPath);
    bool writeDngSidecar(const QString &metadataPath, const QString &targetPath);
    bool preserveRawCaptureFiles();
    bool stageRawBracketFrame();
    void clearRawImageCapture();
    bool loadCamera2Capabilities(const QString &cameraId);

    enum RawCaptureStage {
        RawCaptureIdle,
        RawCaptureCapturing,
        RawCaptureJpegCapturing
    };

    QJsonObject m_captureMetadata;
    QScopedPointer<QProcess> m_rawCaptureProcess;
    QScopedPointer<QFutureWatcher<bool> > m_rawRenderWatcher;
    QScopedPointer<QFutureWatcher<bool> > m_bracketCombineWatcher;
    QScopedPointer<QTemporaryDir> m_rawCaptureDirectory;
    QScopedPointer<QTemporaryDir> m_rawBracketDirectory;
    bool m_rawBracketCancelled = false;
    RawCaptureStage m_rawCaptureStage = RawCaptureIdle;
    QString m_rawCaptureTargetPath;
    QString m_rawCapturePrefix;
    QString m_rawCaptureArchivePrefix;
    QString m_rawCaptureErrors;
    QString m_rawCaptureExposure;
    QByteArray m_rawCaptureStandardOutput;
    QString m_rawCaptureSaveFormat = QStringLiteral("raw16");
    QString m_rawRenderEngine = QStringLiteral("internal");
    bool m_rawCaptureProgressiveJpeg = false;
    int m_rawCaptureJpegQuality = 92;
    int m_rawCaptureRotationDegrees = 0;
    int m_rawCaptureColorTemperature = 0;
    int m_rawCaptureColorTint = 0;
    int m_bracketIndex = -1;
    int m_bracketCount = 0;
    bool m_bracketBurst = false;
    bool m_keepBurstFrames = false;
    qreal m_bracketEv = 0.0;
    QString m_bracketBaseShutterNs;
    QString m_bracketCombineTargetPath;
    QStringList m_bracketCombineSourcePaths;
    int m_bracketCombineJpegQuality = 92;
    qint64 m_rawRenderStart = 0;
    QElapsedTimer m_rawCaptureTimer;
    bool m_camera2CapabilitiesLoaded = false;
    bool m_camera2CapabilitiesValid = false;
    QString m_camera2CapabilitiesCameraId;
    QStringList m_camera2Raw16Sizes;
    QStringList m_camera2Raw10Sizes;
    QStringList m_camera2JpegSizes;
    QStringList m_camera2PreviewSizes;
    QStringList m_camera2FocusModes;
    QStringList m_camera2Scenes;
    QVariantList m_camera2NoiseReductionModes;
    bool m_camera2ManualExposureSupported = false;
    bool m_camera2ManualBracketingSupported = false;
    qint64 m_camera2HalMaxShutterNs = 0;
    qint64 m_camera2MaxShutterNs = 0;
    QJsonObject m_camera2DeviceProfile;
    QString m_camera2DeviceProfileId;
    QString m_camera2DeviceProfileSource;
    QByteArray m_camera2ProbeJson;
    QByteArray m_camera2ProbeErrors;
    QJsonObject m_camera2SelectedCamera;
    qint64 m_camera2OverrideProfileMtime = 0;
};

#endif
