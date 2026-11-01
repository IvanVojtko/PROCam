// SPDX-License-Identifier: BSD-3-Clause
import QtQuick 2.0
import com.procam.camera 1.0
import "ExposureMath.js" as ExposureMath

Item {
    id: control
    visible: false
    property var preview
    property bool busy: false
    property bool active: false
    property string cameraId: Settings.deviceId || "0"
    property var capabilities: Settings.camera2Capabilities.camera2ExposureCapabilities(cameraId)
    property int isoLock: Settings.global.advancedMode ? Settings.mode.rawCaptureIso : 0
    property real timeLock: Settings.global.advancedMode ? Number(Settings.mode.rawCaptureShutterNs) : 0
    readonly property bool fullyManual: isoLock > 0 && timeLock > 0
    readonly property bool partial: (isoLock > 0) !== (timeLock > 0)
    readonly property bool manualSupported: capabilities.manual_supported === true &&
                                            capabilities.result_exposure_supported === true && ExposureMath.validLimits(capabilities)
    readonly property bool estimatedEnabled: Settings.global.estimatedExposureMetering &&
                                             capabilities.result_exposure_supported === true
    readonly property bool meteringReady: estimatedEnabled && reference > 0 && fresh &&
                                          !responseInvalid && sampleReason === "valid"
    readonly property bool canLock: active && !busy && manualSupported &&
                                   (fullyManual || (fresh && actualIso > 0 && actualTime > 0))
    // Brightness set on the tap-to-focus slider (EV), in either mode.
    property real tapEv: 0
    // The EV wheel is a Pro control; Normal mode meters at 0 EV plus the tap slider.
    readonly property real selectedEv: (Settings.global.advancedMode ? Settings.global.exposureCompensation / 2.0 : 0)
                                       + tapEv
    readonly property var hardwareEv: ExposureMath.compensation(selectedEv, capabilities)
    readonly property real effectiveEv: fullyManual ? 0 : partial ? selectedEv : hardwareEv.ev
    readonly property int compensationSteps: isoLock > 0 || timeLock > 0 ? 0 : hardwareEv.steps
    readonly property string scene: isoLock > 0 || timeLock > 0 ? "manual" : Settings.mode.rawCaptureScene
    readonly property bool fresh: lastSampleMs > 0 && clockMs - lastSampleMs < Math.max(1500, appliedTime / 1000000 + 1000)
    // Auto exposure: ready once it has settled, or a second after the last
    // change when it keeps adjusting (some cameras, e.g. face-priority AE on
    // the front camera, rarely report a converged state); the burst plan
    // works from the current readings anyway. Manual pairs must be applied.
    readonly property bool captureReady: (!partial && !fullyManual &&
                                         (!capabilities.result_exposure_supported
                                          || (fresh && (autoReady || clockMs - lastCommandMs > 1000)))) ||
                                         (fresh && acknowledged && appliedIso > 0 && appliedTime > 0 &&
                                          (!isoLock || appliedIso === isoLock) && (!timeLock || appliedTime === timeLock))
    property int appliedIso: 0
    property real appliedTime: 0
    property int actualIso: 0
    property real actualTime: 0
    property real reference: 0
    property real meterEv: 0
    property bool meterValid: false
    property bool acknowledged: false
    property bool autoReady: false
    property string status: ""
    property string limitStatus: ""
    property real residualEv: 0
    property real lastTimestamp: 0
    property real lastSampleMs: 0
    property real lastCommandMs: 0
    property real lastCommandTimestamp: 0
    property real clockMs: Date.now()
    property real filteredError: 0
    property int stableSamples: 0
    property bool changing: false
    property real lastLuminance: 0
    property real lastProduct: 0
    property int responseFailures: 0
    property bool responseInvalid: false

    property string sampleReason: "waiting_calibration"
    property string lastLogState: ""
    property string loggedCamera: ""
    property string lastRejection: ""
    property int resultAeState: -1
    property int resultAeMode: -1
    property real resultEvSteps: NaN
    property string notice: ""
    Timer { id: noticeTimer; interval: 2000; onTriggered: control.notice = "" }

    Timer {
        interval: 250; running: control.active; repeat: true
        onTriggered: {
            control.clockMs = Date.now()
            if (control.busy) return
            if (!control.fresh) {
                control.meterValid = false
                if (control.partial) control.status = "Auto paused: waiting for exposure results"
            }
            control.logState()
        }
    }
    Connections {
        target: control.preview
        onExposureSample: control.sample(sample)
        onRunningChanged: {
            if (control.preview && control.preview.running) control.send()
            else { control.meterValid = false; control.lastSampleMs = 0 }
        }
    }
    onCameraIdChanged: reset()
    onActiveChanged: {
        if (active && !busy) reset()
        else meterValid = false
    }
    onBusyChanged: if (!busy && active) send()
    onIsoLockChanged: if (!changing) reconcile()
    onTimeLockChanged: if (!changing) reconcile()
    onCompensationStepsChanged: send()
    onEffectiveEvChanged: { filteredError = 0; limitStatus = "" }
    onEstimatedEnabledChanged: {
        invalidateCalibration("setting_changed")
        if (!estimatedEnabled && partial) freezeManual()
    }
    Connections {
        target: Settings.global
        onAdvancedModeChanged: control.reset()
    }
    Connections {
        target: Settings.mode
        onRawCaptureApertureChanged: control.invalidateCalibration("aperture_changed")
    }
    Component.onCompleted: reset()

    function meterReason() {
        if (!Settings.global.estimatedExposureMetering) return "disabled"
        if (!capabilities.result_exposure_supported) return "unsupported_results"
        if ((partial || fullyManual) && !manualSupported) return "unsupported_manual"
        if (!lastSampleMs) return "missing_results"
        if (!fresh) return "stale_results"
        if (appliedIso > 0 && !acknowledged) return "unacknowledged_exposure"
        if (responseInvalid) return "unreliable_brightness"
        return sampleReason
    }
    function logEvent(event) {
        console.log("exposure-control " + event + " " + JSON.stringify(diagnostics()))
    }
    function logState() {
        // Normal in-flight requests are brief; report only persistent mismatch.
        if (meterReason() === "unacknowledged_exposure" && clockMs - lastCommandMs <= 1000) return
        var key = [cameraId, isoLock > 0, timeLock > 0, meterReason(), limitStatus, reference > 0].join(":")
        if (key === lastLogState) return
        lastLogState = key
        logEvent("state")
    }
    function invalidateCalibration(reason) {
        var calibrated = reference > 0
        reference = 0; stableSamples = 0; meterValid = false
        sampleReason = "waiting_calibration"
        if (calibrated) logEvent("calibration_invalidated:" + reason)
    }
    function reset() {
        if (loggedCamera !== cameraId) { lastLogState = ""; loggedCamera = cameraId }
        lastRejection = ""; notice = ""
        resultAeState = -1; resultAeMode = -1; resultEvSteps = NaN
        invalidateCalibration("session_reset")
        changing = true
        Settings.mode.rawCaptureIso = 0
        Settings.mode.rawCaptureShutterNs = "0"
        changing = false
        reference = 0; stableSamples = 0; lastSampleMs = 0; lastTimestamp = 0
        meterValid = false; acknowledged = false; autoReady = false; limitStatus = ""; status = ""
        lastLuminance = 0; lastProduct = 0; responseFailures = 0; responseInvalid = false
        appliedIso = 0; appliedTime = 0
        send()
    }
    function send() {
        if (!preview || !active || busy) return
        preview.setExposurePair(appliedIso, String(Math.round(appliedTime)), compensationSteps)
        lastCommandMs = Date.now()
        lastCommandTimestamp = lastTimestamp
        acknowledged = false
        autoReady = false
    }
    function setPair(iso, time) {
        if (appliedIso === iso && appliedTime === time) return
        appliedIso = iso; appliedTime = time
        send()
    }
    function rejectSelection(parameter, value, reason, message) {
        status = message
        var key = [cameraId, parameter, value, reason, meterReason()].join(":")
        if (lastRejection !== key) {
            lastRejection = key
            logEvent("selection_rejected:" + parameter + ":" + value + ":" + reason)
        }
        return false
    }
    function setLock(parameter, value) {
        if (busy || !active) return false
        value = Number(value)
        if ((parameter !== "iso" && parameter !== "shutter") || !isFinite(value) || value < 0) return false
        if (value > 0 && !canLock) {
            return rejectSelection(parameter, value, manualSupported ? "missing_actual" : "unsupported_manual",
                                   manualSupported ? "Waiting for exposure results" : "Manual exposure unavailable")
        }
        var nextIso = parameter === "iso" ? value : isoLock
        var nextTime = parameter === "shutter" ? value : timeLock
        var fallback = ""
        if (value > 0 && !meteringReady && !fullyManual) {
            // Preserve an existing lock; initialize only the automatic companion.
            if (!nextIso) nextIso = actualIso
            if (!nextTime) nextTime = actualTime
            fallback = "manual_from_actual"
        } else if (value === 0 && !meteringReady && ((nextIso > 0) !== (nextTime > 0))) {
            nextIso = 0; nextTime = 0
            fallback = "both_auto"
        }
        if (value > 0) {
            var l = ExposureMath.limits(capabilities)
            if ((nextIso && (nextIso < l.isoMin || nextIso > l.isoMax)) ||
                    (nextTime && (nextTime < l.timeMin || nextTime > l.timeMax))) {
                return rejectSelection(parameter, value, "outside_limits", "Manual selection exceeds camera limits")
            }
        }
        changing = true
        Settings.mode.rawCaptureIso = nextIso
        Settings.mode.rawCaptureShutterNs = String(nextTime)
        changing = false
        lastRejection = ""
        status = ""
        reconcile()
        if (fallback) {
            notice = fallback === "both_auto" ? "Both Auto: metering unavailable" : "Full manual: metering unavailable"
            noticeTimer.restart()
            logEvent("fallback:" + fallback)
        }
        logState()
        return true
    }
    function freezeManual() {
        if (!(appliedIso > 0 && appliedTime > 0)) return
        changing = true
        Settings.mode.rawCaptureIso = appliedIso
        Settings.mode.rawCaptureShutterNs = String(Math.round(appliedTime))
        changing = false
        status = ""
        reconcile()
        logEvent("fallback:freeze_manual")
    }
    function reconcile() {
        meterValid = false; filteredError = 0; limitStatus = ""
        if (!isoLock && !timeLock) { setPair(0, 0); return }
        if (!manualSupported) { reset(); status = "Manual exposure unavailable"; return }
        if (fullyManual) {
            var l = ExposureMath.limits(capabilities)
            if (isoLock < l.isoMin || isoLock > l.isoMax || timeLock < l.timeMin || timeLock > l.timeMax) {
                status = "Manual selection exceeds camera limits"; acknowledged = false; return
            }
            setPair(isoLock, timeLock)
        } else if (reference > 0 && fresh && estimatedEnabled) {
            var p = actualIso * actualTime
            var solution = ExposureMath.solve(p, isoLock, timeLock, capabilities, 0)
            setPair(solution.iso, solution.time)
        }
    }
    function aeEligible(values) {
        var state = Number(values.ae)
        return Number(values.ae_mode) > 0 && Number(values.ev_steps) === hardwareEv.steps &&
                (state === 2 || state === 3 || state === 4)
    }
    function sample(values) {
        if (!active || busy || values.camera !== cameraId) return
        var timestamp = Number(values.timestamp), iso = Number(values.iso), time = Number(values.shutter)
        if (!(timestamp > lastTimestamp && iso > 0 && time > 0)) return
        resultAeState = Number(values.ae); resultAeMode = Number(values.ae_mode)
        resultEvSteps = Number(values.ev_steps)
        lastTimestamp = timestamp; actualIso = iso; actualTime = time
        clockMs = lastSampleMs = Date.now()
        if (appliedIso > 0) {
            acknowledged = Number(values.ae_mode) === 0 && timestamp > lastCommandTimestamp &&
                    ExposureMath.samePair(appliedIso, appliedTime, iso, time)
            if (!acknowledged) {
                meterValid = false
                status = "Waiting for requested exposure"
                return
            }
        } else {
            autoReady = aeEligible(values)
            acknowledged = autoReady
        }
        var luminance = Number(values.luminance)
        if (!estimatedEnabled || !(luminance > 0.002 && luminance < 0.98) || Number(values.clipped) > 0.6) {
            sampleReason = "clipped_brightness"
            meterValid = false; stableSamples = 0
            status = partial ? "Auto paused: metering unavailable" : ""
            return
        }
        if (!isoLock && !timeLock) {
            meterValid = false
            sampleReason = "waiting_ae"
            if (!aeEligible(values)) { stableSamples = 0; return }
            sampleReason = "waiting_calibration"
            stableSamples = Math.min(3, stableSamples + 1)
            if (stableSamples < 3) return
            var acquired = reference <= 0 || responseInvalid
            reference = luminance / Math.pow(2, hardwareEv.ev)
            responseInvalid = false; responseFailures = 0
            if (acquired) logEvent("calibration_acquired")
        }
        if (!(reference > 0) || responseInvalid) {
            sampleReason = "waiting_calibration"
            meterValid = false; status = partial ? "Auto paused: return both wheels to Auto to calibrate" : ""; return
        }
        var product = iso * time
        if (lastProduct > 0 && lastLuminance > 0 && partial) {
            var change = ExposureMath.stops(product / lastProduct)
            if (Math.abs(change) > 0.15) {
                var observed = ExposureMath.stops(luminance / lastLuminance)
                responseFailures = Math.abs(observed - change) > 0.75 ? responseFailures + 1 : 0
                if (responseFailures >= 3) {
                    responseInvalid = true
                    invalidateCalibration("unreliable_brightness")
                    status = "Auto paused: preview response unreliable"; return
                }
            }
        }
        lastProduct = product; lastLuminance = luminance
        meterEv = ExposureMath.stops(luminance / reference)
        meterValid = true
        sampleReason = "valid"
        status = Number(values.flicker) === 1 || Number(values.flicker) === 2
                ? "Estimated" : "Estimated · flicker unverified"
        if (!partial || !acknowledged || clockMs - lastCommandMs < 200) return
        var error = effectiveEv - meterEv
        filteredError = 0.5 * filteredError + 0.5 * error
        var target = product * Math.pow(2, error)
        var bounded = ExposureMath.solve(target, isoLock, timeLock, capabilities, Number(values.flicker))
        if (bounded.limit || Math.abs(error) < 0.08) limitStatus = bounded.limit
        residualEv = bounded.residual
        if (Math.abs(filteredError) < 0.1) return
        var next = ExposureMath.solve(product * Math.pow(2, ExposureMath.clamp(filteredError, -0.25, 0.25)),
                                      isoLock, timeLock, capabilities, Number(values.flicker))
        // Flicker-safe shutter periods can be more than 1/4 stop apart. Avoid getting
        // stuck at the old period, and switch only if it improves the target error.
        if (!timeLock && next.time === appliedTime && bounded.time !== appliedTime &&
                Math.abs(bounded.residual) + 0.1 < Math.abs(error)) next = bounded
        setPair(next.iso, next.time)
    }
    function diagnostics() {
        return { camera: cameraId, metering_reason: meterReason(),
                 ae_state: resultAeState, ae_mode: resultAeMode, result_ev_steps: resultEvSteps,
                 sample_age_ms: lastSampleMs > 0 ? Math.max(0, Date.now() - lastSampleMs) : null,
                 calibration_samples: stableSamples,
                 manual_supported: manualSupported, result_exposure_supported: capabilities.result_exposure_supported === true,
                 iso_mode: isoLock > 0 ? "manual" : "auto", shutter_mode: timeLock > 0 ? "manual" : "auto",
                 requested_ev: selectedEv, effective_ev: effectiveEv, compensation_steps: compensationSteps,
                 requested_iso: appliedIso, requested_shutter_ns: String(appliedTime),
                 actual_iso: actualIso, actual_shutter_ns: String(actualTime),
                 metering: meterValid && fresh ? "estimated" : "unavailable", meter_ev: meterEv,
                 limit: limitStatus, residual_ev: residualEv, acknowledged: acknowledged,
                 device_verified: false }
    }
}
