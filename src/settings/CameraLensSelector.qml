// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0
import com.procam.camera 1.0

CycleValueButton {
    property bool captureBusy: false

    settings: Settings
    settingProperty: "deviceId"
    currentValue: Settings.deviceId
    model: Settings.camera2LensModel()
    valueLabel: Settings.camera2LensLabel
    enabled: !captureBusy && model.length > 1
    opacity: enabled ? 1.0 : Theme.opacityLow
}
