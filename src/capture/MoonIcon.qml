// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0

// Crescent moon (night mode): white, or yellow when lit (night mode on). An
// SVG rather than a Canvas, whose drawing can be lost and never repainted.
Image {
    property bool lit

    width: Theme.iconSizeSmallPlus
    height: width
    sourceSize: Qt.size(width, height)
    source: lit ? "moon-lit.svg" : "moon.svg"
}
