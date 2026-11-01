// SPDX-FileCopyrightText: 2016 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0
import com.procam.camera 1.0

ExpandingMenu {
    model: [ "image", "video" ]
    delegate: ExpandingMenuItem {
        settings: Settings.global
        property: "captureMode"
        value: modelData
        icon: Settings.captureModeIcon(modelData)
    }
}
