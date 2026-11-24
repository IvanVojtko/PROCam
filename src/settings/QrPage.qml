// SPDX-FileCopyrightText: 2020 Open Mobile Platform LLC.
// SPDX-FileCopyrightText: 2021 - 2024 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Silica.private 1.0 as SilicaPrivate

Page {
    id: root
    property alias text: label.plainText

    SilicaPrivate.BackgroundRectangle {
        anchors.fill: parent
    }

    SilicaFlickable {
        id: flickable

        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            property int internalPadding: Math.max(flickable.height
                                                   - header.height - label.height - button.height - Theme.paddingLarge*2,
                                                   Theme.paddingLarge*2) / 2

            width: parent.width

            PageHeader {
                id: header
                //% "QR code"
                title: qsTrId("procam-qr-title")
            }

            Item {
                width: 1
                height: column.internalPadding
            }

            LinkedLabel {
                id: label

                x: Theme.horizontalPageMargin
                width: parent.width - 2*x
                color: highlighted ? Theme.highlightColor : Theme.primaryColor
                linkColor: Theme.highlightColor
                shortenUrl: true
                horizontalAlignment: lineCount > 5 ? Text.AlignLeft : Text.AlignHCenter
            }

            Item {
                width: 1
                height: column.internalPadding
            }

            Button {
                id: button

                //% "Copy"
                text: qsTrId("procam-qr-copy")
                preferredWidth: Screen.sizeCategory < Screen.Large && isPortrait
                                                    ? parent.width - Theme.horizontalPageMargin*2
                                                    : Theme.buttonWidthLarge
                onClicked: Clipboard.text = root.text
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }

        VerticalScrollDecorator {}
    }
}
