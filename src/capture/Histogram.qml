// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0

// The viewfinder's RGB histogram on a translucent card (Pro mode).
Item {
    id: root

    // [{r, g, b}, ...] from Camera2Preview.histogram.
    property var values: []

    onValuesChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        radius: Theme.paddingSmall
        color: Theme.rgba("black", 0.4)
    }

    Canvas {
        id: canvas

        anchors {
            fill: parent
            margins: Theme.paddingSmall / 2
        }
        opacity: 0.85

        onPaint: {
            var context = getContext("2d")
            context.clearRect(0, 0, width, height)
            var values = root.values
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
    }
}
