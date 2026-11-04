// SPDX-FileCopyrightText: 2016 - 2024 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.4
import QtMultimedia 5.6
import Sailfish.Silica 1.0
import com.procam.camera 1.0
import Nemo.Configuration 1.0
import "../capture"

PinchArea {
    id: overlay

    property bool isPortrait
    property int deviceRotation: 90
    property real topButtonRowHeight
    property bool showCommonControls: true // any controls from here
    property bool deviceToggleEnabled
    property bool inButtonLayout

    property alias shutter: shutterContainer.children
    property alias anchorContainer: anchorContainer
    property alias container: container
    readonly property alias settingsOpacity: grid.opacity
    property bool orientationTransitionRunning
    property bool zoomButtonsEnabled: false
    readonly property var exposureControl: captureView ? captureView.exposureControl : null
    onDeviceRotationChanged: syncDeviceRotation()

    // Bottom edge of the floating photo-mode bar (0 when hidden).
    readonly property real floatingTopBarBottom: floatingTopBar.visible
                                                 ? floatingTopBar.y + floatingTopBar.height : 0

    property real currentZoom: 1
    property real maximumZoom: 1
    signal zoomPressed(int direction)
    signal zoomReleased()
    signal zoomCanceled()
    signal zoomResetRequested()
    signal zoomPresetRequested(real value)

    // Viewfinder swipes: up/down flips front and rear (photo mode), left/right
    // switches photo and video.
    function flipCamera() {
        if (!camera2BottomDeck.active || !captureView || captureView.captureBusy || captureView.recording) return
        if (camera2BottomDeck.video) {
            // Qt Multimedia ids for video.
            if (!camera.hasCameraOnBothSides) return
            Settings.deviceId = Settings.global.position === Camera.BackFace
                    ? Settings.global.frontFacingDeviceId
                    : Settings.global.previousBackFacingDeviceId
            captureView.resetZoom()
            return
        }
        var roles = Settings.camera2CameraRoles()
        if (!roles.front) return
        var back = Settings.global.previousBackFacingDeviceId
        var target = Settings.deviceId === roles.front
                ? (back && back !== roles.front ? back : roles.main)
                : roles.front
        if (target && target !== Settings.deviceId) {
            Settings.deviceId = target
            captureView.resetZoom()
        }
    }

    function selectCaptureMode(mode) {
        if (!captureView || captureView.captureBusy || captureView.recording) return
        if (Settings.global.captureMode !== mode) Settings.global.captureMode = mode
    }

    function evChoices() {
        if (!exposureControl) return [0]
        if (exposureControl.partial) return [-4, -3, -2, -1, 0, 1, 2, 3, 4]
        var caps = exposureControl.capabilities
        var step = Number(caps.compensation_step_ev)
        var range = caps.compensation_range || [0, 0]
        if (!(step > 0)) return [0]
        var choices = []
        // Store twice EV for compatibility with existing settings; fractions are supported.
        for (var i = Math.max(range[0], Math.ceil(-2 / step)); i <= Math.min(range[1], Math.floor(2 / step)); ++i)
            choices.push(2 * i * step)
        return choices.length ? choices : [0]
    }
    function evLabel(value) { return Number((Number(value) / 2).toFixed(2)) + " EV" }

    Column {
        z: 20
        anchors {
            top: parent.top
            horizontalCenter: parent.horizontalCenter
            topMargin: Theme.paddingLarge + upperHeader.labelVerticalOffset
        }
        width: parent.width * 0.8
        visible: Settings.global.showExposureStatus && overlay.exposureControl && !overlay._exposed && overlay.showCommonControls
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeTiny
            text: overlay.exposureControl ? (overlay.exposureControl.limitStatus
                  ? overlay.exposureControl.limitStatus + " · " + overlay.exposureControl.residualEv.toFixed(1) + " EV shortfall"
                  : (overlay.exposureControl.notice || overlay.exposureControl.status)) : ""
        }
    }

    property bool _pinchActive
    property bool topMenuOpen
    property bool _closing
    // top menu open or transitioning
    readonly property bool _exposed: topMenuOpen
                                     || _closing
                                     || verticalAnimation.running
                                     || dragArea.drag.active

    default property alias _data: container.data

    readonly property int _captureButtonLocation: overlay.isPortrait
                                                  ? Settings.global.portraitCaptureButtonLocation
                                                  : Settings.global.landscapeCaptureButtonLocation

    property real _progress: (panel.y + panel.height) / panel.height

    property real _menuItemHorizontalSpacing: Screen.sizeCategory >= Screen.Large
                                              ? Theme.paddingLarge * 2
                                              : Theme.paddingLarge
    property real _headerHeight: Screen.sizeCategory >= Screen.Large
                                 ? Theme.itemSizeMedium
                                 : Theme.itemSizeSmall + Theme.paddingMedium
    property real _safeTopMargin: Screen.hasCutouts && overlay.isPortrait
                                  ? Screen.topCutout.height + Theme.paddingMedium
                                  : 0
    property real _headerTopMargin: _safeTopMargin
                                    + (Screen.sizeCategory >= Screen.Large
                                       ? Theme.paddingLarge + Theme.paddingSmall
                                       : Theme.paddingSmall)
    readonly property real _menuWidth: Screen.sizeCategory >= Screen.Large
                                       ? Theme.iconSizeLarge + Theme.paddingMedium*2 // increase icon hitbox
                                       : Theme.iconSizeMedium + Theme.paddingMedium + Theme.paddingSmall

    property color _highlightColor: Theme.colorScheme == Theme.LightOnDark
                                    ? Theme.highlightColor
                                    : Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark)

    property real _commonControlOpacity: showCommonControls ? 1.0 : 0.0
    Behavior on _commonControlOpacity { FadeAnimation {} }

    onShowCommonControlsChanged: {
        if (!showCommonControls) {
            closeMenus()
        }
    }

    on_CaptureButtonLocationChanged: inButtonLayout = false

    onIsPortraitChanged: {
        upperHeader.pressedMenu = null
        syncDeviceRotation()
    }


    Component.onCompleted: syncDeviceRotation()

    Connections {
        target: Settings.global
        onAdvancedModeChanged: syncDeviceRotation()
        onCaptureModeChanged: syncDeviceRotation()
    }

    function syncDeviceRotation() {
        var rotations = Settings.rawCaptureRotationModel()
        if (Settings.global.captureMode === "image"
                && Settings.mode.rawCaptureRotation !== deviceRotation
                && rotations.indexOf(deviceRotation) >= 0) {
            Settings.mode.rawCaptureRotation = deviceRotation
        }
    }

    signal clicked(var mouse)

    function closeMenus() {
        _closing = true
        whiteBalanceMenu.open = false
        topMenuOpen = false
        inButtonLayout = false
        _closing = false
    }

    onPinchStarted: _pinchActive = true
    onPinchFinished: _pinchActive = false

    property list<Item> _buttonAnchors
    _buttonAnchors: [
        ButtonAnchor { id: buttonAnchorTL; index: 0; anchors { left: parent.left; top: parent.top } visible: !overlay.isPortrait },
        ButtonAnchor { id: buttonAnchorCL; index: 1; anchors { left: parent.left; verticalCenter: parent.verticalCenter } visible: !overlay.isPortrait },
        ButtonAnchor { id: buttonAnchorBL; index: 2; anchors { left: parent.left; bottom: parent.bottom } },
        ButtonAnchor { id: buttonAnchorBC; index: 3; anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom } visible: overlay.isPortrait },
        ButtonAnchor { id: buttonAnchorBR; index: 4; anchors { right: parent.right; bottom: parent.bottom } },
        ButtonAnchor { id: buttonAnchorCR; index: 5; anchors { right: parent.right; verticalCenter: parent.verticalCenter } visible: !overlay.isPortrait },
        ButtonAnchor { id: buttonAnchorTR; index: 6; anchors { right: parent.right; top: parent.top } visible: !overlay.isPortrait }
    ]

    // Position of other elements given the capture button position
    property var _portraitPositions: [

        // Unused
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorTL
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorCL

        // Used
        { "captureMode": overlayAnchorBR, "camera2CaptureFormat": overlayAnchorBC, "cameraPosition": overlayAnchorBC, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorBL
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorBC
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBC, "cameraPosition": overlayAnchorBC, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorBR

        // Unused
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignBottom }, // buttonAnchorCR
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignBottom }, // buttonAnchorTR
    ]
    property var _landscapePositions: [
        // Used
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorCL, "cameraPosition": overlayAnchorCL, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignLeft   }, // buttonAnchorTL
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorTL, "cameraPosition": overlayAnchorTL, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignLeft   }, // buttonAnchorCL
        { "captureMode": overlayAnchorCL, "camera2CaptureFormat": overlayAnchorTL, "cameraPosition": overlayAnchorTL, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignLeft   }, // buttonAnchorBL

        // Unused
        { "captureMode": overlayAnchorBR, "camera2CaptureFormat": overlayAnchorTR, "cameraPosition": overlayAnchorTR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignRight  }, // buttonAnchorBC

        // Used
        { "captureMode": overlayAnchorCR, "camera2CaptureFormat": overlayAnchorTR, "cameraPosition": overlayAnchorTR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignRight  }, // buttonAnchorBR
        { "captureMode": overlayAnchorBR, "camera2CaptureFormat": overlayAnchorTR, "cameraPosition": overlayAnchorTR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignRight  }, // buttonAnchorCR
        { "captureMode": overlayAnchorBR, "camera2CaptureFormat": overlayAnchorCR, "cameraPosition": overlayAnchorCR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignRight  }, // buttonAnchorTR
    ]

    property var _overlayPosition: overlay.isPortrait ? _portraitPositions[overlay._captureButtonLocation]
                                                      : _landscapePositions[overlay._captureButtonLocation]

    Item {
        id: camera2BottomDeck

        // Photo and video share the floating layout; video always uses the
        // simple deck (the Pro dials are Camera2 photo controls).
        readonly property bool video: Settings.global.captureMode === "video"
        readonly property bool active: Settings.global.captureMode === "image" || video
        readonly property bool pro: Settings.global.advancedMode && !video
        // Floats over the full-screen viewfinder; Simple needs less room.
        readonly property real controlFraction: pro ? 0.40 : 0.19
        readonly property real controlHeight: Math.round(parent.height * controlFraction)
        readonly property real controlWidth: Math.round(parent.width * controlFraction)
        readonly property real headerHeight: pro
                                             ? Theme.fontSizeTiny + Theme.paddingSmall
                                             : 0

        anchors {
            right: parent.right
            bottom: parent.bottom
        }
        z: 2
        width: active ? (overlay.isPortrait ? parent.width : controlWidth) : 0
        height: active ? (overlay.isPortrait ? controlHeight : parent.height) : 0
        visible: active

        // Taps between the controls must not refocus the viewfinder underneath.
        MouseArea {
            anchors.fill: parent
        }

        // Scrim that keeps the floating controls readable over live video.
        Rectangle {
            anchors.fill: parent
            visible: overlay.isPortrait
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.3; color: Theme.rgba("black", 0.3) }
                GradientStop { position: 1.0; color: Theme.rgba("black", 0.6) }
            }
        }
        Rectangle {
            anchors.fill: parent
            visible: !overlay.isPortrait
            color: Theme.rgba("black", 0.45)
        }

        Row {
            id: camera2HeaderRow

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                leftMargin: camera2ControlGrid.sideMargin
                rightMargin: camera2ControlGrid.sideMargin
                topMargin: Theme.paddingSmall
            }
            height: Theme.fontSizeTiny
            spacing: camera2ControlGrid.columnGap
            z: 3
            visible: camera2BottomDeck.pro

            Label {
                width: camera2ControlGrid.leftWidth
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                color: _highlightColor
                opacity: Theme.opacityHigh
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                text: "CAM"
            }

            Label {
                width: camera2ControlGrid.experimentalLayout ? camera2ControlGrid.exposureWidth : camera2ControlGrid.centerWidth
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                color: _highlightColor
                opacity: Theme.opacityHigh
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                text: "EV"
            }

            Label {
                width: camera2ControlGrid.speedWidth
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                color: _highlightColor
                opacity: Theme.opacityHigh
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                text: "SPEED"
                visible: !camera2ControlGrid.experimentalLayout
            }

            Label {
                width: camera2ControlGrid.isoWidth
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                color: _highlightColor
                opacity: Theme.opacityHigh
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                text: "ISO"
                visible: !camera2ControlGrid.experimentalLayout
            }
        }

        Grid {
            id: camera2ControlGrid

            readonly property bool experimentalLayout: Settings.global.experimentalExposureLayout && Settings.global.advancedMode
            readonly property real exposureWidth: centerWidth + speedWidth + isoWidth + 2 * columnGap
            readonly property real wheelRowHeight: Math.max(Theme.fontSizeLarge * 1.6, Math.round(height / 7))
            readonly property real wheelCenterY: shutterZoomArea.y + bottomShutterAnchor.y + bottomShutterAnchor.height / 2
            readonly property real wheelTop: Math.max(0, wheelCenterY - wheelRowHeight / 2)
            readonly property real sideMargin: Math.max(Theme.paddingMedium,
                                                        camera2BottomDeck.width * 0.015)
            readonly property real columnGap: Math.max(1, Math.round(Theme.pixelRatio))
            readonly property real usableWidth: camera2BottomDeck.width
                                                - 2 * sideMargin - 3 * columnGap
            readonly property real leftWidth: Math.round(usableWidth * 0.20)
            readonly property real centerWidth: Math.round(usableWidth * 0.41)
            readonly property real speedWidth: Math.round(usableWidth * 0.19)
            readonly property real isoWidth: usableWidth - leftWidth
                                             - centerWidth - speedWidth
            readonly property var camera2Viewfinder: captureView ? captureView.camera2Viewfinder : null
            visible: camera2BottomDeck.pro
            enabled: visible

            function focalLengthText() {
                var focal = camera2Viewfinder && camera2Viewfinder.focalLength
                        ? camera2Viewfinder.focalLength : 0
                if (focal <= 0) {
                    return "--"
                }
                return focal < 10 ? focal.toFixed(1) : Math.round(focal)
            }

            function liveShutterValue() {
                if (Settings.global.advancedMode && Settings.mode.rawCaptureShutterNs !== "0")
                    return Settings.mode.rawCaptureShutterNs
                return overlay.exposureControl && overlay.exposureControl.fresh
                        ? String(overlay.exposureControl.actualTime) : "—"
            }

            function liveIsoValue() {
                if (Settings.global.advancedMode && Settings.mode.rawCaptureIso > 0)
                    return Settings.mode.rawCaptureIso
                return overlay.exposureControl && overlay.exposureControl.fresh
                        ? overlay.exposureControl.actualIso : "—"
            }

            function drawHistogram(context, width, height, values) {
                context.clearRect(0, 0, width, height)
                if (!values || values.length === 0) {
                    return
                }

                var peak = 1
                for (var index = 0; index < values.length; ++index) {
                    peak = Math.max(peak, values[index].r,
                                    values[index].g, values[index].b)
                }

                function drawChannel(key, color) {
                    context.beginPath()
                    context.moveTo(0, height)
                    for (var i = 0; i < values.length; ++i) {
                        var x = values.length === 1 ? 0
                                : i * width / (values.length - 1)
                        var y = height - values[i][key] / peak * height
                        context.lineTo(x, y)
                    }
                    context.lineTo(width, height)
                    context.closePath()
                    context.fillStyle = color
                    context.fill()
                }

                drawChannel("r", Qt.rgba(1.0, 0.2, 0.15, 0.32))
                drawChannel("g", Qt.rgba(0.2, 1.0, 0.35, 0.28))
                drawChannel("b", Qt.rgba(0.25, 0.55, 1.0, 0.32))
            }

            anchors {
                fill: parent
                leftMargin: sideMargin
                rightMargin: sideMargin
                topMargin: camera2BottomDeck.headerHeight
            }
            columns: 4
            columnSpacing: columnGap

            Column {
                width: camera2ControlGrid.leftWidth
                height: camera2ControlGrid.height

                Item {
                    width: parent.width
                    height: Settings.global.advancedMode
                            ? parent.height / 3
                            : parent.height

                    CameraSwitchButtons {
                        anchors.centerIn: parent
                        availableWidth: parent.width
                        availableHeight: parent.height
                        captureBusy: captureView.captureBusy || !overlay.deviceToggleEnabled
                        onSwitched: captureView.resetZoom()
                    }
                }

                CycleValueButton {
                    width: parent.width
                    height: Settings.global.advancedMode ? parent.height / 3 : 0
                    visible: Settings.global.advancedMode
                    enabled: visible
                    caption: "FORMAT"
                    settings: Settings.mode
                    settingProperty: "camera2CaptureFormat"
                    currentValue: Settings.mode.camera2CaptureFormat
                    model: Settings.camera2CaptureFormatModel()
                    valueLabel: function(value) { return value === "raw" ? "RAW" : "JPG" }
                }

                CycleValueButton {
                    width: parent.width
                    height: Settings.global.advancedMode ? parent.height / 3 : 0
                    visible: Settings.global.advancedMode && model.length > 0
                    enabled: visible
                    caption: "FOCUS"
                    settings: Settings.mode
                    settingProperty: "rawCaptureFocusMode"
                    currentValue: Settings.mode.rawCaptureFocusMode
                    model: Settings.camera2SelectableFocusModeModel()
                    valueLabel: function(value) {
                        if (value === "manual") {
                            return Settings.rawCaptureFocusDistanceLabel(
                                        Settings.mode.rawCaptureFocusDistance)
                        }
                        switch (value) {
                        case "continuous": return "CONT"
                        case "manual": return "MAN"
                        case "infinity": return "INF"
                        case "none": return "OFF"
                        default: return "AUTO"
                        }
                    }
                }
            }

            Column {
                width: camera2ControlGrid.centerWidth
                height: camera2ControlGrid.height
                z: 1

                Item {
                    width: camera2ControlGrid.experimentalLayout ? camera2ControlGrid.exposureWidth : parent.width
                    height: parent.height * 0.34

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.paddingSmall
                        color: Theme.rgba("black", 0.35)
                    }

                    Canvas {
                        id: exposureHistogram

                        anchors.fill: parent
                        opacity: 0.85

                        onPaint: {
                            camera2ControlGrid.drawHistogram(
                                        getContext("2d"), width, height,
                                        camera2ControlGrid.camera2Viewfinder
                                        ? camera2ControlGrid.camera2Viewfinder.histogram : [])
                        }

                        Connections {
                            target: camera2ControlGrid.camera2Viewfinder
                            onHistogramChanged: exposureHistogram.requestPaint()
                        }
                    }

                    ValueCarousel {
                        anchors.fill: parent
                        visible: !overlay.exposureControl || !overlay.exposureControl.fullyManual
                        enabled: visible && overlay.exposureControl && !captureView.captureBusy
                                 && (overlay.exposureControl.partial || overlay.exposureControl.capabilities.compensation_step_ev > 0)
                        orientation: ListView.Horizontal
                        settings: Settings.global
                        writeThrough: false
                        onValueSelected: Settings.global.exposureCompensation = value
                        settingProperty: "exposureCompensation"
                        currentValue: Settings.global.exposureCompensation
                        displayValue: overlay.exposureControl ? overlay.exposureControl.effectiveEv * 2 : currentValue
                        model: overlay.evChoices()
                        valueLabel: function(value) { return overlay.evLabel(value) }
                    }
                    Label {
                        anchors.centerIn: parent
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        font.pixelSize: Theme.fontSizeSmall
                        visible: Settings.global.estimatedExposureMetering && overlay.exposureControl && overlay.exposureControl.fullyManual
                        text: overlay.exposureControl && overlay.exposureControl.meterValid && overlay.exposureControl.fresh
                              ? "≈ " + overlay.exposureControl.meterEv.toFixed(1) + " EV"
                              : "—"
                    }
                }

                Item {
                    id: shutterZoomArea
                    width: parent.width
                    height: parent.height * 0.66

                    Item {
                        id: bottomShutterAnchor

                        width: Math.min(parent.width, Theme.itemSizeExtraLarge * 1.18,
                                        Math.max(0, parent.height - 2 * Theme.itemSizeSmall))
                        height: width
                        anchors.centerIn: parent
                    }

                    Repeater {
                        model: [1, -1]

                        MouseArea {
                            readonly property int direction: modelData
                            width: parent.width
                            height: (shutterZoomArea.height - bottomShutterAnchor.height) / 2
                            y: direction > 0 ? 0 : bottomShutterAnchor.y + bottomShutterAnchor.height
                            enabled: overlay.zoomButtonsEnabled
                                     && (direction > 0 ? overlay.currentZoom < overlay.maximumZoom
                                                       : overlay.currentZoom > 1)
                            onPressed: overlay.zoomPressed(direction)
                            onDoubleClicked: if (direction < 0) overlay.zoomResetRequested()
                            onReleased: overlay.zoomReleased()
                            onCanceled: overlay.zoomCanceled()
                            onExited: if (pressed) overlay.zoomCanceled()

                            Label {
                                anchors.centerIn: parent
                                text: direction > 0 ? "+" : "−"
                                font.pixelSize: Theme.fontSizeLarge
                                color: parent.pressed ? Theme.highlightColor : Theme.primaryColor
                                opacity: parent.enabled ? 1 : Theme.opacityLow
                            }
                        }
                    }
                }
            }

            Item {
                width: camera2ControlGrid.speedWidth
                height: camera2ControlGrid.height

                ValueCarousel {
                    id: speedWheel
                    width: parent.width
                    y: camera2ControlGrid.experimentalLayout ? camera2ControlGrid.wheelTop : 0
                    height: parent.height - y
                    selectionAtTop: camera2ControlGrid.experimentalLayout
                    rowHeight: camera2ControlGrid.wheelRowHeight
                    visible: Settings.global.advancedMode
                    enabled: visible && overlay.exposureControl && overlay.exposureControl.active && !overlay.exposureControl.busy
                    orientation: ListView.Vertical
                    caption: ""
                    writeThrough: false
                    onValueSelected: { if (!overlay.exposureControl.setLock("shutter", value)) rejectSelection() }
                    settings: Settings.mode
                    settingProperty: "rawCaptureShutterNs"
                    currentValue: Settings.mode.rawCaptureShutterNs
                    displayValue: camera2ControlGrid.liveShutterValue()
                    model: Settings.camera2ShutterModel()
                    valueLabel: function(value) { return value === "—" ? value : Settings.rawCaptureShutterLabel(value) }
                    selectedFontSize: Theme.fontSizeExtraLarge
                    tapered: true
                    wrap: true
                }

                PreviewAidToggle {
                    anchors { bottom: speedWheelHeader.top; horizontalCenter: parent.horizontalCenter; bottomMargin: Theme.paddingSmall }
                    width: parent.width
                    visible: camera2ControlGrid.experimentalLayout
                    enabled: visible && camera2ControlGrid.camera2Viewfinder && camera2ControlGrid.camera2Viewfinder.running
                    text: "PEAK"
                    checked: Settings.global.focusPeaking
                    onClicked: Settings.global.focusPeaking = !Settings.global.focusPeaking
                }

                Label {
                    id: speedWheelHeader
                    visible: camera2ControlGrid.experimentalLayout
                    anchors { bottom: speedWheel.top; horizontalCenter: parent.horizontalCenter; bottomMargin: Theme.paddingSmall }
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Theme.fontSizeTiny
                    font.bold: true
                    color: _highlightColor
                    text: "SPEED"
                }


                Label {
                    anchors.centerIn: parent
                    width: parent.width - 2 * Theme.paddingSmall
                    visible: !Settings.global.advancedMode
                    horizontalAlignment: Text.AlignHCenter
                    truncationMode: TruncationMode.Fade
                    color: Theme.lightPrimaryColor
                    font.pixelSize: Theme.fontSizeLarge
                    font.bold: true
                    text: Settings.rawCaptureShutterLabel(
                              camera2ControlGrid.liveShutterValue())
                }
            }

            Item {
                width: camera2ControlGrid.isoWidth
                height: camera2ControlGrid.height

                ValueCarousel {
                    id: isoWheel
                    width: parent.width
                    y: camera2ControlGrid.experimentalLayout ? camera2ControlGrid.wheelTop : 0
                    height: parent.height - y
                    selectionAtTop: camera2ControlGrid.experimentalLayout
                    rowHeight: camera2ControlGrid.wheelRowHeight
                    visible: Settings.global.advancedMode
                    enabled: visible && overlay.exposureControl && overlay.exposureControl.active && !overlay.exposureControl.busy
                    orientation: ListView.Vertical
                    caption: ""
                    writeThrough: false
                    onValueSelected: { if (!overlay.exposureControl.setLock("iso", value)) rejectSelection() }
                    settings: Settings.mode
                    settingProperty: "rawCaptureIso"
                    currentValue: Settings.mode.rawCaptureIso
                    displayValue: camera2ControlGrid.liveIsoValue()
                    model: Settings.camera2IsoModel()
                    valueLabel: function(value) { return value === "—" ? value : value > 0 ? value : "Auto" }
                    selectedFontSize: Theme.fontSizeExtraLarge
                    tapered: true
                    wrap: true
                }

                PreviewAidToggle {
                    anchors { bottom: isoWheelHeader.top; horizontalCenter: parent.horizontalCenter; bottomMargin: Theme.paddingSmall }
                    width: parent.width
                    visible: camera2ControlGrid.experimentalLayout
                    enabled: visible && camera2ControlGrid.camera2Viewfinder && camera2ControlGrid.camera2Viewfinder.running
                    text: "ZEBRA"
                    checked: Settings.global.exposureZebras
                    onClicked: Settings.global.exposureZebras = !Settings.global.exposureZebras
                }

                Label {
                    id: isoWheelHeader
                    visible: camera2ControlGrid.experimentalLayout
                    anchors { bottom: isoWheel.top; horizontalCenter: parent.horizontalCenter; bottomMargin: Theme.paddingSmall }
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Theme.fontSizeTiny
                    font.bold: true
                    color: _highlightColor
                    text: "ISO"
                }


                Label {
                    anchors.centerIn: parent
                    width: parent.width - 2 * Theme.paddingSmall
                    visible: !Settings.global.advancedMode
                    horizontalAlignment: Text.AlignHCenter
                    truncationMode: TruncationMode.Fade
                    color: Theme.lightPrimaryColor
                    font.pixelSize: Theme.fontSizeLarge
                    font.bold: true
                    text: {
                        var iso = camera2ControlGrid.liveIsoValue()
                        return iso > 0 ? iso : "Auto"
                    }
                }
            }
        }

        Item {
            id: simpleCamera2Deck

            // Normal mode: no histogram, exposure readouts or EV, just the
            // last photo, the shutter and the lens.
            anchors {
                fill: parent
                leftMargin: camera2ControlGrid.sideMargin
                rightMargin: camera2ControlGrid.sideMargin
                topMargin: Theme.paddingSmall
                bottomMargin: Theme.paddingSmall
            }
            visible: !camera2BottomDeck.pro
            enabled: visible

            // Pixel-style row: last capture, shutter, lens.
            Row {
                anchors.fill: parent
                readonly property real sideWidth: (width - camera2ControlGrid.centerWidth) / 2

                Item {
                    width: parent.sideWidth
                    height: parent.height

                    GalleryThumbnail {
                        anchors.centerIn: parent
                        width: Math.min(Theme.itemSizeMedium, parent.width - Theme.paddingLarge,
                                        parent.height - Theme.paddingLarge)
                        height: width
                        captureModel: captureView ? captureView.captureModel : null
                        busy: !!captureView && captureView.processing
                        pendingSource: captureView ? captureView.pendingThumbnail : ""
                        // Hidden while recording, like the camera switch.
                        visible: !captureView || !captureView.recording
                        onClicked: captureView.galleryRequested()
                    }
                }

                Item {
                    width: camera2ControlGrid.centerWidth
                    height: parent.height

                    Item {
                        id: simpleBottomShutterAnchor

                        // Same size as the Advanced shutter.
                        width: Math.min(parent.width, parent.height, Theme.itemSizeExtraLarge * 1.18)
                        height: width
                        anchors.centerIn: parent
                    }
                }

                Item {
                    width: parent.sideWidth
                    height: parent.height

                    CameraSwitchButtons {
                        anchors.centerIn: parent
                        availableWidth: parent.width
                        availableHeight: parent.height
                        captureBusy: captureView.captureBusy || !overlay.deviceToggleEnabled
                        // Video records through Qt Multimedia, with its own camera ids.
                        qtCamera: camera2BottomDeck.video
                        visible: !captureView.recording
                        onSwitched: captureView.resetZoom()
                    }
                }
            }
        }

    }

    // 1x / 2x presets just above the shutter deck (left of it in landscape).
    // Positioned with x/y: anchors toggled per orientation can briefly
    // combine right and horizontalCenter, which QML rejects, stranding the
    // pill off screen after a few rotations.
    ZoomSwitch {
        x: overlay.isPortrait ? camera2BottomDeck.x + (camera2BottomDeck.width - width) / 2
                              : camera2BottomDeck.x - width - Theme.paddingMedium
        y: overlay.isPortrait ? captureModeTabs.y - height - Theme.paddingSmall
                              : camera2BottomDeck.y + (camera2BottomDeck.height - height) / 2
        width: implicitWidth
        height: implicitHeight
        z: 2
        visible: camera2BottomDeck.active && !!captureView
                 && (captureView._camera2ViewfinderActive || camera2BottomDeck.video)
                 && overlay.maximumZoom >= 2 && overlay.settingsOpacity < 0.5
        enabled: !!captureView && !captureView.captureBusy
        zoom: overlay.currentZoom
        onSelected: overlay.zoomPresetRequested(value)
    }

    // PHOTO | VIDEO: above the shutter deck in photo mode (inside its top in
    // landscape), near the bottom edge in video mode.
    CaptureModeTabs {
        id: captureModeTabs

        x: camera2BottomDeck.active
           ? camera2BottomDeck.x + (camera2BottomDeck.width - width) / 2
           : (overlay.width - width) / 2
        y: camera2BottomDeck.active
           ? (overlay.isPortrait ? camera2BottomDeck.y - height - Theme.paddingSmall
                                 : camera2BottomDeck.y + Theme.paddingLarge)
           : overlay.height - height - Theme.itemSizeExtraLarge * 1.5
        z: 2
        visible: opacity > 0.0 && !!captureView && !captureView.recording && !overlay.inButtonLayout
        opacity: overlay.settingsOpacity < 0.5 ? 1.0 : 0.0
        Behavior on opacity { FadeAnimation {} }
        enabled: !!captureView && !captureView.captureBusy
        mode: Settings.global.captureMode
        onSelected: overlay.selectCaptureMode(mode)
    }

    // Photo-mode controls floating along the top edge of the viewfinder.
    Item {
        id: floatingTopBar

        // Burst support depends on the selected camera's RAW capabilities.
        property bool burstAvailable
        function updateBurstAvailable() {
            burstAvailable = Settings.camera2BurstSupported()
        }
        Component.onCompleted: updateBurstAvailable()

        Connections {
            target: Settings
            onDeviceIdChanged: floatingTopBar.updateBurstAvailable()
        }

        z: 3
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            topMargin: overlay._safeTopMargin + Theme.paddingMedium
            leftMargin: Theme.horizontalPageMargin
            // Landscape: only over the viewfinder, never over the deck column
            // on the right, whose top holds the PHOTO | VIDEO tabs.
            rightMargin: Theme.horizontalPageMargin
                         + (overlay.isPortrait || !camera2BottomDeck.active ? 0 : camera2BottomDeck.width)
        }
        height: Theme.itemSizeExtraSmall
        opacity: overlay._exposed ? 0.0 : _commonControlOpacity
        // Recording shows its timer up here instead.
        visible: camera2BottomDeck.active && opacity > 0.0 && !(captureView && captureView.recording)
        Behavior on opacity { FadeAnimation {} }

        Row {
            anchors.left: parent.left
            height: parent.height
            spacing: Theme.paddingMedium

            // Opens the settings panel (formerly a tap on the strip above the viewfinder).
            FloatingChip {
                height: parent.height
                enabled: !overlay.inButtonLayout
                onClicked: overlay.topMenuOpen = true
            }

            // Normal hides the histogram and exposure readouts; Pro shows them all.
            ModeSwitch {
                height: parent.height
                visible: !camera2BottomDeck.video
                enabled: !captureView.captureBusy
                pro: Settings.global.advancedMode
                onSelected: Settings.global.advancedMode = pro
            }
        }

        Row {
            anchors.right: parent.right
            height: parent.height
            spacing: Theme.paddingMedium

            // Night mode: offered when the scene is too dark, on by a tap.
            FloatingChip {
                id: nightChip

                height: parent.height
                visible: floatingTopBar.burstAvailable && !camera2BottomDeck.video
                         && (captureView.sceneDark || Settings.nightMode)
                enabled: !captureView.captureBusy
                checked: Settings.nightMode
                menuDots: false
                onClicked: Settings.nightMode = !Settings.nightMode

                MoonIcon {
                    anchors.centerIn: parent
                    lit: Settings.nightMode
                }
            }

            // Selfies saved mirrored, as the viewfinder shows them (lit), or
            // as others see the scene. Front camera only.
            FloatingChip {
                height: parent.height
                visible: !camera2BottomDeck.video && captureView._camera2ViewfinderActive
                         && captureView.camera2Viewfinder !== null
                         && captureView.camera2Viewfinder.selfieCamera
                checked: Settings.global.mirrorSelfies
                menuDots: false
                onClicked: Settings.global.mirrorSelfies = !Settings.global.mirrorSelfies

                Image {
                    anchors.centerIn: parent
                    width: Theme.iconSizeSmallPlus
                    height: width
                    sourceSize: Qt.size(width, height)
                    source: "../capture/mirror.svg"
                }
            }

            FloatingChip {
                height: parent.height
                visible: floatingTopBar.burstAvailable && !camera2BottomDeck.video
                enabled: !captureView.captureBusy
                text: "HDR"
                checked: Settings.global.hdrBurst
                onClicked: Settings.global.hdrBurst = !Settings.global.hdrBurst
            }

            // Electronic stabilization for ProCam's own video recorder. The
            // bar hides while recording, so it never changes mid-clip (that
            // restarts the camera session).
            FloatingChip {
                height: parent.height
                visible: camera2BottomDeck.video && captureView._camera2VideoActive
                checked: Settings.global.videoStabilization
                menuDots: false
                onClicked: Settings.global.videoStabilization = !Settings.global.videoStabilization

                Image {
                    anchors.centerIn: parent
                    width: Theme.iconSizeSmallPlus
                    height: width
                    sourceSize: Qt.size(width, height)
                    source: Settings.global.videoStabilization
                            ? "../capture/stabilization.svg" : "../capture/stabilization-off.svg"
                }
            }

            FloatingChip {
                height: parent.height
                // Fit shows exactly the photo's frame.
                text: Settings.global.viewfinderFill ? "Full" : Settings.photoAspect
                onClicked: Settings.global.viewfinderFill = !Settings.global.viewfinderFill
            }

        }
    }

    Item {
        id: shutterContainer

        parent: camera2BottomDeck.active
                ? (camera2BottomDeck.pro ? bottomShutterAnchor
                                         : simpleBottomShutterAnchor)
                                         : overlay._buttonAnchors[overlay._captureButtonLocation]
        anchors.fill: parent
    }

    CameraDeviceToggle {
        onSelected: {
            Settings.deviceId = deviceId
            captureView.resetZoom()
        }

        parent: {
            switch (_overlayPosition.backCameraToggle) {
            case Qt.AlignBottom:
                return overlayAnchorBC
            case Qt.AlignLeft:
                return overlayAnchorCL
            default:
            case Qt.AlignRight:
                return overlayAnchorCR
            }
        }

        opacity: _commonControlOpacity
        labels: Settings.global.backCameraLabels
        visible: opacity > 0.0
                 && !!model && model.length > 1
                 && labels.length > 0
                 && Settings.deviceId !== Settings.global.frontFacingDeviceId
                 && !camera2BottomDeck.active
                 && !inButtonLayout
        orientation: overlay.isPortrait ? Qt.Horizontal : Qt.Vertical
        enabled: camera.cameraStatus === Camera.ActiveStatus
                 || captureView._camera2ViewfinderActive
        model: camera.backFacingCameras
        currentDeviceId: camera.deviceId

        x: {
            if (_overlayPosition.backCameraToggle === Qt.AlignLeft) {
                return parent.width + Theme.paddingLarge
            } else if (_overlayPosition.backCameraToggle === Qt.AlignRight) {
                return -width - (isPortrait ? 1 : 2) * Theme.paddingLarge
            }

            return parent.width/2 - width/2
        }

        y: {
            if (_overlayPosition.backCameraToggle & Qt.AlignBottom) {
                return -height - (overlay.isPortrait ? 2 : 1) * Theme.paddingLarge
            }

            return parent.height/2 - height/2
        }
    }

    ToggleButton {
        parent: _overlayPosition.cameraPosition
        anchors.centerIn: parent
        icon: "image://theme/icon-camera-switch"
        opacity: _commonControlOpacity
        // Photo mode switches cameras with the lens selector in the deck.
        visible: opacity > 0.0 && camera.hasCameraOnBothSides && !camera2BottomDeck.active
        enabled: overlay.deviceToggleEnabled

        onClicked: {
            if (Settings.global.position === Camera.BackFace) {
                Settings.deviceId = Settings.global.frontFacingDeviceId
            } else {
                Settings.deviceId = Settings.global.previousBackFacingDeviceId
            }

            captureView.resetZoom()
        }
    }

    CaptureModeMenu {
        id: captureModeMenu

        property real itemStep: Theme.itemSizeExtraSmall + spacing

        parent: _overlayPosition.captureMode
        anchors.verticalCenterOffset: height/2
        alignment: (parent.anchors.left === container.left ? Qt.AlignRight
                                                           : Qt.AlignLeft) | Qt.AlignBottom
        open: true
        opacity: _commonControlOpacity
        visible: opacity > 0.0 && !camera2BottomDeck.active

        Rectangle {
            id: captureModeHighlight

            z: -1
            width: Theme.itemSizeExtraSmall
            height: Theme.itemSizeExtraSmall
            anchors.horizontalCenter: parent.horizontalCenter
            radius: width / 2
            color: Theme.rgba(_highlightColor, Theme.opacityLow)
            opacity: y < -captureModeMenu.itemStep ? 1.0 - (captureModeMenu.itemStep + y) / (-captureModeMenu.itemStep/2)
                                                   : (y > 0 ? 1.0 - y / (captureModeMenu.itemStep/2) : 1.0)
            y: captureModeMenu.currentIndex == 0 ? -captureModeMenu.itemStep : 0
            Behavior on y {
                id: captureModeBehavior

                YAnimator { duration: 400; easing.type: Easing.OutQuad }
            }
        }
    }

    Camera2CaptureFormatMenu {
        id: camera2CaptureFormatMenu

        property real itemStep: Theme.itemSizeExtraSmall + spacing

        parent: _overlayPosition.camera2CaptureFormat
        anchors.verticalCenterOffset: overlay.isPortrait
                                     ? height/2 - Theme.itemSizeMedium - Theme.paddingSmall
                                     : height/2
        anchors.horizontalCenterOffset: overlay.isPortrait
                                        ? 0
                                        : (parent.anchors.left === container.left
                                           ? Theme.itemSizeMedium + Theme.paddingSmall
                                           : -Theme.itemSizeMedium - Theme.paddingSmall)
        alignment: (parent.anchors.left === container.left ? Qt.AlignRight
                                                           : Qt.AlignLeft) | Qt.AlignBottom
        open: true
        opacity: _commonControlOpacity
        visible: opacity > 0.0
                 && Settings.global.advancedMode
                 && !camera2BottomDeck.active
                 && camera.captureMode === Camera.CaptureStillImage
                 && captureView.camera2CaptureAvailable

        Rectangle {
            z: -1
            width: Theme.itemSizeExtraSmall
            height: Theme.itemSizeExtraSmall
            anchors.horizontalCenter: parent.horizontalCenter
            radius: width / 2
            color: Theme.rgba(_highlightColor, Theme.opacityLow)
            opacity: y < -camera2CaptureFormatMenu.itemStep ? 1.0 - (camera2CaptureFormatMenu.itemStep + y) / (-camera2CaptureFormatMenu.itemStep/2)
                                                            : (y > 0 ? 1.0 - y / (camera2CaptureFormatMenu.itemStep/2) : 1.0)
            y: camera2CaptureFormatMenu.currentIndex == 0 ? -camera2CaptureFormatMenu.itemStep : 0
            Behavior on y {
                YAnimator { duration: 400; easing.type: Easing.OutQuad }
            }
        }
    }

    MouseArea {
        id: dragArea

        property real _lastPos
        property real _direction
        property int _extraDragMargin: 0

        anchors.fill: parent
        enabled: !overlay.inButtonLayout && showCommonControls
        drag {
            target: panel
            minimumY: -panel.height
            maximumY: _extraDragMargin
            axis: Drag.YAxis
            filterChildren: true
            onActiveChanged: {
                if (!drag.active) {
                    if (panel.y - _extraDragMargin < -(panel.height / 3) && _direction <= 0) {
                        overlay.topMenuOpen = false
                    } else if (panel.y > (-panel.height * 2 / 3) && _direction >= 0) {
                        overlay.topMenuOpen = true
                    }
                    expandBehavior.enabled = true
                    panel.updateY()
                    expandBehavior.enabled = false
                }
            }
        }

        onPressed: {
            _direction = 0
            _lastPos = panel.y
        }
        onPositionChanged: {
            var pos = panel.y
            _direction = (_direction + pos - _lastPos) / 2
            _lastPos = panel.y
        }

        MouseArea {
            id: container

            property real pressX
            property real pressY
            property bool swiped

            function outOfBounds(mouseX, mouseY) {
                if (captureView && captureView._camera2ViewfinderActive) {
                    return mouseY < captureView.camera2TopInset
                }
                return mouseX < Theme.paddingLarge || mouseX > width - Theme.paddingLarge
                        || mouseY < Theme.paddingLarge || mouseY > height - Theme.paddingLarge
            }

            anchors.fill: parent
            opacity: Math.min(1 - overlay._progress, 1 - anchorContainer.opacity)
            enabled: !overlay._pinchActive && showCommonControls

            onPressed: {
                pressX = mouseX
                pressY = mouseY
                swiped = false
            }

            onReleased: {
                if (overlay.topMenuOpen || overlay.inButtonLayout || outOfBounds(pressX, pressY)) return
                var dx = mouseX - pressX, dy = mouseY - pressY
                if (Math.max(Math.abs(dx), Math.abs(dy)) < Theme.itemSizeSmall) return
                if (Math.abs(dy) > 1.5 * Math.abs(dx)) {
                    swiped = true
                    overlay.flipCamera()
                } else if (Math.abs(dx) > 1.5 * Math.abs(dy)) {
                    swiped = true
                    // Like Android: swiping left moves on to video.
                    overlay.selectCaptureMode(dx < 0 ? "video" : "image")
                }
            }

            onClicked: {
                // A swipe is not a tap (no focus point at its end).
                if (swiped) return
                if (overlay.topMenuOpen) {
                    overlay.topMenuOpen = false
                }
                // don't react near display edges
                if (outOfBounds(mouseX, mouseY))
                    return
                if (whiteBalanceMenu.expanded) {
                    whiteBalanceMenu.open = false
                } else if (overlay.inButtonLayout) {
                    overlay.inButtonLayout = false
                } else {
                    overlay.clicked(mouse)
                }
            }

            onPressAndHold: {
                // don't react near display edges
                if (outOfBounds(mouseX, mouseY)) return
                if (!overlay.topMenuOpen) {
                    var dragDistance = Math.max(Math.abs(mouseX - pressX),
                                                Math.abs(mouseY - pressY))
                    if (dragDistance < Theme.startDragDistance) {
                        overlay.inButtonLayout = true
                    }
                }
            }

            MouseArea {
                anchors {
                    top: parent.top
                    left: parent.left
                    right: parent.right
                }
                height: captureView && captureView._camera2ViewfinderActive
                        ? captureView.camera2TopInset
                        : Math.max(Theme.itemSizeLarge,
                                   topRow._topRowMargin + Theme.iconSizeMedium)
                enabled: !overlay._exposed && !overlay.inButtonLayout && showCommonControls

                onClicked: overlay.topMenuOpen = true

                onPressAndHold: container.pressAndHold(mouse)
            }

            OverlayAnchor { id: overlayAnchorBL; anchors { left: parent.left; bottom: parent.bottom } }
            OverlayAnchor { id: overlayAnchorBC; anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom } }
            OverlayAnchor { id: overlayAnchorBR; anchors { right: parent.right; bottom: parent.bottom } }
            OverlayAnchor { id: overlayAnchorCL; anchors { left: parent.left; verticalCenter: parent.verticalCenter } }
            OverlayAnchor { id: overlayAnchorCR; anchors { right: parent.right; verticalCenter: parent.verticalCenter } }
            OverlayAnchor { id: overlayAnchorTL; anchors { left: parent.left; top: parent.top } }
            OverlayAnchor { id: overlayAnchorTR; anchors { right: parent.right; top: parent.top } }
        }

        MouseArea {
            anchors.fill: parent
            enabled: overlay._exposed
            onClicked: overlay.topMenuOpen = false
        }

        Item {
            id: panel

            y: -panel.height

            function updateY() {
                if (!dragArea.drag.active) {
                    if (overlay.topMenuOpen) {
                        panel.y = dragArea._extraDragMargin
                    } else {
                        panel.y = -panel.height
                    }
                }
            }

            Connections {
                target: overlay
                onIsPortraitChanged: panel.updateY()
                onTopMenuOpenChanged: {
                    expandBehavior.enabled = true
                    panel.updateY()
                    expandBehavior.enabled = false
                }
            }

            Behavior on y {
                id: expandBehavior

                enabled: false
                NumberAnimation {
                    id: verticalAnimation
                    duration: 200; easing.type: Easing.InOutQuad
                }
            }

            width: overlay.width
            height: Screen.width / 2
        }

        Rectangle {
            anchors.fill: parent
            visible: overlay._exposed
            color: "black"
            opacity: Theme.opacityHigh * (1 - container.opacity)
        }

        Item {
            id: grid

            readonly property bool camera2Still: Settings.global.captureMode === "image"
            readonly property bool rawCapture: camera2Still
                                               && Settings.mode.camera2CaptureFormat === "raw"
            readonly property bool camera2FocusSupported: Settings.camera2SelectableFocusModeModel().length > 0
            readonly property bool rawAutoFocus: rawCapture
                                                 && camera2FocusSupported
                                                 && (Settings.mode.rawCaptureFocusMode === "auto"
                                                     || Settings.mode.rawCaptureFocusMode === "continuous")
            readonly property bool filterActive: Settings.global.colorFiltersAllowed
                                                 && colorFilter.supportedFilters.length > 1
            readonly property var itemKeys: currentItemKeys()
            readonly property int count: itemKeys.length
            readonly property real spacing: overlay._menuItemHorizontalSpacing
            readonly property real contentWidth: Math.round(width * (overlay.isPortrait ? 0.88 : 0.92))
            readonly property real menuWidth: overlay._menuWidth
            readonly property real settingsAreaWidth: camera2BottomDeck.active && !overlay.isPortrait
                                                      ? Math.max(menuWidth,
                                                                 Math.round((width - camera2BottomDeck.width) * 0.90))
                                                      : contentWidth
            readonly property real sceneWidth: Math.min(settingsAreaWidth, menuWidth * 5 + spacing * 4)
            readonly property real gridMaximumHeight: settingsPager.visible ? settingsPager.height : settingsFlickable.height
            readonly property var pages: pagedItemKeys()

            width: parent.width
            height: parent.height
            anchors.centerIn: parent

            opacity: 1 - container.opacity
            enabled: overlay._exposed
            visible: overlay._exposed

            function currentItemKeys() {
                if (captureView._camera2VideoActive) {
                    // ProCam's recorder ignores gst-droid's flash, ISO and
                    // exposure modes.
                    return Settings.camera2VideoResolutions(captureView.camera2Viewfinder.cameraId).length > 1
                            ? [ "timer", "resolution", "codec", "grid" ]
                            : [ "timer", "codec", "grid" ]
                }
                if (camera2Still) {
                    if (!Settings.global.advancedMode) {
                        return simpleCamera2ItemKeys()
                    }
                    if (rawCapture) {
                        return rawItemKeys()
                    }
                    return camera2JpegItemKeys()
                }
                return legacyItemKeys()
            }

            function simpleCamera2ItemKeys() {
                var keys = [ "timer", "ratio" ]
                if (rawFilesAvailable) keys.push("rawSave")
                keys.push("advanced")
                keys.push("grid")
                return keys
            }

            // Cameras without RAW (YUV bursts) have no RAW files to keep.
            readonly property bool rawFilesAvailable: Settings.camera2BurstFormat(Settings.deviceId) !== "yuv"

            function legacyItemKeys() {
                var keys = [ "timer" ]
                if (CameraConfigs.supportedFlashModes.length > 0) {
                    keys.push("flash")
                }
                if (experimentalModes.value && CameraConfigs.supportedExposureModes.length > 0) {
                    keys.push("exposure")
                } else if (CameraConfigs.supportedIsoSensitivities.length === 0) {
                    keys.push("exposure")
                }
                if (CameraConfigs.supportedIsoSensitivities.length > 0) {
                    keys.push("iso")
                }
                keys.push("grid")
                if (filterActive) {
                    keys.push("filter")
                }
                return keys
            }

            function rawItemKeys() {
                var keys = [ "timer", "ratio" ]
                if (rawFilesAvailable) keys.push("rawSave")
                keys = keys.concat([ "advanced", "speed", "size", "quality", "timeout" ])
                if (camera2FocusSupported) {
                    keys.push("focus")
                }
                if (camera2FocusSupported && Settings.mode.rawCaptureFocusMode === "manual") {
                    keys.push("distance")
                }
                if (rawAutoFocus) {
                    keys.push("afWait")
                }
                keys.push("scene")
                keys.push("noise")
                keys.push("bracket")
                keys.push("render")
                keys.push("rawFormat")
                keys.push("progressive")
                keys.push("rawExposure")
                keys.push("wb")
                keys.push("tint")
                keys.push("grid")
                return keys
            }

            function camera2JpegItemKeys() {
                var keys = [ "timer", "ratio", "advanced", "speed", "size", "quality" ]
                if (camera2FocusSupported) {
                    keys.push("focus")
                }
                if (camera2FocusSupported && Settings.mode.rawCaptureFocusMode === "manual") {
                    keys.push("distance")
                }
                keys.push("scene")
                keys.push("noise")
                keys.push("bracket")
                keys.push("rawExposure")
                keys.push("grid")
                return keys
            }

            function componentForKey(key) {
                switch (key) {
                case "timer": return timerSettingComponent
                case "flash": return flashSettingComponent
                case "exposure": return exposureSettingComponent
                case "iso": return isoSettingComponent
                case "filter": return filterSettingComponent
                case "advanced": return advancedModeSettingComponent
                case "ratio": return aspectSettingComponent
                case "codec": return videoCodecSettingComponent
                case "resolution": return videoResolutionSettingComponent
                case "rawSave": return rawSaveSettingComponent
                case "speed": return speedSettingComponent
                case "size": return sizeSettingComponent
                case "quality": return qualitySettingComponent
                case "rotate": return rotateSettingComponent
                case "timeout": return timeoutSettingComponent
                case "focus": return focusSettingComponent
                case "distance": return focusDistanceSettingComponent
                case "afWait": return focusTimeoutSettingComponent
                case "scene": return sceneSettingComponent
                case "noise": return noiseReductionSettingComponent
                case "bracket": return bracketSettingComponent
                case "render": return rawRenderEngineSettingComponent
                case "rawFormat": return rawFormatSettingComponent
                case "progressive": return progressiveJpegSettingComponent
                case "rawExposure": return rawExposureSettingComponent
                case "wb": return whiteBalanceSettingComponent
                case "tint": return tintSettingComponent
                default: return gridSettingComponent
                }
            }

            function itemWidthForKey(key) {
                return ["scene", "distance", "size"].indexOf(key) >= 0 ? sceneWidth : menuWidth
            }

            function pagedItemKeys() {
                var pages = []
                var page = []
                var pageWidth = 0
                for (var i = 0; i < itemKeys.length; ++i) {
                    var key = itemKeys[i]
                    var itemWidth = itemWidthForKey(key)
                    var nextWidth = pageWidth + (page.length > 0 ? spacing : 0) + itemWidth
                    if (page.length > 0 && nextWidth > settingsAreaWidth) {
                        pages.push(page)
                        page = []
                        pageWidth = 0
                        nextWidth = itemWidth
                    }
                    page.push(key)
                    pageWidth = nextWidth
                }
                if (page.length > 0) {
                    pages.push(page)
                }
                return pages
            }

            Flickable {
                id: settingsFlickable

                width: grid.contentWidth
                height: Math.round(parent.height * (overlay.isPortrait ? 0.58 : 0.72))
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: overlay._safeTopMargin
                contentWidth: settingsRow.width
                contentHeight: settingsRow.height
                flickableDirection: Flickable.HorizontalFlick
                interactive: contentWidth > width
                visible: overlay.isPortrait || !camera2BottomDeck.active

                Row {
                    id: settingsRow

                    x: Math.max(0, (settingsFlickable.width - width) / 2)
                    anchors.verticalCenter: parent.verticalCenter
                    height: childrenRect.height
                    spacing: grid.spacing

                    Repeater {
                        model: grid.itemKeys

                        Loader {
                            width: grid.itemWidthForKey(modelData)
                            height: item ? item.implicitHeight : Theme.itemSizeMedium
                            sourceComponent: grid.componentForKey(modelData)

                            onLoaded: item.width = width
                        }
                    }
                }
            }

            ListView {
                id: settingsPager

                width: grid.settingsAreaWidth
                height: Math.round(parent.height * 0.66)
                x: Math.round((parent.width - camera2BottomDeck.width - width) / 2)
                y: Math.round((parent.height - height) / 2 + overlay._headerHeight / 2)
                orientation: ListView.Horizontal
                model: grid.pages
                interactive: visible && count > 1
                visible: camera2BottomDeck.active && !overlay.isPortrait
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                snapMode: ListView.SnapOneItem
                highlightMoveDuration: 160
                onCountChanged: currentIndex = Math.min(currentIndex, Math.max(0, count - 1))

                delegate: Item {
                    readonly property var pageKeys: modelData

                    width: settingsPager.width
                    height: settingsPager.height

                    Row {
                        id: settingsRow

                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                        }
                        height: childrenRect.height
                        spacing: grid.spacing

                        Repeater {
                            model: pageKeys

                            Loader {
                                width: grid.itemWidthForKey(modelData)
                                height: item ? item.implicitHeight : Theme.itemSizeMedium
                                sourceComponent: grid.componentForKey(modelData)

                                onLoaded: item.width = width
                            }
                        }
                    }
                }
            }

            Row {
                anchors {
                    horizontalCenter: settingsPager.horizontalCenter
                    top: settingsPager.bottom
                    topMargin: Theme.paddingSmall
                }
                spacing: Theme.paddingSmall
                visible: settingsPager.visible && settingsPager.count > 1

                Repeater {
                    model: settingsPager.count

                    Rectangle {
                        width: Theme.paddingSmall
                        height: width
                        radius: width / 2
                        color: index === settingsPager.currentIndex
                               ? overlay._highlightColor : Theme.lightPrimaryColor
                        opacity: index === settingsPager.currentIndex
                                 ? Theme.opacityHigh : Theme.opacityLow
                    }
                }
            }

            Component {
                id: timerSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.timerText
                    caption: "Timer"
                    header: upperHeader
                    model: [ 0, 3, 10, 15 ]
                    delegate: SettingsMenuItem {
                        settings: Settings.mode
                        property: "timer"
                        value: modelData
                        icon: Settings.timerIcon(modelData)
                    }
                }
            }

            Component {
                id: flashSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.flashText
                    caption: "Flash"
                    header: upperHeader
                    model: CameraConfigs.supportedFlashModes.length > 0
                           ? CameraConfigs.supportedFlashModes : [Camera.FlashOff]
                    delegate: SettingsMenuItem {
                        settings: Settings.mode
                        property: "flash"
                        value: modelData
                        icon: Settings.flashIcon(modelData)
                    }
                }
            }

            Component {
                id: exposureSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.exposureModeText
                    caption: "Exposure"
                    header: upperHeader
                    model: experimentalModes.value && CameraConfigs.supportedExposureModes.length > 0
                           ? CameraConfigs.supportedExposureModes : [Camera.ExposureManual]
                    delegate: SettingsMenuItem {
                        settings: Settings.mode
                        property: "exposureMode"
                        value: modelData
                        icon: Settings.exposureModeIcon(modelData)
                    }
                }
            }

            Component {
                id: isoSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.isoText
                    caption: "ISO"
                    header: upperHeader
                    model: CameraConfigs.supportedIsoSensitivities.length > 0
                           ? CameraConfigs.supportedIsoSensitivities : [0]
                    delegate: SettingsMenuItemBase {
                        settings: Settings.mode
                        property: "iso"
                        value: modelData

                        IsoItem {
                            anchors.centerIn: parent
                            value: modelData
                        }
                    }
                }
            }

            Component {
                id: gridSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.viewfinderGridText
                    caption: "Grid"
                    header: upperHeader
                    model: Settings.viewfinderGridValues
                    delegate: SettingsMenuItem {
                        settings: Settings.global
                        property: "viewfinderGrid"
                        value: modelData
                        icon: Settings.viewfinderGridIcon(modelData)
                    }
                }
            }

            Component {
                id: filterSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.colorFiltersEnabledText
                    caption: "Filter"
                    header: upperHeader
                    model: [false, true]
                    delegate: SettingsMenuItem {
                        settings: Settings.global
                        property: "colorFiltersEnabled"
                        value: modelData
                        icon: Settings.colorFiltersIcon(modelData)
                    }
                }
            }

            Component {
                id: aspectSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: "Photo aspect ratio"
                    header: upperHeader
                    settings: Settings.mode
                    property: "aspectRatio"
                    caption: "Ratio"
                    valueLabel: function(value) { return value === CameraConfigs.AspectRatio_16_9 ? "16:9" : "4:3" }
                    model: [ CameraConfigs.AspectRatio_4_3, CameraConfigs.AspectRatio_16_9 ]
                }
            }

            Component {
                id: videoResolutionSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.global.camera2VideoResolution === "2160p" ? "4K: 3840x2160"
                         : Settings.global.camera2VideoResolution === "1440p" ? "1440p: 2560x1440"
                         : "1080p: 1920x1080"
                    header: upperHeader
                    settings: Settings.global
                    property: "camera2VideoResolution"
                    caption: "Size"
                    valueLabel: function(value) {
                        return value === "2160p" ? "4K" : value === "1440p" ? "1440p" : "1080p"
                    }
                    model: Settings.camera2VideoResolutions(captureView.camera2Viewfinder.cameraId)
                }
            }

            Component {
                id: videoCodecSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.global.camera2VideoCodec === "hevc"
                           ? "HEVC: smaller files" : "H.264: plays everywhere"
                    header: upperHeader
                    settings: Settings.global
                    property: "camera2VideoCodec"
                    caption: "Codec"
                    valueLabel: function(value) { return value === "hevc" ? "HEVC" : "H.264" }
                    model: [ "avc", "hevc" ]
                }
            }

            Component {
                id: rawSaveSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: "RAW files next to the JPEG"
                    header: upperHeader
                    settings: Settings.global
                    property: "rawCaptureSaveFormat"
                    caption: "RAW"
                    valueLabel: function(value) {
                        switch (value) {
                        case "raw16": return "RAW"
                        case "dng": return "DNG"
                        case "both": return "Both"
                        default: return "Off"
                        }
                    }
                    model: [ "none", "dng", "raw16", "both" ]
                }
            }

            Component {
                id: advancedModeSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.global.advancedMode ? "Advanced mode on"
                                                        : "Advanced mode off"
                    header: upperHeader
                    settings: Settings.global
                    property: "advancedMode"
                    caption: "Advanced"
                    valueLabel: function(value) { return value ? "On" : "Off" }
                    model: [ false, true ]
                }
            }

            Component {
                id: speedSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureSpeedText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureSpeedMode"
                    caption: "Speed"
                    valueLabel: Settings.rawCaptureSpeedLabel
                    model: Settings.camera2SpeedModel()
                }
            }

            Component {
                id: sizeSettingComponent
                GridSetting {
                    width: grid.sceneWidth
                    maximumHeight: grid.gridMaximumHeight
                    header: upperHeader
                    settings: Settings.mode
                    settingProperty: "rawCaptureSize"
                    caption: "Size"
                    valueLabel: function(value) { return value.replace("x", " × ") }
                    model: Settings.camera2SizeModel(Settings.mode.camera2CaptureFormat, Settings.mode.rawCaptureRawFormat)
                }
            }

            Component {
                id: qualitySettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureJpegQualityText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureJpegQuality"
                    caption: "JPEG"
                    valueLabel: function(value) { return value }
                    model: [ 75, 85, 92, 96, 100 ]
                }
            }

            Component {
                id: rotateSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureRotationText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureRotation"
                    caption: "Rotate"
                    valueLabel: function(value) { return value + " deg" }
                    model: Settings.rawCaptureRotationModel()
                }
            }

            Component {
                id: timeoutSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureTimeoutText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureTimeout"
                    caption: "Timeout"
                    valueLabel: function(value) { return value + " s" }
                    model: [ 10, 30, 60 ]
                }
            }

            Component {
                id: focusSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureFocusModeText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureFocusMode"
                    caption: "Focus"
                    valueLabel: function(value) {
                        switch (value) {
                        case "auto": return "Auto"
                        case "continuous": return "Cont."
                        case "manual": return "Manual"
                        case "infinity": return "Infinity"
                        default: return "None"
                        }
                    }
                    model: Settings.camera2SelectableFocusModeModel()
                }
            }

            Component {
                id: focusDistanceSettingComponent
                GridSetting {
                    width: grid.sceneWidth
                    maximumHeight: grid.gridMaximumHeight
                    header: upperHeader
                    settings: Settings.mode
                    settingProperty: "rawCaptureFocusDistance"
                    caption: "Distance"
                    valueLabel: Settings.rawCaptureFocusDistanceLabel
                    model: Settings.camera2ManualFocusDistanceModel()
                }
            }

            Component {
                id: focusTimeoutSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureFocusTimeoutText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureFocusTimeout"
                    caption: "AF wait"
                    valueLabel: function(value) { return value + " s" }
                    model: Settings.rawCaptureFocusTimeoutModel()
                }
            }

            Component {
                id: sceneSettingComponent
                SceneGridSetting {
                    maximumHeight: grid.gridMaximumHeight
                    width: grid.sceneWidth
                    header: upperHeader
                }
            }

            Component {
                id: progressiveJpegSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureProgressiveJpegText
                    caption: "JPEG mode"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureProgressiveJpeg"
                    valueLabel: function(value) { return value ? "Progress." : "Standard" }
                    model: [ false, true ]
                }
            }

            Component {
                id: rawRenderEngineSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawRenderEngineText
                    caption: "Renderer"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawRenderEngine"
                    valueLabel: Settings.rawRenderEngineLabel
                    model: Settings.rawRenderEngineModel()
                }
            }

            Component {
                id: rawFormatSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureRawFormatText
                    caption: "RAW"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureRawFormat"
                    valueLabel: Settings.rawCaptureRawFormatLabel
                    model: Settings.rawCaptureRawFormatModel()
                }
            }

            Component {
                id: noiseReductionSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureNoiseReductionText
                    caption: "Noise"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureNoiseReduction"
                    valueLabel: Settings.rawCaptureNoiseReductionLabel
                    model: Settings.camera2NoiseReductionModel()
                }
            }

            Component {
                id: bracketSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureBracketText
                    caption: "Bracket"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureBracket"
                    valueLabel: Settings.rawCaptureBracketLabel
                    model: Settings.camera2BracketModel()
                }
            }

            Component {
                id: rawExposureSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureExposureText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureExposure"
                    caption: "Exposure"
                    valueLabel: function(value) { return "x" + value }
                    model: Settings.rawCaptureExposureModel()
                }
            }

            Component {
                id: whiteBalanceSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureColorTemperatureText
                    caption: "WB"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureColorTemperature"
                    valueLabel: function(value) { return value > 0 ? value + "K" : "Auto" }
                    model: [ 0, 2000, 3200, 4500, 5500, 6500, 7500, 9000 ]
                }
            }

            Component {
                id: tintSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureColorTintText
                    caption: "Tint"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureColorTint"
                    valueLabel: function(value) { return value > 0 ? "+" + value : value }
                    model: [ -200, -100, -50, 0, 50, 100, 200 ]
                }
            }

        }

        HeaderLabel {
            id: upperHeader

            anchors {
                left: parent.left
                bottom: settingsPager.visible ? settingsPager.top : settingsFlickable.top
                right: parent.right
            }
            labelVerticalOffset: (Screen.hasCutouts && overlay.isPortrait)
                                 ? Screen.topCutout.height + Theme.paddingSmall : 0
            height: overlay._headerHeight
            opacity: grid.opacity
        }
    }

    Row {
        id: topRow

        property real _topRowMargin: Math.max(overlay.topButtonRowHeight/2 - overlay._menuWidth/2,
                                              (Screen.hasCutouts && overlay.isPortrait)
                                              ? (Screen.topCutout.height + Theme.paddingSmall) : 0)

        anchors.horizontalCenter: parent.horizontalCenter
        spacing: grid.spacing
        opacity: overlay._exposed ? 0.0 : _commonControlOpacity
        // The floating top bar replaces these indicators (photo and video).
        visible: opacity > 0.0 && !camera2BottomDeck.active

        function dragY(yValue) {
            return yValue != undefined ? Math.max(topRow._topRowMargin, grid.y + yValue)
                                       : topRow._topRowMargin
        }

        Item {
            height: 1
            width: overlay._menuWidth
            visible: false
        }

        Item {
            width: overlay._menuWidth
            height: width
            visible: !grid.camera2Still && CameraConfigs.supportedFlashModes.length > 0
            y: topRow.dragY(0)

            Icon {
                anchors.centerIn: parent
                color: Theme.lightPrimaryColor
                source: Settings.flashIcon(Settings.mode.flash)
            }
        }

        Item {
            width: overlay._menuWidth
            height: width
            visible: !grid.camera2Still
                     && (experimentalModes.value ? CameraConfigs.supportedExposureModes.length > 1
                                                 : CameraConfigs.supportedIsoSensitivities.length == 0)
            y: topRow.dragY(0)

            Icon {
                anchors.centerIn: parent
                color: Theme.lightPrimaryColor
                source: Settings.exposureModeIcon(experimentalModes.value ? Settings.mode.exposureMode
                                                                          : Camera.ExposureManual)
            }
        }

        Item {
            width: overlay._menuWidth
            height: width
            y: topRow.dragY(0)
            visible: !grid.camera2Still && CameraConfigs.supportedIsoSensitivities.length > 1

            IsoItem {
                anchors.centerIn: parent
                value: Settings.mode.iso
            }
        }
    }

    Item {
        width: parent.width
        opacity: grid.opacity
        visible: overlay._exposed
        anchors.bottom: parent.bottom

        CameraButton {
            background.visible: false
            enabled: !Settings.defaultSettings && parent.opacity > 0.0
            opacity: !Settings.defaultSettings ? 1.0 : 0.0
            Behavior on opacity { FadeAnimator {}}

            width: Theme.itemSizeMedium
            height: Theme.itemSizeMedium
            anchors {
                right: parent.right
                bottom: parent.bottom
            }

            icon {
                opacity: pressed ? Theme.opacityLow : 1.0
                source: "image://theme/icon-camera-reset?" + (pressed ? _highlightColor : Theme.lightPrimaryColor)
            }

            onClicked: {
                upperHeader.pressedMenu = null
                Settings.reset()
            }
        }
    }

    Column {
        x: exposureSlider.alignment == Qt.AlignLeft ? (isPortrait ? 0 : Theme.paddingLarge)
                                                    : parent.width - width - (isPortrait ? 0 : Theme.paddingLarge)
        anchors {
            verticalCenter: parent.verticalCenter
            verticalCenterOffset: isPortrait ? (reallyWideScreen ? -Theme.itemSizeSmall : Theme.paddingMedium) : 0
        }
        spacing: Theme.paddingSmall
        opacity: _commonControlOpacity
        visible: opacity > 0.0
                 && (!grid.camera2Still || !Settings.global.advancedMode)

        WhiteBalanceMenu {
            id: whiteBalanceMenu

            anchors {
                horizontalCenter: exposureSlider.horizontalCenter
                centerIn: null
            }
            enabled: !Settings.global.colorFiltersEnabled
                     || camera.imageProcessing.colorFilter === CameraImageProcessing.ColorFilterNone

            alignment: exposureSlider.alignment
            opacity: enabled ? 1.0 - settingsOpacity : 0.0
            visible: !grid.camera2Still
            spacing: Theme.paddingMedium
        }

        ExposureSlider {
            id: exposureSlider

            alignment: _overlayPosition.exposure
            enabled: !overlay.topMenuOpen && !overlay.inButtonLayout && !whiteBalanceMenu.open
            opacity: (1.0 - settingsOpacity) * (1.0 - whiteBalanceMenu.openProgress)
            // Photo and video use the tap brightness slider instead.
            visible: !grid.camera2Still && !camera2BottomDeck.active
            height: Theme.itemSizeSmall * 5
        }
    }

    Item {
        property int paddingVector: overlay.isPortrait
                                    ? -2
                                    : _overlayPosition.exposure === Qt.AlignRight ? 2 : -2

        parent: _overlayPosition.exposure === Qt.AlignRight ? overlayAnchorBL : overlayAnchorBR
        anchors {
            centerIn: parent
            verticalCenterOffset: overlay.isPortrait ? overlayAnchorBL.width*paddingVector : 0
            horizontalCenterOffset: overlay.isPortrait ? 0 : overlayAnchorBL.height*paddingVector
        }

        opacity: qrFilter.result.length !== 0 ? 1.0 : 0.0
        visible: opacity != 0.0
        Behavior on opacity { FadeAnimation {} }

        CameraButton {
            icon.source: "image://theme/icon-camera-qr"
            background.visible: false
            anchors.centerIn: parent
            onClicked: {
                pageStack.push("QrPage.qml", { text: qrFilter.result })
            }
        }
    }

    ColorFilterView {
        id: colorFilter

        property bool ready: CameraConfigs.supportedColorFilters.length > 0
                             && Settings.global.colorFiltersEnabled && Settings.global.colorFiltersAllowed
        property var allowedFilters: [
            CameraImageProcessing.ColorFilterNone, CameraImageProcessing.ColorFilterGrayscale,
            CameraImageProcessing.ColorFilterSepia, CameraImageProcessing.ColorFilterPosterize,
            CameraImageProcessing.ColorFilterWhiteboard, CameraImageProcessing.ColorFilterBlackboard
        ]
        property var supportedFilters: {
            var filters = []
            var supportedFilters = CameraConfigs.supportedColorFilters
            for (var i = 0; i < supportedFilters.length; i++) {
                var filter = supportedFilters[i]
                if (allowedFilters.indexOf(filter) >= 0) {
                    filters.push(filter)
                }
            }
            return filters
        }

        model: ready ? supportedFilters : undefined
        anchors.bottom: parent.bottom
        orientationTransitionRunning: overlay.orientationTransitionRunning
        x: overlay.isPortrait ? 0 : exposureSlider.width + Theme.paddingMedium

        width: {
            if (overlay.isPortrait) {
                return Screen.width
            } else {
                var leftControlWidth = x
                var rightControlWidth = buttonAnchorCR.width + buttonAnchorCR.largeMargin
                var resolution = camera.viewfinder.resolution.width
                var viewfinderWidth = Screen.width * (resolution.width > 0 ? resolution.width/resolution.height : 1.2)
                return Math.min(Screen.height - rightControlWidth, viewfinderWidth) - leftControlWidth
            }
        }

        height: overlay.isPortrait ? Screen.height/11 : Theme.itemSizeMedium
        enabled: !overlay._exposed && Settings.global.colorFiltersEnabled

        onCurrentIndexChanged: update()
        onMovingChanged: update()

        function update() {
            if (!moving && !orientationTransitionRunning) {
                camera.imageProcessing.colorFilter = colorFilter.model[colorFilter.currentIndex]
            }
        }
    }

    OpacityRampEffect {
        offset: 1 - 1 / slope
        sourceItem: colorFilter
        slope: 1 + 20 * colorFilter.width / Screen.width
        visible: Settings.global.colorFiltersEnabled

        direction: OpacityRamp.BothSides
        opacity: (1.0 - settingsOpacity) * _commonControlOpacity
    }

    Item {
        id: anchorContainer

        anchors.fill: parent
        visible: overlay.inButtonLayout || layoutAnimation.running
        opacity: overlay.inButtonLayout ? 1.0 : 0.0
        Behavior on opacity {
            FadeAnimation {
                id: layoutAnimation
            }
        }

        Rectangle {
            anchors.fill: parent
            opacity: Theme.opacityOverlay
            color: "black"
        }

        Label {
            anchors {
                centerIn: parent
                verticalCenterOffset: -Theme.paddingLarge
            }
            width: overlay.isPortrait
                   ? Screen.width - (2 * Theme.itemSizeExtraLarge)
                   : Screen.width - Theme.itemSizeExtraLarge
            font.pixelSize: Theme.fontSizeExtraLarge
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            textFormat: Text.AutoText
            color: _highlightColor

            text: overlay.isPortrait
                  ? "Select location for the portrait capture key"
                  : "Select location for the landscape capture key"
        }
    }

    ConfigurationValue {
        id: experimentalModes

        key: "/apps/procam/enable_experimental_modes"
        defaultValue: false
    }
}
