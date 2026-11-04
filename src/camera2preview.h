#ifndef CAMERA2PREVIEW_H
#define CAMERA2PREVIEW_H

#include "previewanalysis.h"

#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QQuickItem>
#include <QSize>
#include <QTimer>
#include <QVariant>

class Camera2Preview : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool jpegCaptureReady READ jpegCaptureReady NOTIFY jpegCaptureReadyChanged)
    Q_PROPERTY(bool rawCaptureReady READ rawCaptureReady NOTIFY rawCaptureReadyChanged)
    Q_PROPERTY(QString cameraId READ cameraId WRITE setCameraId NOTIFY cameraIdChanged)
    Q_PROPERTY(QSize previewSize READ previewSize WRITE setPreviewSize NOTIFY previewSizeChanged)
    Q_PROPERTY(QString captureSize READ captureSize WRITE setCaptureSize NOTIFY captureSizeChanged)
    // RAW16 stream size when it differs from captureSize (HDR in JPEG mode).
    Q_PROPERTY(QString rawCaptureSize READ rawCaptureSize WRITE setRawCaptureSize NOTIFY rawCaptureSizeChanged)
    // "raw16", or "yuv" for cameras without RAW output (I420 burst frames).
    Q_PROPERTY(QString rawCaptureFormat READ rawCaptureFormat WRITE setRawCaptureFormat NOTIFY rawCaptureFormatChanged)
    // The helper's last RAW/YUV stream report ("raw=0 raw_error=session:-10003 ..."), for diagnostics.
    Q_PROPERTY(QString rawStreamStatus READ rawStreamStatus NOTIFY rawCaptureReadyChanged)
    Q_PROPERTY(int captureTimeout READ captureTimeout WRITE setCaptureTimeout NOTIFY captureTimeoutChanged)
    Q_PROPERTY(bool jpegCaptureEnabled READ jpegCaptureEnabled WRITE setJpegCaptureEnabled NOTIFY jpegCaptureEnabledChanged)
    Q_PROPERTY(bool rawCaptureEnabled READ rawCaptureEnabled WRITE setRawCaptureEnabled NOTIFY rawCaptureEnabledChanged)
    Q_PROPERTY(int jpegQuality READ jpegQuality WRITE setJpegQuality NOTIFY jpegQualityChanged)
    Q_PROPERTY(int jpegOrientation READ jpegOrientation WRITE setJpegOrientation NOTIFY jpegOrientationChanged)
    Q_PROPERTY(QVariant source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(int orientation READ orientation WRITE setOrientation NOTIFY orientationChanged)
    Q_PROPERTY(bool mirror READ mirror WRITE setMirror NOTIFY mirrorChanged)
    Q_PROPERTY(bool fill READ fill WRITE setFill NOTIFY fillChanged)
    // Photo shape ("4:3" sensor frame or "16:9" crop): the drawn frame and
    // the instant thumbnail show only what the photo will contain.
    Q_PROPERTY(QString photoAspect READ photoAspect WRITE setPhotoAspect NOTIFY photoAspectChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(QString focusMode READ focusMode WRITE setFocusMode NOTIFY focusModeChanged)
    Q_PROPERTY(qreal focusDistance READ focusDistance WRITE setFocusDistance NOTIFY focusDistanceChanged)
    Q_PROPERTY(int exposureCompensation READ exposureCompensation WRITE setExposureCompensation NOTIFY exposureCompensationChanged)
    Q_PROPERTY(int sensorSensitivity READ sensorSensitivity WRITE setSensorSensitivity NOTIFY sensorSensitivityChanged)
    Q_PROPERTY(QString exposureTime READ exposureTime WRITE setExposureTime NOTIFY exposureTimeChanged)
    Q_PROPERTY(int aperture READ aperture WRITE setAperture NOTIFY apertureChanged)
    Q_PROPERTY(int noiseReduction READ noiseReduction WRITE setNoiseReduction NOTIFY noiseReductionChanged)
    Q_PROPERTY(bool focusPeaking READ focusPeaking WRITE setFocusPeaking NOTIFY focusPeakingChanged)
    Q_PROPERTY(bool exposureZebras READ exposureZebras WRITE setExposureZebras NOTIFY exposureZebrasChanged)
    // "off", "auto" (the preview's AE then reports when the flash is needed),
    // "on" or "torch" (lit continuously, for video). Photos fire the flash
    // only when captureRaw() is asked to.
    Q_PROPERTY(QString flashMode READ flashMode WRITE setFlashMode NOTIFY flashModeChanged)
    Q_PROPERTY(qreal renderExposure READ renderExposure WRITE setRenderExposure NOTIFY renderExposureChanged)
    Q_PROPERTY(QString sceneMode READ sceneMode WRITE setSceneMode NOTIFY sceneModeChanged)
    Q_PROPERTY(int colorTemperature READ colorTemperature WRITE setColorTemperature NOTIFY colorTemperatureChanged)
    Q_PROPERTY(int colorTint READ colorTint WRITE setColorTint NOTIFY colorTintChanged)
    Q_PROPERTY(qreal focalLength READ focalLength NOTIFY focalLengthChanged)
    Q_PROPERTY(int liveSensorSensitivity READ liveSensorSensitivity NOTIFY liveSensorSensitivityChanged)
    Q_PROPERTY(QString liveExposureTime READ liveExposureTime NOTIFY liveExposureTimeChanged)
    Q_PROPERTY(QVariantList histogram READ histogram NOTIFY histogramChanged)
    // Faces the camera detects, as [left, top, right, bottom] normalised on
    // the full sensor frame (the burst renderer's coordinates).
    Q_PROPERTY(QVariantList faces READ faces NOTIFY sceneChanged)
    // Front camera: a dark centre against bright light counts as the face
    // when the camera detects none (backlit selfie).
    Q_PROPERTY(bool selfieCamera MEMBER m_selfieCamera NOTIFY sceneChanged)
    // Photos saved flipped left-right, as the (mirrored) viewfinder shows
    // them: the "Mirror selfies" setting on a front camera.
    Q_PROPERTY(bool captureMirror MEMBER m_captureMirror NOTIFY captureMirrorChanged)
    // Where the frame is drawn, in item coordinates (larger than the item when
    // fill crops it).
    Q_PROPERTY(QRectF contentRect READ contentRect NOTIFY contentRectChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    // Video mode: a valid videoSize puts the encoder in the preview session
    // (no still streams); startRecording()/stopRecording() then record from
    // it without restarting the viewfinder.
    Q_PROPERTY(QSize videoSize READ videoSize WRITE setVideoSize NOTIFY videoSizeChanged)
    Q_PROPERTY(int videoFrameRate READ videoFrameRate WRITE setVideoFrameRate NOTIFY videoFrameRateChanged)
    Q_PROPERTY(int videoBitrate READ videoBitrate WRITE setVideoBitrate NOTIFY videoBitrateChanged)
    Q_PROPERTY(bool videoStabilization READ videoStabilization WRITE setVideoStabilization NOTIFY videoStabilizationChanged)
    // "avc" (H.264) or "hevc"; the bridge falls back to avc without an HEVC encoder.
    Q_PROPERTY(QString videoCodec READ videoCodec WRITE setVideoCodec NOTIFY videoCodecChanged)
    Q_PROPERTY(bool videoReady READ videoReady NOTIFY videoReadyChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)

public:
    explicit Camera2Preview(QQuickItem *parent = nullptr);
    ~Camera2Preview() override;

    bool active() const;
    void setActive(bool active);

    bool running() const;
    bool jpegCaptureReady() const;
    bool rawCaptureReady() const;

    QString cameraId() const;
    void setCameraId(const QString &cameraId);

    QSize previewSize() const;
    void setPreviewSize(const QSize &previewSize);

    QString captureSize() const;
    void setCaptureSize(const QString &captureSize);
    QString rawCaptureSize() const;
    void setRawCaptureSize(const QString &rawCaptureSize);
    QString rawCaptureFormat() const;
    QString rawStreamStatus() const;
    // Small JPEG of the current frame as the photo will look (unmirrored,
    // zoom crop, rotated by captureRotation), for an instant gallery
    // thumbnail while the photo is processed. Returns a file URL or "".
    Q_INVOKABLE QString saveThumbnail(int captureRotation);
    // For the burst plan: motion (frame widths per second, -1 unknown),
    // subjectMotion (0..1), faceLuma (0..1, -1 without a face), faceClipped.
    Q_INVOKABLE QVariantMap sceneInfo() const;
    void setRawCaptureFormat(const QString &rawCaptureFormat);

    int captureTimeout() const;
    void setCaptureTimeout(int captureTimeout);

    bool jpegCaptureEnabled() const;
    void setJpegCaptureEnabled(bool jpegCaptureEnabled);
    bool rawCaptureEnabled() const;
    void setRawCaptureEnabled(bool rawCaptureEnabled);

    int jpegQuality() const;
    void setJpegQuality(int jpegQuality);

    int jpegOrientation() const;
    void setJpegOrientation(int jpegOrientation);

    QVariant source() const;
    void setSource(const QVariant &source);

    int orientation() const;
    void setOrientation(int orientation);

    bool mirror() const;
    void setMirror(bool mirror);

    bool fill() const;
    QRectF contentRect() const;
    void setFill(bool fill);
    QString photoAspect() const;
    void setPhotoAspect(const QString &aspect);

    qreal zoom() const;
    void setZoom(qreal zoom);

    QString focusMode() const;
    void setFocusMode(const QString &focusMode);

    qreal focusDistance() const;
    void setFocusDistance(qreal focusDistance);

    int exposureCompensation() const;
    void setExposureCompensation(int exposureCompensation);

    int sensorSensitivity() const;
    void setSensorSensitivity(int sensorSensitivity);

    QString exposureTime() const;
    void setExposureTime(const QString &exposureTime);

    int aperture() const;
    void setAperture(int aperture);

    int noiseReduction() const;
    void setNoiseReduction(int noiseReduction);

    bool focusPeaking() const { return m_focusPeaking; }
    void setFocusPeaking(bool enabled);
    bool exposureZebras() const { return m_exposureZebras; }
    void setExposureZebras(bool enabled);
    QString flashMode() const { return m_flashMode; }
    void setFlashMode(const QString &flashMode);
    qreal renderExposure() const;
    void setRenderExposure(qreal renderExposure);

    QString sceneMode() const;
    void setSceneMode(const QString &sceneMode);

    int colorTemperature() const;
    void setColorTemperature(int colorTemperature);

    int colorTint() const;
    void setColorTint(int colorTint);

    qreal focalLength() const;
    int liveSensorSensitivity() const;
    QString liveExposureTime() const;
    QVariantList faces() const;
    QVariantList histogram() const;

    QString errorString() const;

    QSize videoSize() const { return m_videoSize; }
    void setVideoSize(const QSize &videoSize);
    int videoFrameRate() const { return m_videoFrameRate; }
    void setVideoFrameRate(int videoFrameRate);
    int videoBitrate() const { return m_videoBitrate; }
    void setVideoBitrate(int videoBitrate);
    bool videoStabilization() const { return m_videoStabilization; }
    void setVideoStabilization(bool videoStabilization);
    QString videoCodec() const { return m_videoCodec; }
    void setVideoCodec(const QString &videoCodec);
    bool videoReady() const { return m_videoReady; }
    bool recording() const { return m_recording; }
    // Records to path (an .mp4 that must not exist yet); orientation is the
    // clockwise rotation players apply (0, 90, 180, 270). recordingStarted
    // or recordingFinished(success false) follows.
    Q_INVOKABLE bool startRecording(const QString &path, int orientation);
    // recordingFinished follows once the file is complete.
    Q_INVOKABLE void stopRecording();

    /**
     * Restarts the Camera2 preview helper process.
     */
    Q_INVOKABLE void restartPreview();
    Q_INVOKABLE void setExposurePair(int iso, const QString &shutter, int compensationSteps);

    /**
     * Requests Camera2 focus and metering at normalized viewfinder coordinates.
     */
    Q_INVOKABLE void setFocusPoint(qreal x, qreal y);

    /**
     * Clears the Camera2 focus and metering point.
     */
    Q_INVOKABLE void clearFocusPoint();

    /**
     * Holds the current Camera2 lens distance until releaseFocusHold().
     */
    Q_INVOKABLE void holdFocus();

    /**
     * Releases a Camera2 lens distance hold.
     */
    Q_INVOKABLE void releaseFocusHold();

    /**
     * Captures a JPEG from the running Camera2 preview session.
     */
    Q_INVOKABLE void setCaptureMetadata(const QVariantMap &metadata);
    Q_INVOKABLE bool captureJpeg(const QString &path);
    Q_INVOKABLE bool captureJpegWithExposure(const QString &path,
                                             int sensorSensitivity,
                                             const QString &exposureTime);
    Q_INVOKABLE bool captureRaw(const QString &rawPath,
                                const QString &metadataPath,
                                bool flash = false);
    Q_INVOKABLE bool captureRawWithExposure(const QString &rawPath,
                                            const QString &metadataPath,
                                            int sensorSensitivity,
                                            const QString &exposureTime,
                                            bool flash = false);
    Q_INVOKABLE bool captureRawBracket(const QVariantList &rawPaths,
                                       const QVariantList &metadataPaths,
                                       int sensorSensitivity,
                                       const QVariantList &exposureTimes);
    // Equal-exposure RAW burst for merging (2 to 8 frames); completes with
    // rawBracketReady like a bracket.
    // Item point to normalised sensor (active array) coordinates for focus and
    // metering regions: undoes the draw scale/crop, mirroring and rotation.
    // Returns (-1, -1) outside the drawn frame.
    Q_INVOKABLE QPointF mapToSensor(qreal x, qreal y) const;
    Q_INVOKABLE bool captureRawBurst(const QVariantList &rawPaths,
                                     const QVariantList &metadataPaths,
                                     int sensorSensitivity,
                                     const QString &exposureTime);
    // Why the last capture call returned false, if its folder was the
    // reason ("Cannot write to folder ..."); empty otherwise.
    Q_INVOKABLE QString captureStartError() const { return m_captureStartError; }

protected:
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    bool frameGeometry(const QSizeF &imageSize, QRectF *drawRect, QRectF *sourceRect) const;
    qreal frameCropZoom() const;
    bool startRawFrameSet(const QByteArray &command, const QVariantList &rawPaths,
                          const QVariantList &metadataPaths);
    void finishJpegCapture(const QString &path);

signals:
    void activeChanged();
    void runningChanged();
    void jpegCaptureReadyChanged();
    void rawCaptureReadyChanged();
    void cameraIdChanged();
    void previewSizeChanged();
    void captureSizeChanged();
    void rawCaptureSizeChanged();
    void rawCaptureFormatChanged();
    void captureTimeoutChanged();
    void jpegCaptureEnabledChanged();
    void rawCaptureEnabledChanged();
    void jpegQualityChanged();
    void jpegOrientationChanged();
    void sourceChanged();
    void orientationChanged();
    void mirrorChanged();
    void fillChanged();
    void photoAspectChanged();
    void zoomChanged();
    void focusModeChanged();
    void focusDistanceChanged();
    void exposureCompensationChanged();
    void sensorSensitivityChanged();
    void exposureTimeChanged();
    void apertureChanged();
    void noiseReductionChanged();
    void focusPeakingChanged();
    void exposureZebrasChanged();
    void flashModeChanged();
    void renderExposureChanged();
    void sceneModeChanged();
    void colorTemperatureChanged();
    void colorTintChanged();
    void focalLengthChanged();
    void liveSensorSensitivityChanged();
    void liveExposureTimeChanged();
    void histogramChanged();
    void sceneChanged();
    void contentRectChanged();
    // A warm capture is exposed (the JPEG, or a RAW capture's last frame);
    // the files follow with imageCaptured / rawImageReady / rawBracketReady.
    void captureExposed();
    // A warm RAW capture's first frame starts exposing.
    void captureStarted();
    void exposureSample(const QVariantMap &sample);
    void errorStringChanged();
    void imageCaptured(const QString &path, const QString &mimeType);
    void rawImageReady(const QString &rawPath, const QString &metadataPath);
    void rawBracketReady(const QVariantList &rawPaths,
                         const QVariantList &metadataPaths);
    void imageCaptureFailed(const QString &error);
    void videoSizeChanged();
    void captureMirrorChanged();
    void videoFrameRateChanged();
    void videoBitrateChanged();
    void videoStabilizationChanged();
    void videoCodecChanged();
    void videoReadyChanged();
    void recordingChanged();
    void recordingStarted(const QString &path);
    void recordingFinished(const QString &path, bool success, const QString &error);
    // The camera refused the encoder next to the viewfinder (error: the
    // bridge's stage); the app records through gst-droid instead.
    void videoStreamUnavailable(const QString &error);

private slots:
    void readFrames();
    void readErrors();
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void checkCaptureResult();
    void finishPreviewCapture(const QString &path, bool success,
                              const QString &error);

private:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override;

    void restart();
    void startPreview();
    void stop();
    void savePreviewJpegAsync(const QString &path);
    bool savePreviewMetadata(const QString &path, const QSize &imageSize);
    void parseFrames();
    void parseMetadata(const QByteArray &payload);
    void parseCaptureResult(const QMap<QString, QString> &fields);
    void parseRecordStatus(const QByteArray &payload, const QMap<QString, QString> &fields);
    void finishRecording(bool success, const QString &error);
    void updateHistogram(const QImage &frame);
    void updateHistogramYuv();
    bool hasFrame() const;
    QSize frameSize() const;
    // The frame as RGB888 in sensor orientation (converted from YUV on demand).
    QImage rgbFrame() const;
    // The frame as shown: rotated by m_orientation, mirrored if m_mirror.
    QSize displaySize() const;
    QImage displayFrame() const;
    int frameRotation() const;
    bool frameMirror() const;
    void sendSettings();
    void sendSettings(bool captureExposure);
    void sendFlashMode();
    void sendExposureSettings(bool captureExposure);
    void restorePreviewSettingsAfterCapture();
    void setErrorString(const QString &errorString);
    QString helperPath() const;

    bool m_active = false;
    bool m_restartPending = false;
    bool m_running = false;
    QString m_cameraId = QStringLiteral("0");
    QSize m_previewSize;
    QString m_captureSize = QStringLiteral("4096x3072");
    QString m_rawCaptureSize;
    QString m_rawCaptureFormat = QStringLiteral("raw16");
    QString m_rawStreamStatus;
    int m_captureTimeout = 30;
    bool m_jpegCaptureEnabled = false;
    bool m_rawCaptureEnabled = false;
    // The running bridge confirmed it has a RAW stream (streams packet).
    bool m_rawStreamActive = false;
    int m_jpegQuality = 92;
    int m_jpegOrientation = 90;
    QVariant m_source;
    int m_orientation = 90;
    bool m_mirror = false;
    bool m_fill = false;
    QString m_photoAspect = QStringLiteral("4:3");
    qreal m_zoom = 1.0;
    // Zoom the camera already cropped the preview frames with (the bridge's
    // "zoom=" metadata); the viewfinder crops only what is left of m_zoom.
    qreal m_frameZoom = 1.0;
    qreal m_focusX = -1.0;
    qreal m_focusY = -1.0;
    QString m_focusMode = QStringLiteral("continuous");
    qreal m_focusDistance = 0.0;
    int m_exposureCompensation = 0;
    int m_sensorSensitivity = 0;
    QString m_exposureTime = QStringLiteral("0");
    int m_aperture = 0;
    int m_noiseReduction = 0;
    bool m_focusPeaking = false;
    bool m_exposureZebras = false;
    QString m_flashMode = QStringLiteral("off");
    qreal m_renderExposure = 1.0;
    QString m_sceneMode = QStringLiteral("manual");
    int m_colorTemperature = 0;
    int m_colorTint = 0;
    qreal m_focalLength = 0.0;
    int m_liveSensorSensitivity = 0;
    QString m_liveExposureTime = QStringLiteral("0");
    QVariantList m_histogram;
    QString m_errorString;
    QString m_captureStartError;
    bool prepareCaptureFolders(const QStringList &paths);
    // Newest preview frame as the bridge sent it (sensor orientation, not
    // mirrored) when it came as RGB; the scene graph rotates and mirrors it.
    QImage m_frame;
    // Or the newest frame as I420 (Y, then U and V at half resolution);
    // then m_frame is null. Shared with the render thread, never modified.
    QByteArray m_yuvFrame;
    QSize m_yuvSize;
    // m_frame is the blurred last frame of the previous camera, already as
    // shown (no rotation or mirror to apply).
    bool m_frameDisplayOriented = false;
    quint64 m_frameSerial = 0;
    quint64 m_drawnSerial = 0;
    QElapsedTimer m_histogramTimer;
    PreviewAnalysis::MotionMeter m_motionMeter;
    QElapsedTimer m_motionClock;
    std::vector<std::array<float, 4>> m_faces;
    // The last detected faces, kept for 1.5 s after detection drops out
    // (a face turning or lit from behind at the moment of the shot), and the
    // backlit-selfie fallback.
    std::vector<std::array<float, 4>> m_lastFaces, m_subjectFaces;
    QElapsedTimer m_faceSeen;
    bool m_selfieCamera = false;
    bool m_captureMirror = false;
    std::vector<std::array<float, 4>> effectiveFaces(QString *source = nullptr) const;
    PreviewAnalysis::FaceExposure m_faceExposure;
    // Preview rate diagnostics, logged every 15 s.
    QElapsedTimer m_statsTimer;
    int m_statsFrames = 0;
    qint64 m_statsParseNs = 0;
    QByteArray m_buffer;
    QMap<QString, QString> m_lastCaptureResultFields;
    bool m_previewCaptureRunning = false;
    QString m_pendingCapturePath;
    QString m_pendingCaptureFinalPath;
    qint64 m_pendingCaptureSize = -1;
    int m_pendingCaptureStablePolls = 0;
    int m_capturePollsRemaining = 0;
    bool m_pendingRawCapture = false;
    bool m_pendingRawBracketCapture = false;
    int m_pendingRawBracketCount = 0;
    QVariantList m_pendingRawBracketRawPaths;
    QJsonObject m_captureMetadata;
    QByteArray m_originalJpegExif;
    QVariantList m_pendingRawBracketMetadataPaths;
    QVariantList m_completedRawBracketRawPaths;
    QVariantList m_completedRawBracketMetadataPaths;
    bool m_exposureOnlyCapture = false;
    bool m_restorePreviewSettingsAfterCapture = false;
    bool m_restoreExposureOnlySettingsAfterCapture = false;
    QTimer m_captureTimer;
    QProcess *m_process = nullptr;
    QSize m_videoSize;
    int m_videoFrameRate = 30;
    int m_videoBitrate = 20000000;
    bool m_videoStabilization = true;
    QString m_videoCodec = QStringLiteral("avc");
    bool m_videoReady = false;
    bool m_recording = false;
    QString m_recordingPath;
    // A stop that the helper never answers still ends the recording.
    QTimer m_recordingStopTimer;
    // Timing for the log: since the helper started (first frame) and since
    // a capture command went out (exposed, files ready).
    QElapsedTimer m_helperClock;
    bool m_firstFrameLogged = false;
    QElapsedTimer m_captureClock;
};

#endif
