// SPDX-FileCopyrightText: 2018 - 2021 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.1
import Sailfish.Silica 1.0
import com.procam.camera 1.0

GalleryView {
    id: root

    captureModel: CaptureModel {
        id: model

        directories: Settings.storagePathStatus, [
            Settings.photoDirectory,
            Settings.videoDirectory
        ]
    }

    ViewPlaceholder {
        text: "Captured photos and videos will appear here when you take some"
        enabled: model.count === 0 && model.populated
    }
}
