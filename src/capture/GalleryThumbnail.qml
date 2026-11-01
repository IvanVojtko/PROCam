// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0
import Nemo.Thumbnailer 1.0

// Most recent capture; tapping it opens the gallery.
MouseArea {
    id: root

    property QtObject captureModel
    // A new photo is being rendered.
    property bool busy
    // The viewfinder's look of the newest shot, shown until it is processed.
    property url pendingSource
    readonly property bool hasCapture: !!captureModel && captureModel.count > 0

    width: Theme.itemSizeMedium
    height: width
    enabled: hasCapture && !busy
    opacity: hasCapture || busy || pendingImage.visible ? 1.0 : 0.0
    Behavior on opacity { FadeAnimation {} }

    ListView {
        id: list

        anchors.fill: parent
        interactive: false
        clip: true
        model: root.captureModel
        // Only the first (newest) entry is ever in view. A ListView keeps
        // the item it shows when rows are inserted before it, so a new photo
        // (inserted at the top) would stay hidden behind the previous one.
        cacheBuffer: 0
        onCountChanged: positionViewAtBeginning()
        onModelChanged: positionViewAtBeginning()
        delegate: Thumbnail {
            width: root.width
            height: root.height
            source: model.url
            mimeType: model.mimeType
            sourceSize.width: width
            sourceSize.height: height
            fillMode: Thumbnail.PreserveAspectCrop
            smooth: true
        }
    }

    Image {
        id: pendingImage

        anchors.fill: parent
        visible: root.pendingSource != "" && status === Image.Ready
        source: root.pendingSource
        cache: false
        asynchronous: true
        fillMode: Image.PreserveAspectCrop
        sourceSize.width: width
        sourceSize.height: height
        smooth: true
    }

    Rectangle {
        anchors.fill: parent
        color: root.pressed ? Theme.rgba(Theme.highlightBackgroundColor, Theme.highlightBackgroundOpacity)
                            : "transparent"
        border.color: Theme.rgba("white", 0.85)
        border.width: Math.round(Theme.pixelRatio * 2)
        radius: Theme.paddingSmall
    }

    Rectangle {
        anchors.fill: parent
        visible: root.busy
        radius: Theme.paddingSmall
        // Lighter over the instant thumbnail, so the shot stays visible.
        color: Theme.rgba("black", pendingImage.visible ? 0.25 : 0.5)

        BusyIndicator {
            anchors.centerIn: parent
            size: BusyIndicatorSize.Small
            running: root.busy
        }
    }
}
