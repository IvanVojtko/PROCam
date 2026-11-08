// SPDX-FileCopyrightText: 2013 - 2024 Jolla Ltd.
// SPDX-FileCopyrightText: 2020 - 2021 Open Mobile Platform LLC.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.1
import QtMultimedia 5.0
import Amber.QrFilter 1.0
import Sailfish.Silica 1.0
import com.procam.camera 1.0
import "pages"

ApplicationWindow {
    id: window

    // Check once per launch, in ProCam's application identity (not system Settings).
    Component.onCompleted: {
        Settings.camera2Capabilities.logStartupTiming("qml-window-created")
        Settings.camera2Capabilities.ensureCamera2GeneratedHalConfig(Settings.deviceId)
    }

    property var captureModel: null
    property bool galleryActive
    property bool galleryVisible
    property int galleryIndex
    property bool camera2CaptureBusy
    readonly property bool _applicationActive: Qt.application.state === Qt.ApplicationActive
    // Launching: the camera starts while the window is still being shown
    // instead of after it became active (about a second later). Ends at the
    // first activation, or after 5 s for a launch that never became active.
    property bool _launching: true
    Timer {
        running: window._launching
        interval: 5000
        onTriggered: window._launching = false
    }
    // The viewfinder fills the window; controls float on top of it.
    readonly property real camera2TopStripHeight: 0

    on_ApplicationActiveChanged: {
        if (_applicationActive) {
            if (_launching) {
                _launching = false
                Settings.camera2Capabilities.logStartupTiming("app-active")
            }
            // Recovers a viewfinder that stopped in the background; one
            // already running (started during the launch) is kept.
            if (!camera2Preview.running)
                camera2Preview.restartPreview()
        }
    }

    allowedOrientations: defaultAllowedOrientations
    _defaultPageOrientations: Orientation.All
    _defaultLabelFormat: Text.PlainText

    cover: Qt.resolvedUrl("cover/CameraCover.qml")

    initialPage: Component {
        MainCameraPage {
            camera2Viewfinder: camera2Preview
            videoViewfinder: videoOutput
            camera2TopInset: camera2TopStripHeight
        }
    }

    // viewfinder background
    Rectangle {
        parent: window
        anchors.fill: parent
        z: -1
        color: "black"
        visible: (pageStack.depth < 2 && !pageStack.busy) || !galleryActive
    }

    VideoOutput {
        id: videoOutput

        z: -1
        // Full screen like the photo viewfinder (video has no tap-to-focus,
        // so no focus mapping depends on the old layout). "Full" crops to
        // fill, "4:3" fits the whole frame.
        width: window.width
        height: window.height
        fillMode: Settings.global.viewfinderFill ? VideoOutput.PreserveAspectCrop
                                                 : VideoOutput.PreserveAspectFit
        visible: pageStack.depth < 2 && !galleryActive && !camera2Preview.active

        Behavior on y {
            enabled: !galleryVisible
            NumberAnimation { duration: 150; easing.type: Easing.InOutQuad }
        }

        filters: [ qrFilter ]
    }

    Camera2Preview {
        id: camera2Preview

        z: -1
        y: camera2TopStripHeight
        width: window.width
        height: Math.max(1, window.height - camera2TopStripHeight)
        // Video mode with the "ProCam video recorder" setting: the encoder
        // joins this session, at 1080p with a 16:9 viewfinder.
        readonly property bool videoMode: Settings.global.captureMode === "video"
                                          && Settings.global.camera2Video
                                          && videoFailedCameras.indexOf(cameraId) < 0
        // Cameras that refused the encoder stream this session: their video
        // is recorded through gst-droid instead.
        property var videoFailedCameras: []
        // Cameras that dropped (or degraded) the RAW stream next to the
        // 1920x1440 viewfinder this session: they get 1440x1080, the size
        // Camera2 guarantees next to RAW.
        property var largePreviewRefused: []

        onRawCaptureReadyChanged: {
            if (!rawCaptureEnabled || videoMode || previewSize.width <= 1440
                    || largePreviewRefused.indexOf(cameraId) >= 0
                    || rawStreamStatus.indexOf("raw_error=") < 0
                    || rawStreamStatus.indexOf("raw_error=none") >= 0)
                return
            Settings.camera2Capabilities.logCaptureEvent("preview " + previewSize.width + "x"
                    + previewSize.height + " refused next to RAW (" + rawStreamStatus
                    + "); using 1440x1080")
            largePreviewRefused = largePreviewRefused.concat([cameraId])
        }

        onVideoStreamUnavailable: {
            if (videoFailedCameras.indexOf(cameraId) < 0)
                videoFailedCameras = videoFailedCameras.concat([cameraId])
        }
        active: (Settings.global.captureMode === "image" || videoMode)
                && !window.camera2CaptureBusy
                // A recording continues in the background (CaptureView stops
                // it after a minute) instead of losing the file.
                && (window._applicationActive || recording || window._launching)
                && pageStack.depth < 2
                && !galleryActive
        visible: pageStack.depth < 2 && !galleryActive
                 && (active || window.camera2CaptureBusy)
        cameraId: Settings.deviceId || (Settings.global.position === Camera.FrontFace ? "1" : "0")
        previewSize: videoMode ? Settings.camera2VideoPreviewSize(cameraId)
                               : Settings.camera2PreferredPreviewSize(
                                     cameraId, largePreviewRefused.indexOf(cameraId) < 0)
        readonly property string videoResolution:
            Settings.camera2VideoResolutions(cameraId).indexOf(Settings.global.camera2VideoResolution) >= 0
            ? Settings.global.camera2VideoResolution : "1080p"
        videoSize: videoMode ? Settings.camera2VideoSize(videoResolution) : Qt.size(0, 0)
        videoStabilization: Settings.global.videoStabilization
        videoCodec: Settings.global.camera2VideoCodec
        // HEVC needs about 30% fewer bits for the same picture; larger
        // frames need fewer bits per pixel (Android's camera profiles alike).
        videoBitrate: (videoResolution === "2160p" ? 48000000
                       : videoResolution === "1440p" ? 32000000 : 20000000)
                      * (videoCodec === "hevc" ? 0.7 : 1)
        // HDR merges RAW16 bursts whatever the capture format; JPEG mode then
        // streams RAW instead of JPEG.
        readonly property bool hdrCapture: Settings.global.hdrBurst || Settings.nightMode
                                           && Settings.camera2BurstSupported(cameraId)
        captureSize: Settings.mode.rawCaptureSize
        rawCaptureSize: Settings.camera2BurstRawSize()
        rawCaptureFormat: Settings.camera2BurstFormat(cameraId) === "yuv" ? "yuv" : "raw16"
        maxFrameRate: Settings.camera2PhotoMaxFps(cameraId)
        captureTimeout: Settings.mode.rawCaptureTimeout
        readonly property bool jpegFromRaw: Settings.camera2JpegFromRaw(cameraId)
        jpegCaptureEnabled: Settings.mode.camera2CaptureFormat === "jpeg" && !hdrCapture && !jpegFromRaw
                            && Settings.camera2WarmJpegCaptureSupported(Settings.mode.rawCaptureSize)
                            && Settings.mode.rawCaptureSpeedMode !== "quality"
        rawCaptureEnabled: Settings.mode.rawCaptureSpeedMode !== "quality"
                           && (hdrCapture || jpegFromRaw
                               || (Settings.mode.camera2CaptureFormat === "raw"
                                   && Settings.mode.rawCaptureRawFormat === "raw16"
                                   && Settings.camera2WarmRawCaptureSupported(Settings.mode.rawCaptureSize)))
        jpegQuality: Settings.photoJpegQuality()
        // Camera2 reports lens facing; Qt Multimedia's ids are the fallback.
        readonly property string lensFacing: Settings.camera2LensFacing(cameraId)
        readonly property bool frontFacing: lensFacing !== "unknown"
                                            ? lensFacing === "front"
                                            : Settings.global.frontFacingDeviceId !== ""
                                              && cameraId === Settings.global.frontFacingDeviceId
        orientation: Settings.camera2PreviewOrientation(
                         cameraId, frontFacing ? 270 : 90)
        mirror: Settings.camera2PreviewMirror(cameraId, frontFacing)
        selfieCamera: frontFacing
        captureMirror: frontFacing && Settings.global.mirrorSelfies
        fill: Settings.global.viewfinderFill
        photoAspect: videoMode ? "16:9" : Settings.photoAspect

        Behavior on y {
            enabled: !galleryVisible
            NumberAnimation { duration: 150; easing.type: Easing.InOutQuad }
        }
    }

    Label {
        anchors.centerIn: camera2Preview
        width: camera2Preview.width - 2 * Theme.horizontalPageMargin
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        visible: camera2Preview.visible && camera2Preview.active
                 && (camera2Preview.previewSize.width <= 0
                     || camera2Preview.previewSize.height <= 0)
        text: qsTr("Preview unavailable")
    }

    QrFilter {
        id: qrFilter

        active: Settings.global.qrFilterEnabled
                && Settings.global.captureMode === "image"
                && Settings.global.position === Camera.BackFace

        onActiveChanged: qrFilter.clearResult()
    }
}
