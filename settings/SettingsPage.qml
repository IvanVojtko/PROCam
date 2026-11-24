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
import Sailfish.Pickers 1.0
import Sailfish.Policy 1.0
import com.jolla.settings 1.0
import com.jolla.settings.system 1.0

ApplicationSettings {
    id: settingsPage

    CameraExtensions { id: camera2Compatibility }

    // A picked folder was outside what ProCam's sandbox can write to.
    property bool folderRefused

    // "Pictures/Trips" for a folder in the home directory, "Memory card/DCIM"
    // for one on a card (mounted at /run/media/<user>/<card>).
    function folderName(path) {
        var home = StandardPaths.home
        if (path.indexOf(home + "/") === 0)
            return path.substring(home.length + 1)
        var card = path.match(/^\/run\/media\/[^\/]+\/[^\/]+(\/.*)?$/)
        if (card) {
            //% "Memory card%1"
            return qsTrId("procam-settings-memory-card-path").arg(card[1] ? card[1] : "")
        }
        return path
    }

    function folderDescription(custom, available, used) {
        if (custom !== "" && !available) {
            //% "The selected folder is not available. Saving to %1 instead."
            return qsTrId("procam-settings-folder-unavailable").arg(folderName(used))
        }
        //% "Saving to %1"
        return qsTrId("procam-settings-saving-to").arg(folderName(used))
    }

    function pickFolder(videos) {
        folderRefused = false
        pageStack.animatorPush(folderPickerPage, { videos: videos })
    }

    Component {
        id: folderPickerPage

        FolderPickerPage {
            property bool videos

            dialogTitle: {
                if (videos) {
                    //% "Save videos here"
                    return qsTrId("procam-settings-save-videos-here")
                } else {
                    //% "Save photos here"
                    return qsTrId("procam-settings-save-photos-here")
                }
            }
            onSelectedPathChanged: {
                if (selectedPath === "")
                    return
                if (!Settings.folderAllowed(selectedPath)) {
                    settingsPage.folderRefused = true
                } else if (videos) {
                    Settings.customVideoDirectory = selectedPath
                } else {
                    Settings.customPhotoDirectory = selectedPath
                }
            }
        }
    }

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
        //% "Estimated exposure metering"
        text: qsTrId("procam-settings-estimated-exposure-metering")
        //% "Use preview brightness for partial-auto exposure and the manual meter. Experimental; camera accuracy is not verified."
        description: qsTrId("procam-settings-estimated-exposure-metering-description")
        checked: Settings.global.estimatedExposureMetering
        onClicked: Settings.global.estimatedExposureMetering = !checked
    }

    IconTextSwitch {
        automaticCheck: false
        //% "Show exposure status"
        text: qsTrId("procam-settings-show-exposure-status")
        //% "Show metering, waiting and exposure-limit messages at the top of the viewfinder."
        description: qsTrId("procam-settings-show-exposure-status-description")
        checked: Settings.global.showExposureStatus
        onClicked: Settings.global.showExposureStatus = !checked
    }

    IconTextSwitch {
        automaticCheck: false
        //% "Keep every burst frame"
        text: qsTrId("procam-settings-keep-every-burst-frame")
        //% "Save all frames of each HDR and night photo as RAW (YUV for the front and ultra-wide cameras) with a JSON sidecar, about 25 MB each. For diagnostics and training data."
        description: qsTrId("procam-settings-keep-every-burst-frame-description")
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.keepBurstFrames
        onClicked: Settings.global.keepBurstFrames = !checked
    }

    IconTextSwitch {
        automaticCheck: false
        //% "Keep trying RAW"
        text: qsTrId("procam-settings-keep-trying-raw")
        //% "For testing cameras whose RAW photos come out damaged (seen on the Xperia 10 III). ProCam keeps taking RAW photos instead of switching that camera to YUV frames. Damaged photos fail, and their RAW frame is saved next to your photos for troubleshooting."
        description: qsTrId("procam-settings-keep-trying-raw-description")
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.keepTryingRaw
        onClicked: Settings.global.keepTryingRaw = !checked
    }

    IconTextSwitch {
        automaticCheck: false
        //% "Learned denoise"
        text: qsTrId("procam-settings-learned-denoise")
        //% "Clean up noisy HDR and night photos with a small neural network on the graphics processor instead of the standard noise filter. Keeps more texture in dim light; adds about half a second per photo. Brighter photos keep the standard filter."
        description: qsTrId("procam-settings-learned-denoise-description")
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.learnedDenoise
        onClicked: Settings.global.learnedDenoise = !checked
    }

    IconTextSwitch {
        automaticCheck: false
        //% "ProCam video recorder"
        text: qsTrId("procam-settings-video-recorder")
        //% "Record video through ProCam's own camera session: 1080p at 30 fps with electronic stabilization, no system configuration needed. Turn off to record with the system camera service instead, if recording fails."
        description: qsTrId("procam-settings-video-recorder-description")
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.camera2Video
        onClicked: Settings.global.camera2Video = !checked
    }

    LocationSettings { id: locationSettings }

    DisabledByMdmBanner {
        active: !AccessPolicy.cameraEnabled
    }

    IconTextSwitch {
        automaticCheck: false
        icon.source: "image://theme/icon-m-gps"
        //% "Save location"
        text: qsTrId("procam-settings-save-location")
        //% "Save current GPS coordinates in captured photos."
        description: qsTrId("procam-settings-save-location-description")
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.saveLocationInfo
        onClicked: Settings.global.saveLocationInfo = !Settings.global.saveLocationInfo
    }

    IconTextSwitch {
        automaticCheck: false
        icon.source: "image://theme/icon-m-developer-mode"
        //% "Advanced mode"
        text: qsTrId("procam-settings-advanced-mode")
        //% "Show ProCam manual controls and RAW capture settings."
        description: qsTrId("procam-settings-advanced-mode-description")
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.advancedMode
        onClicked: Settings.global.advancedMode = !Settings.global.advancedMode
    }

    SectionHeader {
        //% "ProCam compatibility"
        text: qsTrId("procam-settings-compatibility")
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
        //% "Detect QR codes"
        text: qsTrId("procam-settings-detect-qr-codes")
        //% "A QR code in the photo viewfinder shows its link (or text) in a chip; tap the chip to open it."
        description: qsTrId("procam-settings-detect-qr-codes-description")
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.qrFilterEnabled
        onClicked: Settings.global.qrFilterEnabled = !Settings.global.qrFilterEnabled
    }

    IconTextSwitch {
        automaticCheck: false
        icon.source: "image://theme/icon-m-file-document"
        //% "Detect documents"
        text: qsTrId("procam-settings-detect-documents")
        //% "With the camera pointing down at a page, receipt or card, the photo viewfinder outlines it. Tap the outline to save a flat, straightened photo of just the document."
        description: qsTrId("procam-settings-detect-documents-description")
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.documentDetection
        onClicked: Settings.global.documentDetection = !Settings.global.documentDetection
    }

    ComboBox {
        //% "Photo format"
        label: qsTrId("procam-settings-photo-format")
        description: {
            if (Settings.global.photoFormat === "heic") {
                //% "HEIC photos take about a third less space than JPEG at the same quality. Some older apps and computers cannot open them."
                return qsTrId("procam-settings-heic-description")
            } else {
                //% "JPEG opens everywhere. HEIC takes about a third less space."
                return qsTrId("procam-settings-jpeg-description")
            }
        }
        enabled: AccessPolicy.cameraEnabled
        currentIndex: Settings.global.photoFormat === "heic" ? 1 : 0

        menu: ContextMenu {
            MenuItem {
                text: "JPEG"
                onClicked: Settings.global.photoFormat = "jpeg"
            }
            MenuItem {
                text: "HEIC"
                onClicked: Settings.global.photoFormat = "heic"
            }
        }
    }

    IconTextSwitch {
        automaticCheck: false
        //% "Save photo details"
        text: qsTrId("procam-settings-save-photo-details")
        //% "Keep a .json file next to each photo with its capture and processing details, for troubleshooting. The photo's own EXIF (exposure, ISO, location) is saved either way, and RAW + JSON files always keep theirs."
        description: qsTrId("procam-settings-save-photo-details-description")
        enabled: AccessPolicy.cameraEnabled
        checked: Settings.global.savePhotoInfo
        onClicked: Settings.global.savePhotoInfo = !checked
    }

    ComboBox {
        //% "RAW capture files"
        label: qsTrId("procam-settings-raw-capture-files")
        //% "Kept next to each photo, in Normal and Pro mode. Not available for cameras without RAW output."
        description: qsTrId("procam-settings-raw-capture-files-description")
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
                //% "Do not save"
                text: qsTrId("procam-settings-do-not-save")
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
        //% "Positioning is turned off. Enable it in Settings | Connectivity | Location"
        text: qsTrId("procam-settings-positioning-disabled")
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

        //% "Storage"
        label: qsTrId("procam-settings-storage")
        enabled: AccessPolicy.cameraEnabled
        menu: ContextMenu {
            MenuItem {
                property string mountPath: ""
                //% "Device memory"
                text: qsTrId("procam-settings-device-memory")
                onClicked: Settings.storagePath = ""
            }
            MenuItem {
                // This is a placeholder for a card that was previously selected, but is no longer inserted
                property string mountPath: Settings.storagePath

                //% "Memory card not inserted"
                text: qsTrId("procam-settings-memory-card-not-inserted")
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
                    text: {
                        if (model.status === PartitionModel.Mounted) {
                            //% "Memory card %1"
                            return qsTrId("procam-settings-memory-card").arg(Format.formatFileSize(model.bytesAvailable))
                        } else if (model.devicePath !== "") {
                            //% "Memory card not mounted"
                            return qsTrId("procam-settings-memory-card-not-mounted")
                        } else {
                            //% "Memory card not inserted"
                            return qsTrId("procam-settings-memory-card-not-inserted")
                        }
                    }
                    onClicked: Settings.storagePath = model.mountPath
                }
            }
        }
    }

    Label {
        //% "The selected storage is not available. Device memory will be used instead."
        text: qsTrId("procam-settings-storage-unavailable")
        visible: Settings.storagePathStatus == Settings.Unavailable
        x: Theme.horizontalPageMargin
        width: parent.width - x*2
        color: Theme.secondaryColor
        font.pixelSize: Theme.fontSizeExtraSmall
        wrapMode: Text.Wrap
    }

    ComboBox {
        //% "Photos folder"
        label: qsTrId("procam-settings-photos-folder")
        //% "Default"
        value: Settings.customPhotoDirectory !== "" ? folderName(Settings.customPhotoDirectory)
                                                    : qsTrId("procam-settings-default")
        description: folderDescription(Settings.customPhotoDirectory,
                                       Settings.customPhotoDirectoryAvailable, Settings.photoDirectory)
        enabled: AccessPolicy.cameraEnabled
        currentIndex: Settings.customPhotoDirectory !== "" ? 1 : 0
        menu: ContextMenu {
            MenuItem {
                //% "Default"
                text: qsTrId("procam-settings-default")
                onClicked: Settings.customPhotoDirectory = ""
            }
            MenuItem {
                //% "Choose folder..."
                text: qsTrId("procam-settings-choose-folder")
                onClicked: pickFolder(false)
            }
        }
    }

    ComboBox {
        //% "Videos folder"
        label: qsTrId("procam-settings-videos-folder")
        //% "Default"
        value: Settings.customVideoDirectory !== "" ? folderName(Settings.customVideoDirectory)
                                                    : qsTrId("procam-settings-default")
        description: folderDescription(Settings.customVideoDirectory,
                                       Settings.customVideoDirectoryAvailable, Settings.videoDirectory)
        enabled: AccessPolicy.cameraEnabled
        currentIndex: Settings.customVideoDirectory !== "" ? 1 : 0
        menu: ContextMenu {
            MenuItem {
                //% "Default"
                text: qsTrId("procam-settings-default")
                onClicked: Settings.customVideoDirectory = ""
            }
            MenuItem {
                //% "Choose folder..."
                text: qsTrId("procam-settings-choose-folder")
                onClicked: pickFolder(true)
            }
        }
    }

    Label {
        //% "ProCam can save only in Pictures, Videos or on a memory card. Choose a folder there; create new folders with the File Browser."
        text: qsTrId("procam-settings-folder-restriction")
        visible: folderRefused
        x: Theme.horizontalPageMargin
        width: parent.width - x*2
        color: Theme.highlightColor
        font.pixelSize: Theme.fontSizeExtraSmall
        wrapMode: Text.Wrap
    }

    SectionHeader {
        //% "Back camera"
        text: qsTrId("procam-settings-back-camera")
        opacity: AccessPolicy.cameraEnabled ? 1.0 : Theme.opacityLow
    }

    ComboBox {
        //% "Aspect ratio"
        label: qsTrId("procam-settings-aspect-ratio")
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
        //% "Front camera"
        text: qsTrId("procam-settings-front-camera")
        opacity: AccessPolicy.cameraEnabled ? 1.0 : Theme.opacityLow
    }

    ComboBox {
        //% "Aspect ratio"
        label: qsTrId("procam-settings-aspect-ratio")
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
