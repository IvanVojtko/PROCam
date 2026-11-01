// SPDX-FileCopyrightText: 2013 - 2022 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import QtMultimedia 5.0
import com.procam.camera 1.0
import Nemo.Configuration 1.0
import org.nemomobile.systemsettings 1.0
import Sailfish.Silica 1.0
import Sailfish.Policy 1.0
import com.jolla.settings 1.0
import com.jolla.settings.system 1.0

ApplicationSettings {
    CameraExtensions { id: camera2Compatibility }

    function aspectRatioName(aspectRatio) {
        if (aspectRatio === CameraConfigs.AspectRatio_16_9) {
            return "16:9"
        } else if (aspectRatio === CameraConfigs.AspectRatio_4_3) {
            return "4:3"
        } else {
            console.warn("Camera Settings: Unsupported aspect ratio")
        }
    }

    ConfigurationValue {
        id: backCameraAspectRatio

        key: "/apps/procam/back/image/aspectRatio"
        defaultValue: CameraConfigs.AspectRatio_4_3
    }

    ConfigurationValue {
        id: frontCameraAspectRatio

        key: "/apps/procam/front/image/aspectRatio"
        defaultValue: CameraConfigs.AspectRatio_4_3
    }

    IconTextSwitch {
        automaticCheck: false
        text: "Estimated exposure metering"
        description: "Use preview brightness for partial-auto exposure and the manual meter. Experimental; camera accuracy is not verified."
        checked: Settings.global.estimatedExposureMetering
        onClicked: Settings.global.estimatedExposureMetering = !checked
    }

    IconTextSwitch {
        automaticCheck: false
        text: "Show exposure status"
        description: "Show metering, waiting and exposure-limit messages at the top of the viewfinder."
        checked: Settings.global.showExposureStatus
        onClicked: Settings.global.showExposureStatus = !checked
    }

    IconTextSwitch {
        automaticCheck: false
        text: "Keep every burst frame"
        description: "Save all frames of each HDR and night photo as RAW (YUV for the front and ultra-wide cameras) with a JSON sidecar, about 25 MB each. For diagnostics and training data."
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.keepBurstFrames
        onClicked: Settings.global.keepBurstFrames = !checked
    }

    IconTextSwitch {
        automaticCheck: false
        text: "Experimental exposure layout"
        description: "Widen histogram and EV, with Speed and ISO aligned to the shutter and values below. Advanced photo mode only."
        checked: Settings.global.experimentalExposureLayout
        onClicked: Settings.global.experimentalExposureLayout = !checked
    }

    LocationSettings { id: locationSettings }

    DisabledByMdmBanner {
        active: !AccessPolicy.cameraEnabled
    }

    IconTextSwitch {
        automaticCheck: false
        icon.source: "image://theme/icon-m-gps"
        text: "Save location"
        description: "Save current GPS coordinates in captured photos."
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.saveLocationInfo
        onClicked: Settings.global.saveLocationInfo = !Settings.global.saveLocationInfo
    }

    IconTextSwitch {
        automaticCheck: false
        icon.source: "image://theme/icon-m-developer-mode"
        text: "Advanced mode"
        description: "Show ProCam manual controls and RAW capture settings."
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.advancedMode
        onClicked: Settings.global.advancedMode = !Settings.global.advancedMode
    }

    SectionHeader {
        text: "ProCam compatibility"
        visible: Settings.global.advancedMode
    }

    Label {
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * x
        visible: Settings.global.advancedMode
        wrapMode: Text.Wrap
        color: Theme.secondaryColor
        font.pixelSize: Theme.fontSizeSmall
        text: camera2Compatibility.camera2CompatibilitySummary(Settings.deviceId)
    }

    IconTextSwitch {
        automaticCheck: false
        icon.source: "image://theme/icon-m-qr"
        text: "Enable QR-code recognition"
        description: "Detect QR-code via camera."
        visible: Settings.global.advancedMode || Settings.global.qrFilterEnabled
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.qrFilterEnabled
        onClicked: Settings.global.qrFilterEnabled = !Settings.global.qrFilterEnabled
    }

    ComboBox {
        label: "RAW capture files"
        description: "Kept next to each photo, in Normal and Pro mode. Not available for cameras without RAW output."
        enabled: AccessPolicy.cameraEnabled
        currentIndex: {
            switch (Settings.global.rawCaptureSaveFormat) {
            case "raw16": return 1
            case "dng": return 2
            case "both": return 3
            default: return 0
            }
        }

        menu: ContextMenu {
            MenuItem {
                text: "Do not save"
                onClicked: Settings.global.rawCaptureSaveFormat = "none"
            }
            MenuItem {
                text: "RAW + JSON"
                onClicked: Settings.global.rawCaptureSaveFormat = "raw16"
            }
            MenuItem {
                text: "DNG"
                onClicked: Settings.global.rawCaptureSaveFormat = "dng"
            }
            MenuItem {
                text: "RAW + JSON + DNG"
                onClicked: Settings.global.rawCaptureSaveFormat = "both"
            }
        }
    }

    Label {
        text: "Positioning is turned off. Enable it in Settings | Connectivity | Location"
        wrapMode: Text.Wrap
        x: Theme.horizontalPageMargin
        width: parent.width - 2*x
        color: Theme.highlightColor
        font.pixelSize: Theme.fontSizeSmall
        visible: !locationSettings.locationEnabled
    }

    ComboBox {
        id: storageCombo

        readonly property int storageStatus: Settings.storagePathStatus
        readonly property string storagePath: Settings.storagePath

        function updateCurrentIndex() {
            if (!partitions.externalStoragesPopulated)
                return

            for (var i = 0; i < menu.children.length; ++i) {
                var item = menu.children[i]
                if (item.hasOwnProperty("__silica_menuitem") && item.visible && item.mountPath == Settings.storagePath) {
                    currentIndex = i
                    currentItem = item
                    return
                }
            }
            currentIndex = -1
        }

        onStorageStatusChanged: updateCurrentIndex()
        onStoragePathChanged: updateCurrentIndex()
        Component.onCompleted: updateCurrentIndex()

        label: "Storage"
        enabled: AccessPolicy.cameraEnabled
        menu: ContextMenu {
            MenuItem {
                property string mountPath: ""
                text: "Device memory"
                onClicked: Settings.storagePath = ""
            }
            MenuItem {
                // This is a placeholder for a card that was previously selected, but is no longer inserted
                property string mountPath: Settings.storagePath

                text: "Memory card not inserted"
                visible: partitions.externalStoragesPopulated && partitions.count == 0 && Settings.storagePath !== ""
                onVisibleChanged: storageCombo.updateCurrentIndex()
                opacity: Theme.opacityLow
            }
            Repeater {
                model: partitions
                delegate: MenuItem {
                    property string mountPath: model.mountPath

                    onMountPathChanged: storageCombo.updateCurrentIndex()
                    enabled: model.status === PartitionModel.Mounted && model.devicePath !== ""
                    text: model.status === PartitionModel.Mounted
                          ? "Memory card " + Format.formatFileSize(model.bytesAvailable)
                          : model.devicePath !== ""
                            ? "Memory card not mounted"
                            : "Memory card not inserted"
                    onClicked: Settings.storagePath = model.mountPath
                }
            }
        }
    }

    Label {
        text: "The selected storage is not available. Device memory will be used instead."
        visible: Settings.storagePathStatus == Settings.Unavailable
        x: Theme.horizontalPageMargin
        width: parent.width - x*2
        color: Theme.secondaryColor
        font.pixelSize: Theme.fontSizeExtraSmall
        wrapMode: Text.Wrap
    }

    SectionHeader {
        text: "Back camera"
        opacity: AccessPolicy.cameraEnabled ? 1.0 : Theme.opacityLow
    }

    ComboBox {
        label: "Aspect ratio"
        enabled: AccessPolicy.cameraEnabled
        currentIndex: backCameraAspectRatio.value

        menu: ContextMenu {
            MenuItem {
                text: aspectRatioName(CameraConfigs.AspectRatio_4_3)
                onClicked: backCameraAspectRatio.value = CameraConfigs.AspectRatio_4_3
            }
            MenuItem {
                text: aspectRatioName(CameraConfigs.AspectRatio_16_9)
                onClicked: backCameraAspectRatio.value = CameraConfigs.AspectRatio_16_9
            }
        }
    }

    SectionHeader {
        text: "Front camera"
        opacity: AccessPolicy.cameraEnabled ? 1.0 : Theme.opacityLow
    }

    ComboBox {
        label: "Aspect ratio"
        enabled: AccessPolicy.cameraEnabled
        currentIndex: frontCameraAspectRatio.value
        menu: ContextMenu {
            MenuItem {
                text: aspectRatioName(CameraConfigs.AspectRatio_4_3)
                onClicked: frontCameraAspectRatio.value = CameraConfigs.AspectRatio_4_3
            }
            MenuItem {
                text: aspectRatioName(CameraConfigs.AspectRatio_16_9)
                onClicked: frontCameraAspectRatio.value = CameraConfigs.AspectRatio_16_9
            }
        }
    }

    PartitionModel {
        id: partitions

        storageTypes: PartitionModel.External | PartitionModel.ExcludeParents
        onExternalStoragesPopulatedChanged: storageCombo.updateCurrentIndex()
    }
}
