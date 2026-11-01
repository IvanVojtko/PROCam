// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0

// Pixel-style zoom presets floating over the viewfinder. The active preset
// shows the live zoom while pinching (e.g. "1.4×").
Item {
    id: root

    property real zoom: 1.0
    property var presets: [1, 2]
    signal selected(real value)

    // The largest preset at or below the current zoom.
    readonly property int activeIndex: {
        var index = 0
        for (var i = 0; i < presets.length; ++i) {
            if (zoom >= presets[i] - 0.05) index = i
        }
        return index
    }

    function label(value, active) {
        var shown = active ? zoom : value
        var rounded = Math.round(shown * 10) / 10
        return (rounded % 1 === 0 ? rounded.toFixed(0) : rounded.toFixed(1)) + "×"
    }

    implicitWidth: row.width
    implicitHeight: Theme.itemSizeExtraSmall
    opacity: enabled ? 1.0 : Theme.opacityLow

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Theme.rgba("black", 0.4)
    }

    Row {
        id: row

        height: parent.height

        Repeater {
            model: root.presets

            MouseArea {
                readonly property bool active: index === root.activeIndex

                width: Math.max(height, chipLabel.implicitWidth + 2 * Theme.paddingMedium)
                height: row.height
                onClicked: root.selected(modelData)

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: Theme.paddingSmall / 2
                    radius: height / 2
                    color: parent.active ? Theme.rgba(Theme.highlightBackgroundColor, 0.6)
                                         : parent.pressed ? Theme.rgba(Theme.highlightBackgroundColor, 0.3)
                                                          : "transparent"
                }

                Label {
                    id: chipLabel

                    anchors.centerIn: parent
                    text: root.label(modelData, parent.active)
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: parent.active
                    color: Theme.lightPrimaryColor
                }
            }
        }
    }
}
