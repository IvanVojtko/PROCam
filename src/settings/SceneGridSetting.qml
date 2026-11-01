// SPDX-License-Identifier: BSD-3-Clause
import QtQuick 2.6
import com.procam.camera 1.0

GridSetting {
    caption: "Scene"
    settings: Settings.mode
    settingProperty: "rawCaptureScene"
    model: Settings.camera2SceneModel()
    valueLabel: Settings.rawCaptureSceneLabel
}
