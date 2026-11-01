// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0

// Segmented Normal | Pro switch floating over the viewfinder.
Item {
    id: root

    property bool pro
    signal selected(bool pro)

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
            model: [false, true]

            MouseArea {
                readonly property bool active: modelData === root.pro

                width: label.implicitWidth + 2 * Theme.paddingLarge
                height: row.height
                onClicked: if (!active) root.selected(modelData)

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: Theme.paddingSmall / 2
                    radius: height / 2
                    color: parent.active ? Theme.rgba(Theme.highlightBackgroundColor, 0.6)
                                         : parent.pressed ? Theme.rgba(Theme.highlightBackgroundColor, 0.3)
                                                          : "transparent"
                }

                Label {
                    id: label

                    anchors.centerIn: parent
                    text: modelData ? "Pro" : "Normal"
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: parent.active
                    color: Theme.lightPrimaryColor
                }
            }
        }
    }
}
