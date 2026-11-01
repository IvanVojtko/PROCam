// SPDX-FileCopyrightText: 2016 - 2024 Jolla Ltd.
// SPDX-FileCopyrightText: 2024 - 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import QtQuick.Window 2.0 as QuickWindow
import QtMultimedia 5.4
import QtPositioning 5.1
import QtSensors 5.0
import Nemo.Time 1.0
import Nemo.Configuration 1.0
import Nemo.Notifications 1.0
import org.nemomobile.systemsettings 1.0
import Sailfish.Silica 1.0
import Nemo.Ngf 1.0
import com.procam.camera 1.0

import "../settings"

SettingsOverlay {
    id: settingsOverlay

    property bool reallyWideScreen: captureView.reallyWideScreen
    property var captureView
    property var camera
    property Item focusArea
    readonly property real camera2MaximumZoom: Settings.camera2MaximumZoom()
    currentZoom: captureView._camera2ViewfinderActive ? captureView.camera2Zoom : camera.digitalZoom
    maximumZoom: zoomController.limit
    zoomButtonsEnabled: zoomController.allowed && maximumZoom > 1
    onZoomPressed: zoomController.press(direction)
    onZoomReleased: zoomController.release()
    onZoomCanceled: zoomController.cancel()
    onZoomResetRequested: if (zoomController.allowed) zoomController.setDirect(1)
    // The 1x / 2x presets work in Normal mode too.
    onZoomPresetRequested: zoomController.setDirect(value)

    function cancelZoom() {
        zoomController.cancel()
    }

    ZoomController {
        id: zoomController
        zoom: settingsOverlay.currentZoom
        maximumZoom: captureView._camera2ViewfinderActive
                     ? settingsOverlay.camera2MaximumZoom : camera.maximumDigitalZoom
        allowed: Settings.global.advancedMode && Settings.global.captureMode === "image"
                 && captureView.effectiveActive && !captureView.captureBusy
                 && !captureTimer.running && !settingsOverlay._exposed
                 && !settingsOverlay._pinchActive
                 && !settingsOverlay.orientationTransitionRunning
        onZoomRequested: {
            if (captureView._camera2ViewfinderActive) {
                captureView.camera2Zoom = value
            } else {
                camera.digitalZoom = value
            }
            zoomIndicator.show()
        }
    }

    property int _recordingDuration: clock.enabled ? ((clock.time - _startTime) / 1000) : 0
    property int _recSecsRemaining: {
        var totalBitRate = (camera.videoRecorder.videoBitRate + camera.videoRecorder.audioBitRate) * 1.05
        var maxDuration = Settings.storageMaxFileSize * 8 / totalBitRate
        return maxDuration - _recordingDuration
    }

    property var _startTime: new Date()

    width: captureView.width
    height: captureView.height

    property int _pictureRotation: {
        if (orientationSensor.connectedToBackend) {
            return QuickWindow.Screen.primaryOrientation == Qt.PortraitOrientation ? 0 : 90
        } else {
            switch (QuickWindow.Screen.orientation) {
            case Qt.PortraitOrientation:
                return 0
            case Qt.LandscapeOrientation:
                return 90
            case Qt.InvertedPortraitOrientation:
                return 180
            case Qt.InvertedLandscapeOrientation:
                return 270
            default:
                return 0
            }
        }
    }

    OrientationSensor {
        id: orientationSensor

        active: captureView.effectiveActive
        onReadingChanged: {
            switch (reading.orientation) {
            case OrientationReading.TopUp:
                _pictureRotation = 0; break
            case OrientationReading.TopDown:
                _pictureRotation = 180; break
            case OrientationReading.LeftUp:
                _pictureRotation = 270; break
            case OrientationReading.RightUp:
                _pictureRotation = 90; break
            default:
                // Keep device orientation at previous state
            }
        }
    }

    function captureMetadata() {
        var metadata = {
            capture_time_ms: Date.now(),
            capture_mode: Settings.global.advancedMode ? "advanced" : "simple",
            capture_format: Settings.mode.camera2CaptureFormat,
            camera_id: captureView._camera2ResolvedCameraId,
            capture_orientation: captureView._camera2CaptureRotation(),
            zoom_ratio: currentZoom,
            save_location: Settings.global.saveLocationInfo,
            requested_settings: {}
        }
        var settings = ["rawCaptureSize", "rawCaptureSpeedMode", "rawCaptureFocusMode",
                        "rawCaptureFocusDistance", "rawCaptureFocusTimeout", "rawCaptureExposure",
                        "rawCaptureIso", "rawCaptureShutterNs", "rawCaptureAperture",
                        "rawCaptureNoiseReduction", "rawCaptureBracket", "rawCaptureJpegQuality",
                        "rawCaptureScene", "rawCaptureProgressiveJpeg", "rawRenderEngine",
                        "rawCaptureRawFormat", "rawCaptureColorTemperature", "rawCaptureColorTint",
                        "rawCaptureTimeout", "rawCaptureFocusFailure", "flash", "iso", "meteringMode", "exposureMode"]
        for (var i = 0; i < settings.length; ++i)
            metadata.requested_settings[settings[i]] = Settings.mode[settings[i]]
        metadata.requested_settings.whiteBalance = Settings.global.whiteBalance
        metadata.exposure_compensation_requested = Settings.global.advancedMode ? Settings.global.exposureCompensation : 0
        if (metadata.save_location && positionSource.active && positionSource.position.coordinate.isValid) {
            var position = positionSource.position
            metadata.gps = { latitude: position.coordinate.latitude,
                             longitude: position.coordinate.longitude }
            if (position.coordinate.altitudeValid) metadata.gps.altitude = position.coordinate.altitude
            if (position.timestamp && !isNaN(position.timestamp.getTime()))
                metadata.gps.timestamp = position.timestamp.toISOString()
        }
        return metadata
    }

    function writeMetaData() {
        captureView.captureOrientation = deviceRotation

        // Camera documentation says dateTimeOriginal should be used but at the moment CameraBinMetaData uses only
        // date property (which the documentation doesn't even list)
        camera.metaData.date = new Date()

        if (positionSource.active) {
            var coordinate = positionSource.position.coordinate
            if (coordinate.isValid) {
                camera.metaData.gpsLatitude = coordinate.latitude
                camera.metaData.gpsLongitude = coordinate.longitude
            } else {
                camera.metaData.gpsLatitude = undefined
                camera.metaData.gpsLongitude = undefined
            }
            camera.metaData.gpsAltitude = positionSource.position.altitudeValid
                        ? coordinate.altitude
                        : undefined
        } else {
            camera.metaData.gpsLatitude = undefined
            camera.metaData.gpsLongitude = undefined
            camera.metaData.gpsAltitude = undefined
        }
    }

    readonly property int storagePathStatus: Settings.storagePathStatus
    onStoragePathStatusChanged: checkStorage()

    readonly property bool _applicationActive: Qt.application.state == Qt.ApplicationActive
    on_ApplicationActiveChanged: if (_applicationActive) checkStorage()

    Component.onCompleted: checkStorage()

    function checkStorage() {
        if (Qt.application.state != Qt.ApplicationActive) {
            // We don't want to show notification when we're in the background
            return
        }

        var prevStatus = previousStoragePathStatus.value
        if (Settings.storagePathStatus == Settings.Unavailable) {
            if (prevStatus != Settings.storagePathStatus) {
                notification.publishMessage("The selected storage is unavailable. Device memory will be used instead")
            }
        } else if (Settings.storagePathStatus == Settings.Available) {
            if (prevStatus == Settings.Unavailable || prevStatus == Settings.Mounting) {
                notification.publishMessage("Using memory card")
            }
        } else if (Settings.storagePathStatus == Settings.Mounting) {
            notification.publishMessage("Busy mounting the memory card. Device memory will be used instead")
        }
        previousStoragePathStatus.value = Settings.storagePathStatus

        // Refresh the remaining storage space, in case it was modified while we weren't active
        // TODO: Do this regularly throughout recording, if it doesn't interfere.
        if (!captureView.recording) {
            Settings.refreshMaxFileSize()
        }
    }

    PositionSource {
        id: positionSource

        // QDeclarativePositionSource::active property is not well behaved.
        // The internal position source may not be initialised until the component completes,
        // and in that instance the active property may be reset.
        // So, initialise the property after component completion to ensure correct behaviour.
        Component.onCompleted: positionSource.active = Qt.binding(function() {
            return captureView.effectiveActive && locationSettings.locationEnabled && Settings.global.saveLocationInfo
        })
    }

    opacity: 0.0

    showCommonControls: !captureView.recording
    isPortrait: captureView.isPortrait
    deviceRotation: {
        switch (captureView.orientation) {
        case Orientation.Landscape: return 0
        case Orientation.PortraitInverted: return 270
        case Orientation.LandscapeInverted: return 180
        default: return 90
        }
    }
    topButtonRowHeight: Screen.sizeCategory >= Screen.Large ? Theme.itemSizeLarge : Theme.itemSizeSmall
    deviceToggleEnabled: !captureView.captureUiBlocked

    onPinchStarted: {
        zoomController.cancel()
        // We're not getting notifications when the maximumDigitalZoom changes,
        // so update the value here.
        zoomIndicator.maximumZoom = captureView._camera2ViewfinderActive
                ? camera2MaximumZoom : camera.maximumDigitalZoom
    }

    onPinchUpdated: {
        var maximumZoom = captureView._camera2ViewfinderActive
                ? camera2MaximumZoom : camera.maximumDigitalZoom
        var currentZoom = captureView._camera2ViewfinderActive
                ? captureView.camera2Zoom : camera.digitalZoom
        var nextZoom = Math.max(1, Math.min(
                    currentZoom + ((maximumZoom - 1)
                                   * ((pinch.scale / Math.abs(pinch.previousScale) - 1))),
                    maximumZoom))
        zoomController.setDirect(nextZoom)
    }

    Connections {
        target: captureView
        ignoreUnknownSignals: true
        onEffectiveActiveChanged: {
            if (!captureView.effectiveActive) {
                settingsOverlay.closeMenus()
            }
        }
    }

    onClicked: {
        if (!captureView._captureOnFocus && captureView.touchFocusSupported) {
            var focusPoint
            if (captureView._camera2ViewfinderActive) {
                focusPoint = focusArea.mapFromItem(settingsOverlay, mouse.x, mouse.y)
            } else {
                // Translate and rotate the touch point into focusArea's space.
                switch ((360 - captureView.viewfinderOrientation) % 360) {

                case 90:
                    focusPoint = Qt.point(
                                mouse.y - ((height - focusArea.width) / 2) - captureView.viewfinderOffset,
                                width - mouse.x);
                    break;
                case 180:
                    focusPoint = Qt.point(
                                width - mouse.x - ((width - focusArea.width) / 2) + captureView.viewfinderOffset,
                                height - mouse.y);
                    break;
                case 270:
                    focusPoint = Qt.point(
                                height - mouse.y - ((height - focusArea.width) / 2) + captureView.viewfinderOffset,
                                mouse.x);
                    break;
                default:
                    focusPoint = Qt.point(
                                mouse.x - ((width - focusArea.width) / 2) - captureView.viewfinderOffset,
                                mouse.y);
                    break;
                }
            }

            // Normalize the focus point.
            focusPoint.x = focusPoint.x / focusArea.width
            focusPoint.y = focusPoint.y / focusArea.height

            // Mirror the point if the viewfinder is mirrored. Camera2 points
            // stay in display coordinates; Camera2Preview.mapToSensor() handles it.
            if (captureView._horizontalMirror && !captureView._camera2ViewfinderActive) {
                focusPoint.x = 1 - focusPoint.x
            }
            if (captureView._verticalMirror && !captureView._camera2ViewfinderActive) {
                focusPoint.y = 1 - focusPoint.y
            }

            if (focusPoint.x >= 0.0 && focusPoint.x <= 1.0 && focusPoint.y >= 0.0 && focusPoint.y <= 1.0) {
                captureView.setFocusPoint(focusPoint)
            }
        } else if (camera.captureMode == Camera.CaptureVideo && !captureView.recording) {
            tapBrightness.videoPoint = Qt.point(mouse.x, mouse.y)
            captureView.showVideoBrightness()
        }
    }

    shutter: CameraButton {
        id: captureButton

        property bool canStopVideo: startRecordTimer.running
                                    || camera.videoRecorder.recorderState == CameraRecorder.RecordingState

        z: settingsOverlay.inButtonLayout ? 1 : 0
        size: Theme.iconSizeMedium
        anchors.centerIn: parent
        background.visible: icon.opacity < 1.0
        enabled: !captureView._captureOnFocus
        // A short press-in, like Android shutters.
        scale: pressed && containsMouse ? 0.88 : 1.0
        Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }

        onPressed: {
            shutterHaptic.play()
            if (!captureView._camera2ViewfinderActive) {
                camera.lockAutoFocus()
            }
        }
        onReleased: {
            if (containsMouse) {
                captureView._triggerCapture()
            } else {
                camera.unlockAutoFocus()
            }
        }
        onCanceled: camera.unlockAutoFocus()

        icon {
            opacity: {
                if (captureTimer.running) {
                    return Theme.opacityLow
                } else if (captureButton.pressed) {
                    return Theme.opacityHigh
                } else if (captureView._captureOnFocus) {
                    return Theme.opacityOverlay
                } else {
                    return 1.0
                }
            }

            source: canStopVideo
                    ? "image://theme/icon-camera-video-shutter-off"
                    : (camera.captureMode == Camera.CaptureVideo
                       ? "image://theme/icon-camera-video-shutter-on"
                       : "image://theme/icon-camera-shutter")
        }

        Label {
            anchors {
                centerIn: parent
                // The font is not monospaced so countdown values over 10 seconds will be slightly off center
                // TODO: FontMetrics might make more sense than magic below, but also be quite much more complex codewise
                verticalCenterOffset: -Math.round(font.pixelSize/20)
                horizontalCenterOffset: text.length > 1 ? -Math.round(font.pixelSize/20) : 0
            }
            text: captureTimer.running ? Math.floor(captureView._captureCountdown + 1) : Settings.mode.timer
            visible: Settings.mode.timer != 0 && !captureButton.canStopVideo
            opacity: captureTimer.running ? captureView._captureCountdown % 1 : 1.0
            color: captureTimer.running ? Theme.lightPrimaryColor : Theme.darkPrimaryColor
            font {
                pixelSize: captureTimer.running ? Theme.fontSizeHuge : Theme.fontSizeTiny
                weight: Font.Light
            }
            Behavior on font.pixelSize {
                NumberAnimation { easing.type: Easing.InOutQuad; duration: 150 }
            }
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        size: BusyIndicatorSize.Large
        running: captureView.captureUiBlocked
                 && camera.captureMode == Camera.CaptureStillImage
                 && !(captureView._camera2ViewfinderActive
                      && Settings.mode.camera2CaptureFormat === "jpeg")
        visible: running
        z: 10
    }

    // Recording time in a pill with a red dot, where the floating top bar
    // sits (it hides while recording).
    Rectangle {
        anchors {
            verticalCenter: timerLabel.verticalCenter
            left: timerLabel.left
            right: timerLabel.right
            leftMargin: -Theme.paddingMedium - recordingDot.width - Theme.paddingSmall
            rightMargin: -Theme.paddingMedium
        }
        height: timerLabel.height + Theme.paddingSmall
        radius: height / 2
        color: Theme.rgba("black", 0.45)
        opacity: timerLabel.opacity

        Rectangle {
            id: recordingDot

            anchors {
                left: parent.left
                leftMargin: Theme.paddingMedium
                verticalCenter: parent.verticalCenter
            }
            width: Theme.paddingMedium
            height: width
            radius: width / 2
            color: "#ff3b30"
            SequentialAnimation on opacity {
                running: captureView.recording
                loops: Animation.Infinite
                NumberAnimation { to: 0.3; duration: 600 }
                NumberAnimation { to: 1.0; duration: 600 }
            }
        }
    }

    // Night mode exposes for a second or two: ask for a steady hand.
    Rectangle {
        anchors.centerIn: parent
        width: holdStillRow.width + 2 * Theme.paddingLarge
        height: holdStillRow.height + 2 * Theme.paddingMedium
        radius: height / 2
        color: Theme.rgba("black", 0.55)
        opacity: captureView.nightCaptureRunning ? 1.0 : 0.0
        visible: opacity > 0.0
        Behavior on opacity { FadeAnimator {} }

        Row {
            id: holdStillRow

            anchors.centerIn: parent
            spacing: Theme.paddingMedium

            MoonIcon {
                anchors.verticalCenter: parent.verticalCenter
                lit: true
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: "Hold still"
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.lightPrimaryColor
            }
        }
    }

    Label {
        id: timerLabel

        y: settingsOverlay._safeTopMargin + Theme.paddingMedium
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: Theme.paddingSmall
        opacity: captureView.recording ? 1.0 : 0.0
        Behavior on opacity { FadeAnimator {} }

        text: Format.formatDuration(_recordingDuration,
                                    _recordingDuration >= 3600 ? Formatter.DurationLong : Formatter.DurationShort)
        font.pixelSize: Theme.fontSizeLarge
        color: Theme.lightPrimaryColor
        style: Text.Outline
        styleColor: "#20000000"
    }

    Label {
        anchors {
            horizontalCenter: parent.horizontalCenter
            top: timerLabel.bottom
        }
        opacity: {
            if (captureView.recording)
                return _recSecsRemaining < 300 || (_recordingDuration > 120 && _recSecsRemaining < 3600) ? 1.0 : 0.0
            else 
                return camera.captureMode == Camera.CaptureVideo && _recSecsRemaining < 300 ? 1.0 : 0.0
        }
        Behavior on opacity { FadeAnimator {} }
        text: {
            if (captureView.recording) {
                if (_recSecsRemaining >= 60) {
                    return "Recording stops in " + Math.floor(_recSecsRemaining/60) + " min"
                } else {
                    return "Recording stops in " + _recSecsRemaining + " sec"
                }
            } else if (_recSecsRemaining == 0) {
                return "Storage full"
            } else {
                return Math.floor(_recSecsRemaining/60) + " min estimated recording time left"
            }
        }
        font.pixelSize: Theme.fontSizeExtraSmall
        color: Theme.lightPrimaryColor
        style: Text.Outline
        styleColor: "#20000000"
    }

    WallClock {
        id: clock

        updateFrequency: WallClock.Second
        enabled: camera.videoRecorder.recorderState == CameraRecorder.RecordingState
        onEnabledChanged: {
            if (enabled) {
                _startTime = clock.time
            }
        }
        onTimeChanged: {
            if (enabled && _recSecsRemaining <= 0 && camera) {
                camera.videoRecorder.stop()
            }
        }
    }

    // Viewfinder Grid
    Item {
        id: grid

        // Camera2: the part of the drawn frame that is on screen (all of it
        // when fitted, the screen when cropped to fill).
        readonly property var camera2Visible: {
            var viewfinder = captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
            if (!viewfinder || !parent) return null
            var content = viewfinder.contentRect
            var mapped = parent.mapFromItem(viewfinder, content.x, content.y, content.width, content.height)
            var left = Math.max(0, mapped.x), top = Math.max(0, mapped.y)
            var right = Math.min(parent.width, mapped.x + mapped.width)
            var bottom = Math.min(parent.height, mapped.y + mapped.height)
            return right > left && bottom > top ? Qt.rect(left, top, right - left, bottom - top) : null
        }
        property real gridWidth: camera2Visible ? camera2Visible.width
                                 : captureView.viewfinderOrientation % 180 == 0 ? focusArea.width : focusArea.height
        property real gridHeight: camera2Visible ? camera2Visible.height
                                  : captureView.viewfinderOrientation % 180 == 0 ? focusArea.height : focusArea.width

        anchors.centerIn: parent
        anchors.verticalCenterOffset: camera2Visible
                                      ? camera2Visible.y + camera2Visible.height / 2 - parent.height / 2
                                      : isPortrait ? captureView.viewfinderOffset : 0
        anchors.horizontalCenterOffset: camera2Visible
                                        ? camera2Visible.x + camera2Visible.width / 2 - parent.width / 2
                                        : isPortrait ? 0 : captureView.viewfinderOffset

        visible: Settings.global.viewfinderGrid != "none"
                 && (camera.cameraStatus == Camera.ActiveStatus
                     || captureView._camera2ViewfinderActive)

        width: gridWidth / 3
        height: gridHeight / 3

        GridLine {
            anchors {
                horizontalCenter: grid.horizontalCenter
                verticalCenter: grid.top
            }
            width: grid.gridWidth
        }

        GridLine {
            anchors {
                horizontalCenter: grid.horizontalCenter
                verticalCenter: grid.bottom
            }
            width: grid.gridWidth
        }

        GridLine {
            anchors {
                horizontalCenter: grid.left
                verticalCenter: grid.verticalCenter
            }
            width: grid.gridHeight
            rotation: 90
        }

        GridLine {
            anchors {
                horizontalCenter: grid.right
                verticalCenter: grid.verticalCenter
            }
            width: grid.gridHeight
            rotation: 90
        }
    }

    // Pixel-style brightness after tap-to-focus: drag the sun along the bar
    // beside the focus ring (+-2 EV).
    Item {
        id: tapBrightness

        readonly property bool video: Settings.global.captureMode === "video"
        // Where the video viewfinder was tapped (overlay coordinates).
        property point videoPoint
        readonly property bool shown: settingsOverlay.settingsOpacity < 0.5 && !!focusArea
                                      && (video ? captureView.videoBrightnessActive && !captureView.recording
                                                : captureView._camera2ViewfinderActive && captureView.tapFocusActive)
        readonly property real ringHalf: focusArea && !video ? focusArea.width * 0.075 : Theme.paddingLarge
        readonly property point ring: video
                                      ? videoPoint
                                      : focusArea
                                        ? focusArea.mapToItem(settingsOverlay,
                                                              captureView._camera2FocusPoint.x * focusArea.width,
                                                              captureView._camera2FocusPoint.y * focusArea.height)
                                        : Qt.point(0, 0)
        readonly property real fraction: (2 - captureView.tapExposureEv) / 4 // 0 = +2 EV at the top

        width: Theme.itemSizeExtraSmall
        height: Theme.itemSizeLarge * 1.6
        x: ring.x + ringHalf + Theme.paddingMedium + width <= settingsOverlay.width
           ? ring.x + ringHalf + Theme.paddingMedium
           : ring.x - ringHalf - Theme.paddingMedium - width
        y: Math.max(0, Math.min(settingsOverlay.height - height, ring.y - height / 2))
        z: 3
        visible: shown
        opacity: shown ? 1.0 : 0.0
        Behavior on opacity { FadeAnimation {} }

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.max(2, Math.round(Theme.pixelRatio * 2))
            height: parent.height
            radius: width / 2
            color: Theme.rgba("white", 0.7)
        }

        // The sun: a filled disc with short rays.
        Item {
            id: sun

            width: Theme.iconSizeSmall
            height: width
            anchors.horizontalCenter: parent.horizontalCenter
            y: tapBrightness.fraction * (tapBrightness.height - height)

            Repeater {
                model: 8
                Rectangle {
                    width: Math.max(2, Math.round(Theme.pixelRatio * 2))
                    height: sun.height * 0.22
                    radius: width / 2
                    color: "#ffd54a"
                    x: sun.width / 2 - width / 2
                    y: 0
                    // Pivot at the disc centre.
                    transform: Rotation {
                        origin.x: width / 2
                        origin.y: sun.height / 2
                        angle: index * 45
                    }
                }
            }
            Rectangle {
                anchors.centerIn: parent
                width: parent.width * 0.5
                height: width
                radius: width / 2
                color: "#ffd54a"
            }
        }

        Label {
            anchors {
                right: parent.left
                rightMargin: Theme.paddingSmall
                verticalCenter: sun.verticalCenter
            }
            visible: brightnessDrag.pressed
            text: (captureView.tapExposureEv > 0 ? "+" : "") + captureView.tapExposureEv.toFixed(1)
            font.pixelSize: Theme.fontSizeExtraSmall
            color: "#ffd54a"
        }

        MouseArea {
            id: brightnessDrag

            anchors.fill: parent
            anchors.margins: -Theme.paddingMedium
            preventStealing: true
            function apply(mouseY) {
                // The area reaches past the bar by -anchors.margins on each side.
                var f = Math.max(0, Math.min(1, (mouseY + anchors.margins - sun.height / 2)
                                             / (tapBrightness.height - sun.height)))
                captureView.setTapExposure(Math.round((2 - 4 * f) * 10) / 10)
            }
            onPressed: apply(mouse.y)
            onPositionChanged: apply(mouse.y)
        }
    }

    NonGraphicalFeedback {
        id: shutterHaptic
        event: "feedback_press"
    }

    ZoomIndicator {
        id: zoomIndicator

        anchors {
            top: timerLabel.bottom
            topMargin: settingsOverlay.floatingTopBarBottom
            horizontalCenter: parent.horizontalCenter
        }

        zoom: captureView._camera2ViewfinderActive
              ? captureView.camera2Zoom : camera.digitalZoom
        maximumZoom: captureView._camera2ViewfinderActive
                     ? settingsOverlay.camera2MaximumZoom
                     : camera.maximumDigitalZoom
    }

    Notification {
        id: notification

        function publishMessage(msg) {
            notification.previewBody = msg
            notification.publish()
        }

        isTransient: true
        urgency: Notification.Critical
    }

    LocationSettings { id: locationSettings }

    ConfigurationValue {
        id: previousStoragePathStatus
        key: "/apps/procam/previousStoragePathStatus"
    }
}
