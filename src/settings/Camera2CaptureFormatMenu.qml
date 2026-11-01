// SPDX-FileCopyrightText: 2026 RAWfish contributors
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0
import com.procam.camera 1.0

ExpandingMenu {
    model: Settings.camera2CaptureFormatModel()
    delegate: MouseArea {
        id: menuItem

        readonly property bool selected: Settings.mode.camera2CaptureFormat === modelData
        readonly property bool highlighted: parent && (parent.open || parent.pressed) && pressed
        property color highlightColor: Theme.colorScheme == Theme.LightOnDark
                                       ? Theme.highlightColor
                                       : Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark)

        width: parent.width
        height: selected ? parent.width : parent.itemHeight

        onSelectedChanged: {
            if (selected && parent) {
                parent.currentIndex = index
                parent.currentItem = menuItem
            }
        }

        onParentChanged: {
            if (selected && parent) {
                parent.currentIndex = index
                parent.currentItem = menuItem
            }
        }

        onClicked: Settings.mode.camera2CaptureFormat = modelData

        Item {
            anchors.fill: parent

            opacity: menuItem.selected || !menuItem.parent ? 1.0 : menuItem.parent.itemOpacity
            visible: menuItem.selected || (menuItem.parent && menuItem.parent.itemsVisible)

            Rectangle {
                anchors.centerIn: parent
                width: Theme.itemSizeExtraSmall
                height: Theme.itemSizeExtraSmall
                radius: width / 2
                color: menuItem.highlightColor
                opacity: menuItem.highlighted ? Theme.opacityLow : 0.0
                Behavior on opacity { FadeAnimation {} }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width
                color: menuItem.pressed ? menuItem.highlightColor : Theme.lightPrimaryColor
                font {
                    pixelSize: Theme.fontSizeTiny
                    bold: true
                }
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: modelData === "jpeg" ? "JPG" : "RAW"
            }
        }
    }
}
