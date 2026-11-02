// SPDX-FileCopyrightText: 2014 - 2022 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import QtMultimedia 5.6
import Nemo.Configuration 1.0
import "settings/SimpleMode.js" as SimpleMode
import com.procam.camera 1.0

SettingsBase {
    property bool _normalizingCamera2Settings: false
    property bool _lastAppliedAdvancedMode: false
    property alias mode: modeSettings
    property alias global: globalSettings
    // Camera change goes here, CaptureView updates to global.deviceId
    property string deviceId: global.deviceId
    onDeviceIdChanged: normalizeCamera2Settings()
    property CameraExtensions camera2Capabilities: CameraExtensions {}

    readonly property int aspectRatio: mode.aspectRatio
    // Photo shape for ProCam's renderer and viewfinder: the sensor's "4:3" or
    // a centre crop to "16:9" (the per-camera aspect ratio setting).
    readonly property string photoAspect: aspectRatio === CameraConfigs.AspectRatio_16_9 ? "16:9" : "4:3"
    // Night mode: offered (moon in the top bar) when the scene is too dark,
    // switched on by the user; not remembered across restarts.
    property bool nightMode: false
    readonly property int exposureCompensationDefault: 0
    property var viewfinderGridValues: [ "none", "thirds" ]

    readonly property var settingsDefaults: ({
                                                 "iso": 0,
                                                 "timer": 0,
                                                 "viewfinderGrid": "none",
                                                 "camera2CaptureFormat": "jpeg",
                                                 "rawCaptureSpeedMode": "balanced",
                                                 "rawCaptureSize": "4096x3072",
                                                 "rawCaptureFocusMode": "auto",
                                                 "rawCaptureFocusDistance": "0",
                                                 "rawCaptureTimeout": 30,
                                                 "rawCaptureFocusTimeout": 3,
                                                 "rawCaptureFocusFailure": "capture",
                                                 "rawCaptureExposure": "1.0",
                                                 "rawCaptureIso": 0,
                                                 "rawCaptureShutterNs": "0",
                                                 "rawCaptureAperture": 0,
                                                 "rawCaptureNoiseReduction": 1,
                                                 "rawCaptureBracket": "off",
                                                 "rawCaptureJpegQuality": 92,
                                                 "rawCaptureRotation": 90,
                                                 "rawCaptureScene": "manual",
                                                 "rawCaptureProgressiveJpeg": false,
                                                 "rawRenderEngine": "enhanced",
                                                 "rawCaptureRawFormat": "raw16",
                                                 "rawCaptureColorTemperature": 0,
                                                 "rawCaptureColorTint": 0,
                                                 "camera2Viewfinder": true,
                                                 "exposureMode": Camera.ExposureManual,
                                                 "flash": ((globalSettings.captureMode == "image")
                                                           && (globalSettings.position === Camera.BackFace)
                                                           ? Camera.FlashAuto : Camera.FlashOff)
                                             })

    readonly property bool defaultSettings: modeSettings.iso === settingsDefaults["iso"]
                                            && modeSettings.timer === settingsDefaults["timer"]
                                            && modeSettings.viewfinderGrid === settingsDefaults["viewfinderGrid"]
                                            && modeSettings.camera2CaptureFormat === settingsDefaults["camera2CaptureFormat"]
                                            && modeSettings.rawCaptureSpeedMode === settingsDefaults["rawCaptureSpeedMode"]
                                            && modeSettings.rawCaptureSize === settingsDefaults["rawCaptureSize"]
                                            && modeSettings.rawCaptureFocusMode === settingsDefaults["rawCaptureFocusMode"]
                                            && modeSettings.rawCaptureFocusDistance === settingsDefaults["rawCaptureFocusDistance"]
                                            && modeSettings.rawCaptureTimeout === settingsDefaults["rawCaptureTimeout"]
                                            && modeSettings.rawCaptureFocusTimeout === settingsDefaults["rawCaptureFocusTimeout"]
                                            && modeSettings.rawCaptureFocusFailure === settingsDefaults["rawCaptureFocusFailure"]
                                            && modeSettings.rawCaptureExposure === settingsDefaults["rawCaptureExposure"]
                                            && modeSettings.rawCaptureIso === settingsDefaults["rawCaptureIso"]
                                            && modeSettings.rawCaptureShutterNs === settingsDefaults["rawCaptureShutterNs"]
                                            && modeSettings.rawCaptureAperture === settingsDefaults["rawCaptureAperture"]
                                            && modeSettings.rawCaptureNoiseReduction === settingsDefaults["rawCaptureNoiseReduction"]
                                            && modeSettings.rawCaptureBracket === settingsDefaults["rawCaptureBracket"]
                                            && modeSettings.rawCaptureJpegQuality === settingsDefaults["rawCaptureJpegQuality"]
                                            && modeSettings.rawCaptureRotation === settingsDefaults["rawCaptureRotation"]
                                            && modeSettings.rawCaptureScene === settingsDefaults["rawCaptureScene"]
                                            && modeSettings.rawCaptureProgressiveJpeg === settingsDefaults["rawCaptureProgressiveJpeg"]
                                            && modeSettings.rawRenderEngine === settingsDefaults["rawRenderEngine"]
                                            && modeSettings.rawCaptureRawFormat === settingsDefaults["rawCaptureRawFormat"]
                                            && modeSettings.rawCaptureColorTemperature === settingsDefaults["rawCaptureColorTemperature"]
                                            && modeSettings.rawCaptureColorTint === settingsDefaults["rawCaptureColorTint"]
                                            && modeSettings.camera2Viewfinder === settingsDefaults["camera2Viewfinder"]
                                            && globalSettings.exposureCompensation === exposureCompensationDefault
                                            && modeSettings.exposureMode === settingsDefaults["exposureMode"]
                                            && modeSettings.flash == settingsDefaults["flash"]

    function reset() {
        var basePath = globalSettings.path + "/" + modeSettings.path
        var i
        for (i in settingsDefaults) {
            _singleValue.key = basePath + "/" + i
            _singleValue.value = settingsDefaults[i]
        }
        _singleValue.key = globalSettings.path + "/exposureCompensation"
        _singleValue.value = exposureCompensationDefault
    }

    property ConfigurationValue _singleValue: ConfigurationValue {}

    property ConfigurationGroup _global: ConfigurationGroup {
        id: globalSettings

        path: "/apps/procam"

        // Note! don't touch this for changing between cameras, see cameraDevice on root
        property string deviceId
        property string previousBackFacingDeviceId
        property string frontFacingDeviceId
        property int position: Camera.BackFace
        property string captureMode: "image"

        // Need to be defined by adaptation to enable multiple back cameras,
        // e.g. normal, macro and wide angle camera labels could be ["1.0", "2.0", "0.6"]
        property var backCameraLabels: []

        property int portraitCaptureButtonLocation: 3
        property int landscapeCaptureButtonLocation: 5

        property string audioCodec: "audio/mpeg, mpegversion=(int)4"
        property int audioSampleRate: 48000
        property string videoCodec: "video/x-h264"
        property string mediaContainer: "video/quicktime, variant=(string)iso"

        property int videoEncodingMode: CameraRecorder.AverageBitRateEncoding
        property int videoBitRate: 12000000

        property bool saveLocationInfo
        // RAW files kept next to the JPEG, in Normal and Pro mode alike:
        // "none", "raw16" (RAW + JSON), "dng" or "both".
        property string rawCaptureSaveFormat: saveRawCaptureFiles ? "raw16" : "none"
        property bool saveRawCaptureFiles: false
        // Every frame of each burst kept as RAW (or YUV) + JSON, for
        // diagnostics and training data; about 25 MB per frame.
        property bool keepBurstFrames: false
        property bool estimatedExposureMetering: false
        property bool showExposureStatus: false
        property bool focusPeaking: false
        property bool exposureZebras: false
        property bool simpleHistogramVisible: true
        property bool experimentalExposureLayout: false
        // Full-screen viewfinder (crops the 4:3 frame) or the whole frame fitted.
        property bool viewfinderFill: true
        // HDR: every photo is a merged RAW burst rendered to JPEG, in both
        // capture formats and both modes.
        property bool hdrBurst: true
        property bool advancedMode: false
        property string advancedCamera2CaptureFormat: "jpeg"
        property string advancedRawCaptureSize: "4096x3072"
        property int advancedRawCaptureAperture: 0
        property string advancedRawCaptureFocusFailure: "capture"
        property string advancedRawCaptureSpeedMode: "balanced"
        property string advancedRawCaptureFocusMode: "auto"
        property string advancedRawCaptureFocusDistance: "0"
        property int advancedRawCaptureTimeout: 30
        property int advancedRawCaptureFocusTimeout: 3
        property string advancedRawCaptureExposure: "1.0"
        property int advancedRawCaptureIso: 0
        property string advancedRawCaptureShutterNs: "0"
        property int advancedRawCaptureNoiseReduction: 1
        property string advancedRawCaptureBracket: "off"
        property int advancedRawCaptureJpegQuality: 92
        property int advancedRawCaptureRotation: 90
        property string advancedRawCaptureScene: "manual"
        property bool advancedRawCaptureProgressiveJpeg: false
        property string advancedRawRenderEngine: "enhanced"
        property string advancedRawCaptureRawFormat: "raw16"
        property int advancedRawCaptureColorTemperature: 0
        property int advancedRawCaptureColorTint: 0

        property bool qrFilterEnabled: false
        property bool colorFiltersEnabled: false
        property bool colorFiltersAllowed: true

        property real exposureCompensation: exposureCompensationDefault
        property int whiteBalance: CameraImageProcessing.WhiteBalanceAuto

        property var exposureCompensationValues: [ 4, 3, 2, 1, 0, -1, -2, -3, -4 ]
        property string viewfinderGrid: "none"

        onPositionChanged: {
            normalizeCamera2Settings()
        }

        onAdvancedModeChanged: applyAdvancedMode()

        Component.onCompleted: {
            exposureCompensation = exposureCompensationDefault
            applyAdvancedMode()
        }

        ConfigurationGroup {
            id: modeSettings

            path: {
                var position = globalSettings.position === Camera.FrontFace ? "front" : "back"
                return position + "/" + globalSettings.captureMode
            }

            property int iso: 0
            property int flash: Camera.FlashOff
            property int exposureMode: Camera.ExposureManual
            property int meteringMode: Camera.MeteringMatrix
            property int timer: 0
            property int aspectRatio: -1
            property string camera2CaptureFormat: "jpeg"
            property string rawCaptureSpeedMode: "balanced"
            property string rawCaptureSize: "4096x3072"
            property string rawCaptureFocusMode: "auto"
            property string rawCaptureFocusDistance: "0"
            property int rawCaptureTimeout: 30
            property int rawCaptureFocusTimeout: 3
            property string rawCaptureFocusFailure: "capture"
            property string rawCaptureExposure: "1.0"
            property int rawCaptureIso: 0
            property string rawCaptureShutterNs: "0"
            property int rawCaptureAperture: 0
            property int rawCaptureNoiseReduction: 1
            property string rawCaptureBracket: "off"
            property int rawCaptureJpegQuality: 92
            property int rawCaptureRotation: 90
            property string rawCaptureScene: "manual"
            property bool rawCaptureProgressiveJpeg: false
            property string rawRenderEngine: "enhanced"
            property string rawCaptureRawFormat: "raw16"
            property int rawCaptureColorTemperature: 0
            property int rawCaptureColorTint: 0
            property bool camera2Viewfinder: true

            onCamera2CaptureFormatChanged: normalizeCamera2Settings()
            onRawCaptureRawFormatChanged: normalizeCamera2Settings()
            onRawCaptureFocusModeChanged: normalizeCamera2Settings()
            onRawCaptureJpegQualityChanged: {
                // Save edits immediately; startup restoration and Simple overrides are not edits.
                if (globalSettings.advancedMode && _lastAppliedAdvancedMode && !_normalizingCamera2Settings)
                    globalSettings.advancedRawCaptureJpegQuality = modeSettings.rawCaptureJpegQuality
            }

            Component.onCompleted: {
                // Every Advanced capture setting is saved as soon as it is
                // edited (JPEG quality has its own handler below), otherwise
                // the next start restores the previous backup.
                var saved = ["rawCaptureRotation"]
                for (var key in SimpleMode.schema) saved.push(SimpleMode.schema[key][0])
                saved.forEach(function(property) {
                    if (property === "rawCaptureJpegQuality") return
                    modeSettings[property + "Changed"].connect(function() { saveAdvancedSetting(property) })
                })
                rawCaptureIso = settingsDefaults["rawCaptureIso"]
                rawCaptureShutterNs = settingsDefaults["rawCaptureShutterNs"]
                if (rawCaptureScene === "none") {
                    rawCaptureScene = "manual"
                }
                normalizeCamera2Settings()
                if (aspectRatio === -1) {
                    if (globalSettings.captureMode === "image") {
                        aspectRatio = CameraConfigs.AspectRatio_4_3
                    } else {
                        aspectRatio = CameraConfigs.AspectRatio_16_9
                    }
                }
            }
        }
    }

    function saveAdvancedSetting(property) {
        // Edits only: startup restoration, normalisation and Simple overrides are not.
        if (globalSettings.advancedMode && _lastAppliedAdvancedMode && !_normalizingCamera2Settings)
            globalSettings["advanced" + property.charAt(0).toUpperCase() + property.slice(1)] = modeSettings[property]
    }

    function applyAdvancedMode() {
        if (_normalizingCamera2Settings) return
        _normalizingCamera2Settings = true
        try {
            var properties = ["rawCaptureRotation"]
            for (var key in SimpleMode.schema) properties.push(SimpleMode.schema[key][0])
            for (var i = 0; i < properties.length; ++i) {
                var property = properties[i]
                var saved = "advanced" + property.charAt(0).toUpperCase() + property.slice(1)
                if (globalSettings.advancedMode) {
                    modeSettings[property] = globalSettings[saved]
                } else if (_lastAppliedAdvancedMode) {
                    // Never overwrite Advanced backups when starting in Simple mode.
                    globalSettings[saved] = modeSettings[property]
                }
            }
            modeSettings.rawCaptureIso = 0
            modeSettings.rawCaptureShutterNs = "0"
            _lastAppliedAdvancedMode = globalSettings.advancedMode
        } finally {
            _normalizingCamera2Settings = false
        }
        normalizeCamera2Settings()
    }

    function applySimpleModeSettings() {
        var overrides = camera2Capabilities.camera2SimpleModeOverrides(deviceId)
        var defaults = {}
        for (var property in settingsDefaults) defaults[property] = settingsDefaults[property]
        defaults.rawCaptureScene = "hdr"
        // Normal mode renders every photo with ProCam's own pipeline: HDR
        // merges a burst, HDR off renders one RAW frame. A device profile can
        // still ask for the HAL's JPEG.
        if (camera2CaptureFormatModel().indexOf("raw") >= 0) {
            defaults.camera2CaptureFormat = "raw"
            defaults.rawCaptureRawFormat = "raw16"
        }
        // Resolve format before querying format-dependent size and RAW models.
        modeSettings.camera2CaptureFormat = SimpleMode.select("capture_format", overrides,
                    defaults.camera2CaptureFormat, camera2CaptureFormatModel())
        modeSettings.rawCaptureRawFormat = SimpleMode.select("raw_format", overrides,
                    defaults.rawCaptureRawFormat, rawCaptureRawFormatModel())
        defaults.rawCaptureSize = camera2PreferredCaptureSize(modeSettings.camera2CaptureFormat)
                                 || settingsDefaults.rawCaptureSize
        var choices = {
            capture_format: camera2CaptureFormatModel(),
            raw_format: rawCaptureRawFormatModel(),
            capture_size: camera2SizeModel(modeSettings.camera2CaptureFormat),
            scene: camera2SceneModel(),
            speed_mode: camera2SpeedModel(),
            focus_mode: camera2FocusModeModel(),
            focus_distance_diopters: camera2FocusDistanceModel(),
            focus_timeout: rawCaptureFocusTimeoutModel(),
            focus_failure: ["capture", "abort"],
            exposure_multiplier: rawCaptureExposureModel(),
            iso: camera2IsoModel(),
            shutter_ns: camera2ShutterModel(),
            noise_reduction: camera2NoiseReductionModel(),
            bracket: camera2BracketModel(),
            render_engine: modeSettings.rawCaptureRawFormat === "raw10"
                           ? ["enhanced", "internal"] : rawRenderEngineModel()
        }
        // Unsupported HDR must fall back to the ordinary scene, not another scene effect.
        if (choices.scene.indexOf(defaults.rawCaptureScene) < 0) defaults.rawCaptureScene = "manual"
        var values = SimpleMode.resolve(defaults, overrides, choices)
        for (var name in values) modeSettings[name] = values[name]
        // Simple exposure is always automatic, regardless of legacy profile values.
        modeSettings.rawCaptureIso = 0
        modeSettings.rawCaptureShutterNs = "0"
        modeSettings.rawCaptureExposure = "1.0"
        modeSettings.rawCaptureBracket = "off"
    }

    function captureModeIcon(mode) {
        switch (mode) {
        case "image": return "image://theme/icon-camera-camera-mode"
        case "video": return "image://theme/icon-camera-video"
        default:  return ""
        }
    }

    function exposureText(exposure) {
        switch (exposure) {
        case -4: return "-2"
        case -3: return "-1.5"
        case -2: return "-1"
        case -1: return "-0.5"
        case 0:  return ""
        case 1:  return "+0.5"
        case 2:  return "+1"
        case 3:  return "+1.5"
        case 4:  return "+2"
        }
    }

    function timerIcon(timer) {
        return timer > 0
                ? "image://theme/icon-camera-timer-" + timer + "s"
                : "image://theme/icon-camera-timer"
    }

    function timerText(timer) {
        return timer > 0
                ? timer + " second delay"
                : "No delay"
    }

    function rawCaptureSizeText(size) {
        return "Size " + size
    }

    function camera2SizeModel(format, rawFormat) {
        if (format === "raw") {
            var rawSizes = camera2Capabilities.camera2RawSizeModel(
                        deviceId, rawFormat || modeSettings.rawCaptureRawFormat)
            return rawSizes.length > 0 ? rawSizes
                                       : camera2Capabilities.camera2RawSizeModel(
                                             deviceId, "raw16")
        }
        return camera2Capabilities.camera2JpegSizeModel(deviceId)
    }

    function camera2PreferredCaptureSize(format) {
        return camera2Capabilities.camera2PreferredCaptureSize(
                    deviceId, format || modeSettings.camera2CaptureFormat)
    }

    function camera2WarmCaptureSize() {
        return camera2Capabilities.camera2WarmCaptureSize(deviceId)
    }

    function camera2WarmJpegCaptureSupported(size) {
        return camera2Capabilities.camera2WarmJpegCaptureSupported(deviceId, size)
    }

    function camera2WarmRawCaptureSupported(size) {
        return camera2Capabilities.camera2WarmRawCaptureSupported(deviceId, size)
    }

    // JPEG mode renders single shots from a preview RAW16 frame with
    // ProCam's own pipeline, like HDR and RAW mode (the HAL's JPEG is far
    // softer); the HAL's JPEG is only the fallback without a RAW16 stream.
    function camera2JpegFromRaw(cameraId) {
        return modeSettings.camera2CaptureFormat === "jpeg"
                && camera2Capabilities.camera2BurstRawSize(cameraId !== undefined ? cameraId : deviceId, "") !== ""
    }

    // HDR burst input: "raw16", or "yuv" for cameras without RAW output.
    function camera2BurstFormat(cameraId) {
        return camera2Capabilities.camera2BurstFormat(cameraId !== undefined ? cameraId : deviceId)
    }

    // Pass the camera id from bindings so they re-evaluate on camera changes.
    function camera2BurstSupported(cameraId) {
        return camera2Capabilities.camera2BurstSupported(cameraId !== undefined ? cameraId : deviceId)
    }

    function camera2BurstRawSize() {
        return camera2Capabilities.camera2BurstRawSize(
                    deviceId, modeSettings.camera2CaptureFormat === "raw" ? modeSettings.rawCaptureSize : "")
    }

    function camera2PreferredPreviewSize(cameraId) {
        return camera2Capabilities.camera2PreferredPreviewSize(cameraId)
    }

    function camera2PreviewOrientation(cameraId, fallback) {
        return camera2Capabilities.camera2PreviewOrientation(cameraId, fallback)
    }

    function camera2PreviewMirror(cameraId, fallback) {
        return camera2Capabilities.camera2PreviewMirror(cameraId, fallback)
    }

    function camera2SceneModel() {
        return camera2Capabilities.camera2SceneModel(deviceId)
    }

    function camera2NoiseReductionModel() {
        return camera2Capabilities.camera2NoiseReductionModel(deviceId)
    }

    function camera2BracketModel() {
        return mode.camera2CaptureFormat === "jpeg" ? ["off"]
                : camera2Capabilities.camera2BracketModel(deviceId)
    }

    function rawCaptureBracketText(mode) {
        return "Bracket " + rawCaptureBracketLabel(mode)
    }

    function rawCaptureBracketLabel(mode) {
        switch (mode) {
        case "ev3": return "6-stop pair"
        case "ev2": return "4-stop pair"
        case "ev1": return "2-stop pair"
        default: return "Off"
        }
    }

    function camera2HalNoiseReduction(mode) {
        return mode
    }

    function normalizeCamera2Settings() {
        if (_normalizingCamera2Settings) return
        _normalizingCamera2Settings = true
        try {
            if (!globalSettings.advancedMode) applySimpleModeSettings()
            var captureFormats = camera2CaptureFormatModel()
            if (captureFormats.length > 0 && captureFormats.indexOf(modeSettings.camera2CaptureFormat) < 0)
                modeSettings.camera2CaptureFormat = captureFormats[0]
            var rawFormats = rawCaptureRawFormatModel()
            if (rawFormats.indexOf(modeSettings.rawCaptureRawFormat) < 0) {
                modeSettings.rawCaptureRawFormat = rawFormats.length > 0
                        ? rawFormats[0] : settingsDefaults["rawCaptureRawFormat"]
                if (globalSettings.advancedMode)
                    globalSettings.advancedRawCaptureRawFormat = modeSettings.rawCaptureRawFormat
            }
            var sizes = camera2SizeModel(modeSettings.camera2CaptureFormat)
            var preferredSize = camera2PreferredCaptureSize(modeSettings.camera2CaptureFormat)
            if (sizes.length > 0 && sizes.indexOf(modeSettings.rawCaptureSize) < 0) {
                modeSettings.rawCaptureSize = sizes.indexOf(preferredSize) >= 0
                        ? preferredSize : sizes[0]
            } else if (globalSettings.advancedMode && sizes.length > 0 &&
                       modeSettings.rawCaptureSize === settingsDefaults["rawCaptureSize"] &&
                       sizes.indexOf(preferredSize) >= 0) {
                modeSettings.rawCaptureSize = preferredSize
            }
            if (camera2SpeedModel().indexOf(modeSettings.rawCaptureSpeedMode) < 0) {
                modeSettings.rawCaptureSpeedMode = settingsDefaults["rawCaptureSpeedMode"]
            }
            if (modeSettings.rawCaptureIso === undefined ||
                    modeSettings.rawCaptureIso === null) {
                modeSettings.rawCaptureIso = settingsDefaults["rawCaptureIso"]
            }
            if (!modeSettings.rawCaptureShutterNs) {
                modeSettings.rawCaptureShutterNs = settingsDefaults["rawCaptureShutterNs"]
            }
            var isoModel = camera2IsoModel()
            var isoIndex = isoModel.indexOf(modeSettings.rawCaptureIso)
            if (isoIndex < 0) {
                isoIndex = isoModel.indexOf(parseInt(modeSettings.rawCaptureIso))
            }
            if (!globalSettings.advancedMode)
                modeSettings.rawCaptureIso = isoIndex >= 0 ? isoModel[isoIndex] : 0
            var shutterModel = camera2ShutterModel()
            var requestedShutterNs = parseInt(modeSettings.rawCaptureShutterNs)
            var shutterIndex = shutterModel.indexOf(String(modeSettings.rawCaptureShutterNs))
            if (shutterIndex < 0) {
                shutterIndex = shutterModel.indexOf(modeSettings.rawCaptureShutterNs)
            }
            if (shutterIndex >= 0) {
                modeSettings.rawCaptureShutterNs = shutterModel[shutterIndex]
            } else if (!globalSettings.advancedMode && requestedShutterNs > 0 && shutterModel.length > 1) {
                modeSettings.rawCaptureShutterNs = shutterModel[shutterModel.length - 1]
                console.log("Shutter limited to " +
                            rawCaptureShutterLabel(modeSettings.rawCaptureShutterNs))
            } else if (!globalSettings.advancedMode) {
                modeSettings.rawCaptureShutterNs = settingsDefaults["rawCaptureShutterNs"]
            }
            var focusModel = camera2FocusModeModel()
            if (focusModel.indexOf(modeSettings.rawCaptureFocusMode) < 0) {
                modeSettings.rawCaptureFocusMode = focusModel[0]
            }
            var distances = modeSettings.rawCaptureFocusMode === "manual"
                    ? camera2ManualFocusDistanceModel() : camera2FocusDistanceModel()
            if (distances.indexOf(modeSettings.rawCaptureFocusDistance) < 0) {
                modeSettings.rawCaptureFocusDistance = distances.length ? distances[0] : "0"
            }
            var focusTimeoutModel = rawCaptureFocusTimeoutModel()
            if (focusTimeoutModel.indexOf(modeSettings.rawCaptureFocusTimeout) < 0) {
                modeSettings.rawCaptureFocusTimeout = focusTimeoutModel.indexOf(settingsDefaults["rawCaptureFocusTimeout"]) >= 0
                        ? settingsDefaults["rawCaptureFocusTimeout"] : focusTimeoutModel[0]
            }
            var exposureModel = rawCaptureExposureModel()
            if (exposureModel.indexOf(modeSettings.rawCaptureExposure) < 0) {
                modeSettings.rawCaptureExposure = exposureModel.indexOf(settingsDefaults["rawCaptureExposure"]) >= 0
                        ? settingsDefaults["rawCaptureExposure"] : exposureModel[0]
            }
            var rotationModel = rawCaptureRotationModel()
            if (rotationModel.indexOf(modeSettings.rawCaptureRotation) < 0) {
                modeSettings.rawCaptureRotation = rotationModel.indexOf(settingsDefaults["rawCaptureRotation"]) >= 0
                        ? settingsDefaults["rawCaptureRotation"] : rotationModel[0]
            }
            if (camera2SceneModel().indexOf(modeSettings.rawCaptureScene) < 0) {
                modeSettings.rawCaptureScene = "manual"
            }
            if (camera2NoiseReductionModel().indexOf(modeSettings.rawCaptureNoiseReduction) < 0) {
                // HAL noise reduction (FAST keeps the preview frame rate) when available.
                modeSettings.rawCaptureNoiseReduction =
                        camera2NoiseReductionModel().indexOf(settingsDefaults["rawCaptureNoiseReduction"]) >= 0
                        ? settingsDefaults["rawCaptureNoiseReduction"] : 0
            }
            if (camera2BracketModel().indexOf(modeSettings.rawCaptureBracket) < 0) {
                modeSettings.rawCaptureBracket = settingsDefaults["rawCaptureBracket"]
            }
            if (rawRenderEngineModel().indexOf(modeSettings.rawRenderEngine) < 0) {
                modeSettings.rawRenderEngine = settingsDefaults["rawRenderEngine"]
            }
        } finally {
            _normalizingCamera2Settings = false
        }
    }

    function camera2CaptureFormatText(format) {
        return format === "jpeg" ? "Direct JPEG" : "RAW render"
    }

    function camera2CaptureFormatModel() {
        return camera2Capabilities.camera2CaptureFormatModel(deviceId)
    }

    function camera2LensModel() {
        var model = camera2Capabilities.camera2LensModel(deviceId)
        var lenses = []
        for (var i = 0; i < model.length; ++i) {
            if (model[i] !== globalSettings.frontFacingDeviceId) {
                lenses.push(model[i])
            }
        }
        if (lenses.length > 0) {
            return lenses
        }
        return model.length > 0 ? model : [ deviceId ]
    }

    // Camera2 ids by role ({front, main, ultrawide, tele}, "" where missing), with
    // Qt Multimedia's front camera as the fallback for bridges that do not
    // report lens facing.
    function camera2CameraRoles() {
        var roles = camera2Capabilities.camera2CameraRoles()
        var ids = camera2Capabilities.camera2LensModel(deviceId)
        if (!roles.front && ids.indexOf(globalSettings.frontFacingDeviceId) >= 0) {
            roles.front = globalSettings.frontFacingDeviceId
        }
        if (!roles.main) {
            roles.main = ids.indexOf(globalSettings.previousBackFacingDeviceId) >= 0
                    && globalSettings.previousBackFacingDeviceId !== roles.front
                    ? globalSettings.previousBackFacingDeviceId : (ids.length > 0 ? ids[0] : "0")
        }
        return roles
    }

    function camera2LensFacing(cameraId) {
        return camera2Capabilities.camera2LensFacing(cameraId || deviceId)
    }

    function camera2LensLabel(cameraId) {
        return camera2Capabilities.camera2LensLabel(cameraId || deviceId)
    }

    function camera2SpeedModel() {
        return [ "fast", "balanced" ]
    }

    function rawCaptureSpeedText(mode) {
        return "Speed " + rawCaptureSpeedLabel(mode)
    }

    function rawCaptureSpeedLabel(mode) {
        switch (mode) {
        case "fast": return "Fast"
        default: return "Balanced"
        }
    }

    function rawCaptureFocusModeText(mode) {
        switch (mode) {
        case "auto": return "Focus auto"
        case "continuous": return "Focus continuous"
        case "manual": return "Focus manual"
        case "infinity": return "Focus infinity"
        case "none": return "Focus none"
        default: return "Focus " + mode
        }
    }

    function rawCaptureFocusDistanceText(distance) {
        return "Focus " + rawCaptureFocusDistanceLabel(distance)
    }

    function rawCaptureFocusDistanceLabel(distance) {
        var diopters = parseFloat(distance)
        if (!diopters || diopters <= 0) {
            return "Infinity"
        }

        var meters = 1.0 / diopters
        return meters >= 1.0
                ? meters.toFixed(meters >= 10 ? 0 : 1) + " m"
                : Math.round(meters * 100) + " cm"
    }

    function camera2FocusDistanceModel() {
        return camera2Capabilities.camera2FocusDistanceModel(deviceId)
    }

    function camera2ManualFocusDistanceModel() {
        return camera2FocusDistanceModel().filter(function(value) { return Number(value) > 0 })
    }

    function camera2FocusModeModel() {
        var values = camera2Capabilities.camera2FocusModeModel(deviceId).filter(function(value) {
            return value !== "none"
        })
        // Keep the internal no-focus fallback for fixed-focus or unavailable cameras.
        return values.length ? values : ["none"]
    }

    function camera2SelectableFocusModeModel() {
        return camera2FocusModeModel().filter(function(value) { return value !== "none" })
    }

    function rawCaptureTimeoutText(timeout) {
        return "Capture " + timeout + " s"
    }

    function rawCaptureFocusTimeoutText(timeout) {
        return "AF " + timeout + " s"
    }

    function rawCaptureFocusFailureText(policy) {
        return policy === "abort" ? "AF abort" : "AF capture"
    }

    function rawCaptureExposureText(exposure) {
        return "Exposure x" + exposure
    }

    function rawCaptureExposureModel() {
        return camera2Capabilities.rawCaptureExposureModel(deviceId)
    }

    function rawCaptureFocusTimeoutModel() {
        return camera2Capabilities.rawCaptureFocusTimeoutModel(deviceId)
    }

    function rawCaptureIsoText(iso) {
        return iso > 0 ? "ISO " + iso : "ISO auto"
    }

    function rawCaptureShutterText(shutterNs) {
        return "Shutter " + rawCaptureShutterLabel(shutterNs)
    }

    function rawCaptureShutterLabel(shutterNs) {
        switch (shutterNs) {
        case "100000": return "1/10000"
        case "250000": return "1/4000"
        case "500000": return "1/2000"
        case "1000000": return "1/1000"
        case "2000000": return "1/500"
        case "4000000": return "1/250"
        case "8333333": return "1/120"
        case "16666667": return "1/60"
        case "33333333": return "1/30"
        case "66666667": return "1/15"
        case "125000000": return "1/8"
        case "250000000": return "1/4"
        case "400000000": return "0.4s"
        case "500000000": return "1/2"
        case "1000000000": return "1s"
        case "2000000000": return "2s"
        case "4000000000": return "4s"
        case "8000000000": return "8s"
        case "16000000000": return "16s"
        default:
            var ns = parseInt(shutterNs)
            if (ns > 0) {
                return ns < 1000000000
                        ? "1/" + Math.round(1000000000 / ns)
                        : (ns / 1000000000).toFixed(ns % 1000000000 === 0 ? 0 : 1) + "s"
            }
            return "auto"
        }
    }

    function camera2IsoModel() {
        return camera2Capabilities.camera2IsoModel(deviceId)
    }

    function camera2ShutterModel() {
        return camera2Capabilities.camera2ShutterModel(deviceId)
    }

    function camera2MaximumZoom() {
        return camera2Capabilities.camera2MaximumZoom(deviceId)
    }

    function rawCaptureApertureText(aperture) {
        return aperture > 0 ? "Aperture f/" + rawCaptureApertureLabel(aperture)
                            : "Aperture auto"
    }

    function rawCaptureApertureLabel(aperture) {
        return aperture > 0 ? (aperture / 10).toFixed(1) : "Auto"
    }

    function rawCaptureNoiseReductionText(mode) {
        return "Noise " + rawCaptureNoiseReductionLabel(mode)
    }

    function rawCaptureNoiseReductionLabel(mode) {
        switch (mode) {
        case 1: return "Fast"
        case 2: return "High"
        case 3: return "Minimal"
        case 4: return "ZSL"
        default: return "Off"
        }
    }

    function rawCaptureJpegQualityText(quality) {
        return "JPEG " + quality
    }

    function rawCaptureRotationText(rotation) {
        return rotation === 0 ? "Rotate 0" : "Rotate " + rotation
    }

    function rawCaptureRotationModel(mode) {
        var effectiveMode = mode || (!globalSettings.advancedMode
                                     ? "simple"
                                     : modeSettings.camera2CaptureFormat)
        return camera2Capabilities.rawCaptureRotationModel(
                    deviceId, effectiveMode)
    }

    function rawCaptureSceneText(scene) {
        return "Scene " + rawCaptureSceneLabel(scene)
    }

    function rawCaptureSceneLabel(scene) {
        switch (scene) {
        case "manual": return "Manual"
        case "portrait": return "Portrait"
        case "landscape": return "Landscape"
        case "sport": return "Sport"
        case "night": return "Night"
        case "auto": return "Auto"
        case "action": return "Action"
        case "night-portrait": return "Night portrait"
        case "theatre": return "Theatre"
        case "beach": return "Beach"
        case "snow": return "Snow"
        case "sunset": return "Sunset"
        case "steady-photo": return "Steady photo"
        case "fireworks": return "Fireworks"
        case "party": return "Party"
        case "candlelight": return "Candlelight"
        case "barcode": return "Barcode"
        case "hdr": return "HDR"
        default: return scene
        }
    }

    function rawCaptureProgressiveJpegText(enabled) {
        return enabled ? "Progressive JPEG" : "Standard JPEG"
    }

    function rawRenderEngineModel() {
        return [ "enhanced", "internal", "fastjpeg" ]
    }

    function rawRenderEngineText(engine) {
        switch (engine) {
        case "enhanced": return "Enhanced renderer"
        case "fastjpeg": return "Fast JPEG converter"
        default: return "Internal renderer"
        }
    }

    function rawRenderEngineLabel(engine) {
        switch (engine) {
        case "enhanced": return "Enhanced"
        case "fastjpeg": return "Fast JPEG"
        default: return "Internal"
        }
    }

    function rawCaptureRawFormatModel() {
        return camera2Capabilities.rawCaptureRawFormatModel(deviceId)
    }

    function rawCaptureRawFormatText(format) {
        return format === "raw10" ? "RAW10 packed" : "RAW16"
    }

    function rawCaptureRawFormatLabel(format) {
        return format === "raw10" ? "RAW10" : "RAW16"
    }

    function rawCaptureColorTemperatureText(temperature) {
        return temperature > 0 ? "WB " + temperature + " K" : "Auto WB"
    }

    function rawCaptureColorTintText(tint) {
        return tint === 0 ? "Tint 0" : "Tint " + tint
    }

    function colorFiltersIcon(enabled) {
        return "image://theme/icon-camera-filter-" + (enabled ? "on" : "off")
    }

    function colorFiltersEnabledText(enabled) {
        return enabled
                ? "Color filters on"
                : "Color filters off"
    }

    function isoText(iso) {
        if (iso == 0) {
            return "Light sensitivity - Automatic"
        } else {
            return "Light sensitivity - ISO " + iso
        }
    }

    function meteringModeIcon(mode) {
        switch (mode) {
        case Camera.MeteringMatrix:  return "image://theme/icon-camera-metering-matrix"
        case Camera.MeteringAverage: return "image://theme/icon-camera-metering-weighted"
        case Camera.MeteringSpot:    return "image://theme/icon-camera-metering-spot"
        }
    }

    function exposureModeIcon(exposureMode) {
        switch (exposureMode) {
        case Camera.ExposureManual:         return "image://theme/icon-camera-mode-automatic"
        case Camera.ExposurePortrait:       return "image://theme/icon-camera-mode-portrait"
        case Camera.ExposureNight:          return "image://theme/icon-camera-mode-night"
        case Camera.ExposureSports:         return "image://theme/icon-camera-mode-sports"
        case Camera.ExposureHDR:            return "image://theme/icon-camera-mode-hdr"
        default:
            return "" // not supported
        }
    }

    function exposureModeText(exposureMode) {
        switch (exposureMode) {
        case Camera.ExposureManual:         return "Automatic exposure"
        case Camera.ExposurePortrait:       return "Portrait exposure"
        case Camera.ExposureNight:          return "Night exposure"
        case Camera.ExposureSports:         return "Sports exposure"
        case Camera.ExposureHDR:            return "HDR exposure"
        default:
            return "" // not supported
        }
    }

    function flashIcon(flash) {
        switch (flash) {
        case Camera.FlashAuto:              return "image://theme/icon-camera-flash-automatic"
        case Camera.FlashOff:               return "image://theme/icon-camera-flash-off"
        case Camera.FlashTorch:
        case Camera.FlashOn:                return "image://theme/icon-camera-flash-on"
        // JB#54201: Red-eye mode does not work
        // case Camera.FlashRedEyeReduction:   return "image://theme/icon-camera-flash-redeye"
        default:
            return "" // not supported
        }
    }

    function flashText(flash) {
        switch (flash) {
        case Camera.FlashAuto:       return "Flash automatic"
        case Camera.FlashOff:        return "Flash disabled"
        case Camera.FlashOn:         return "Flash enabled"
        case Camera.FlashTorch:      return "Flash on"
        case Camera.FlashRedEyeReduction: return "Flash red eye"
        default:
            return "" // not supported
        }
    }

    function whiteBalanceIcon(balance) {
        switch (balance) {
        case CameraImageProcessing.WhiteBalanceAuto:        return "image://theme/icon-camera-wb-automatic"
        case CameraImageProcessing.WhiteBalanceSunlight:    return "image://theme/icon-camera-wb-sunny"
        case CameraImageProcessing.WhiteBalanceCloudy:      return "image://theme/icon-camera-wb-cloudy"
        // case CameraImageProcessing.WhiteBalanceShade:       return "image://theme/icon-camera-wb-shade"
        // case CameraImageProcessing.WhiteBalanceSunset:      return "image://theme/icon-camera-wb-sunset"
        case CameraImageProcessing.WhiteBalanceFluorescent: return "image://theme/icon-camera-wb-fluorecent"
        case CameraImageProcessing.WhiteBalanceTungsten:    return "image://theme/icon-camera-wb-tungsten"
        default:
            return "" // not supported
        }
    }

    function whiteBalanceText(balance) {
        switch (balance) {
        case CameraImageProcessing.WhiteBalanceAuto:        return "Automatic"
        case CameraImageProcessing.WhiteBalanceSunlight:    return "Sunny"
        case CameraImageProcessing.WhiteBalanceCloudy:      return "Cloudy"
        case CameraImageProcessing.WhiteBalanceShade:       return "Shade"
        case CameraImageProcessing.WhiteBalanceSunset:      return "Sunset"
        case CameraImageProcessing.WhiteBalanceFluorescent: return "Fluorescent"
        case CameraImageProcessing.WhiteBalanceTungsten:    return "Tungsten"
        default:
            return "" // not supported
        }
    }

    function colorFilterText(filter) {
        switch (filter) {
        case CameraImageProcessing.ColorFilterNone:
            return "Normal"
        case CameraImageProcessing.ColorFilterGrayscale:
            return "Grayscale"
        case CameraImageProcessing.ColorFilterNegative:
            return "Negative"
        case CameraImageProcessing.ColorFilterSolarize:
            return "Solarize"
        case CameraImageProcessing.ColorFilterSepia:
            return "Sepia"
        case CameraImageProcessing.ColorFilterPosterize:
            return "Posterize"
        case CameraImageProcessing.ColorFilterWhiteboard:
            return "Whiteboard"
        case CameraImageProcessing.ColorFilterBlackboard:
            return "Blackboard"
        case CameraImageProcessing.ColorFilterAqua:
            return "Aqua"
        default:
            return "" // not supported
        }
    }

    function viewfinderGridIcon(grid) {
        switch (grid) {
        case "none": return "image://theme/icon-camera-grid-none"
        case "thirds": return "image://theme/icon-camera-grid-thirds"
        default: return ""
        }
    }

    function viewfinderGridText(grid) {
        switch (grid) {
        case "none":
            return "No grid"
        case "thirds":
            return "Thirds grid"
        default: return ""
        }
    }
}
