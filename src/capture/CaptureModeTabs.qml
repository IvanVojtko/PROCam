// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0

// PHOTO | VIDEO tabs above the shutter, as on Android cameras. Swiping the
// viewfinder left/right switches them too.
Row {
    id: root

    property string mode: "image"
    signal selected(string mode)

    height: Theme.itemSizeExtraSmall
    spacing: Theme.paddingLarge
    opacity: enabled ? 1.0 : Theme.opacityLow

    Repeater {
        model: [{ "mode": "image", "label": "Photo" }, { "mode": "video", "label": "Video" }]

        MouseArea {
            readonly property bool active: modelData.mode === root.mode

            width: tabLabel.implicitWidth + 2 * Theme.paddingMedium
            height: root.height
            onClicked: if (!active) root.selected(modelData.mode)

            Rectangle {
                anchors.fill: parent
                radius: height / 2
                color: parent.active ? Theme.rgba("black", 0.45)
                                     : parent.pressed ? Theme.rgba("black", 0.3) : "transparent"
            }

            Label {
                id: tabLabel

                anchors.centerIn: parent
                text: modelData.label
                font.pixelSize: Theme.fontSizeSmall
                font.capitalization: Font.AllUppercase
                font.bold: parent.active
                color: parent.active ? "#ffd54a" : Theme.rgba(Theme.lightPrimaryColor, 0.8)
            }
        }
    }
}
