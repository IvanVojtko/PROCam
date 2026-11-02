// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0
import com.procam.camera 1.0

// Camera switching without focal lengths: flip between front and rear, and
// on the rear side toggle the ultra-wide camera. Stacked vertically so the
// pair never spreads into the shutter in narrow (landscape) decks.
Column {
    id: root

    property bool captureBusy: false
    // Video records through Qt Multimedia: its own front/rear ids, no ultra-wide.
    property bool qtCamera: false
    onQtCameraChanged: refresh()
    property var roles: ({ "front": "", "main": "", "ultrawide": "", "tele": "", "teleZoom": 0 })
    readonly property bool frontActive: roles.front !== "" && Settings.deviceId === roles.front
    readonly property bool ultraWideActive: roles.ultrawide !== "" && Settings.deviceId === roles.ultrawide
    readonly property bool teleActive: !!roles.tele && Settings.deviceId === roles.tele
    readonly property int buttonCount: (roles.front !== "" ? 1 : 0)
                                       + (roles.ultrawide !== "" && !frontActive ? 1 : 0)
                                       + (!!roles.tele && !frontActive ? 1 : 0)
    // Space the stacked buttons may take; they shrink to fit.
    property real availableWidth: Theme.itemSizeSmall
    property real availableHeight: 2 * Theme.itemSizeSmall + spacing
    readonly property real buttonSize: Math.max(Theme.itemSizeExtraSmall * 0.75,
                                                 Math.min(Theme.itemSizeSmall, availableWidth,
                                                          (availableHeight - spacing * (Math.max(2, buttonCount) - 1))
                                                          / Math.max(2, buttonCount)))

    signal switched()

    function refresh() {
        if (qtCamera) {
            var back = Settings.global.previousBackFacingDeviceId
            roles = { "front": Settings.global.frontFacingDeviceId,
                      "main": back && back !== Settings.global.frontFacingDeviceId ? back : "",
                      "ultrawide": "", "tele": "", "teleZoom": 0 }
        } else {
            roles = Settings.camera2CameraRoles()
        }
    }

    function select(cameraId) {
        if (!cameraId || cameraId === Settings.deviceId) return
        Settings.deviceId = cameraId
        switched()
    }

    function flip() {
        if (frontActive) {
            var back = Settings.global.previousBackFacingDeviceId
            select(back && back !== roles.front ? back : roles.main)
        } else {
            select(roles.front)
        }
    }

    spacing: Theme.paddingMedium
    enabled: !captureBusy
    opacity: enabled ? 1.0 : Theme.opacityLow
    Component.onCompleted: refresh()

    Connections {
        target: Settings
        onDeviceIdChanged: root.refresh()
    }

    // Ultra-wide toggle (rear side only).
    MouseArea {
        id: wideButton

        width: root.buttonSize
        height: root.buttonSize
        visible: root.roles.ultrawide !== "" && !root.frontActive
        onClicked: root.select(root.ultraWideActive ? root.roles.main : root.roles.ultrawide)

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: root.ultraWideActive ? Theme.rgba(Theme.highlightBackgroundColor, 0.6)
                                        : Theme.rgba("black", wideButton.pressed ? 0.6 : 0.4)
        }

        // Wide field of view: two rays fanning out from the lens.
        Canvas {
            id: wideIcon

            anchors.centerIn: parent
            width: parent.width * 0.6
            height: width
            property color color: root.ultraWideActive ? Theme.lightPrimaryColor
                                                       : wideButton.pressed ? Theme.highlightColor
                                                                            : Theme.lightPrimaryColor
            onColorChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d")
                var w = width, h = height
                ctx.reset()
                ctx.strokeStyle = color
                ctx.fillStyle = color
                ctx.lineWidth = Math.max(2, w * 0.08)
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                var cx = w / 2, cy = h * 0.78
                ctx.beginPath()
                ctx.moveTo(w * 0.06, h * 0.30)
                ctx.lineTo(cx, cy)
                ctx.lineTo(w * 0.94, h * 0.30)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(cx, cy + h * 0.05, w * 0.62, Math.PI * 1.25, Math.PI * 1.75, false)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(cx, cy, w * 0.07, 0, Math.PI * 2, false)
                ctx.fill()
            }
        }
    }

    // Front / rear flip.
    // Telephoto, labelled with its magnification ("2×"); tapping it again
    // returns to the main camera.
    MouseArea {
        id: teleButton

        width: root.buttonSize
        height: root.buttonSize
        visible: !!root.roles.tele && !root.frontActive
        onClicked: root.select(root.teleActive ? root.roles.main : root.roles.tele)

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: root.teleActive ? Theme.rgba(Theme.highlightBackgroundColor, 0.6)
                                   : Theme.rgba("black", teleButton.pressed ? 0.6 : 0.4)
        }

        Label {
            anchors.centerIn: parent
            text: {
                var zoom = Math.round((root.roles.teleZoom || 2) * 10) / 10
                return (zoom % 1 === 0 ? zoom.toFixed(0) : zoom.toFixed(1)) + "×"
            }
            font.pixelSize: Theme.fontSizeSmall
            font.bold: root.teleActive
            color: teleButton.pressed && !root.teleActive ? Theme.highlightColor : Theme.lightPrimaryColor
        }
    }

    MouseArea {
        id: flipButton

        width: root.buttonSize
        height: root.buttonSize
        visible: root.roles.front !== ""
        onClicked: root.flip()

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: Theme.rgba("black", flipButton.pressed ? 0.6 : 0.4)
        }

        Icon {
            anchors.centerIn: parent
            source: "image://theme/icon-camera-switch"
            color: flipButton.pressed ? Theme.highlightColor : Theme.lightPrimaryColor
        }
    }
}
