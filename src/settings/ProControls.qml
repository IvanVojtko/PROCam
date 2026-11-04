// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.6
import Sailfish.Silica 1.0
import com.procam.camera 1.0
import "../capture"

// Pro mode's manual controls next to the shutter deck: one chip per value
// (EV, shutter, ISO, focus), lit while set by hand. Tapping a chip opens its
// dial between the chips and the deck; tapping it again, or taking a photo,
// closes it. Portrait: chips in a row above the dial. Landscape: chips in a
// column with the dial to their left.
Item {
    id: root

    property var exposureControl
    property var camera2Viewfinder
    property bool captureBusy
    property bool vertical
    property var evChoices: [0]
    // "", "ev", "shutter", "iso" or "focus".
    property string openDial

    readonly property real spacing: Theme.paddingMedium
    readonly property real chipHeight: Theme.itemSizeExtraSmall
    readonly property real chipColumnWidth: Theme.itemSizeExtraLarge
    readonly property real dialThickness: vertical ? Theme.itemSizeExtraLarge : Theme.itemSizeMedium
    readonly property bool dialShown: openDial.length > 0
    readonly property var focusModel: focusValues()
    readonly property var keys: focusModel.length > 1 ? ["ev", "shutter", "iso", "focus"]
                                                      : ["ev", "shutter", "iso"]
    readonly property bool exposureAdjustable: !!exposureControl && exposureControl.active
                                               && !exposureControl.busy
    readonly property bool evAdjustable: !!exposureControl && !captureBusy
                                         && !exposureControl.fullyManual
                                         && (exposureControl.partial
                                             || exposureControl.capabilities.compensation_step_ev > 0)

    implicitWidth: vertical ? chipColumnWidth + (dialShown ? dialThickness + spacing : 0) : 0
    implicitHeight: vertical ? 0 : chipHeight + (dialShown ? dialThickness + spacing : 0)
    Behavior on implicitWidth { NumberAnimation { duration: 150; easing.type: Easing.InOutQuad } }
    Behavior on implicitHeight { NumberAnimation { duration: 150; easing.type: Easing.InOutQuad } }

    onCaptureBusyChanged: if (captureBusy) openDial = ""
    onVisibleChanged: if (!visible) openDial = ""

    // Twice the EV, as Settings.global.exposureCompensation stores it.
    function evText(twiceEv) {
        var ev = Math.round(Number(twiceEv) * 5) / 10
        return (ev > 0 ? "+" + ev : ev < 0 ? "−" + (-ev) : "±0") + " EV"
    }

    // Focus modes, then manual distances from near to far, then infinity.
    function focusValues() {
        var modes = Settings.camera2SelectableFocusModeModel()
        var values = modes.filter(function(mode) {
            return mode !== "manual" && mode !== "infinity"
        })
        if (modes.indexOf("manual") >= 0) {
            var distances = Settings.camera2ManualFocusDistanceModel().slice()
            distances.sort(function(a, b) { return Number(b) - Number(a) })
            values = values.concat(distances)
        }
        if (modes.indexOf("infinity") >= 0) values.push("infinity")
        return values
    }

    function focusValue() {
        return Settings.mode.rawCaptureFocusMode === "manual"
                ? Settings.mode.rawCaptureFocusDistance : Settings.mode.rawCaptureFocusMode
    }

    function focusLabel(value) {
        switch (value) {
        case "auto": return "AF"
        case "continuous": return "AF-C"
        case "infinity": return "∞"
        default: return Settings.rawCaptureFocusDistanceLabel(value)
        }
    }

    function selectFocus(value) {
        if (focusModel.indexOf(value) < 0) return
        if (value === "auto" || value === "continuous" || value === "infinity") {
            Settings.mode.rawCaptureFocusMode = value
        } else {
            Settings.mode.rawCaptureFocusDistance = value
            Settings.mode.rawCaptureFocusMode = "manual"
        }
    }

    function liveFresh() {
        return !!exposureControl && exposureControl.fresh
    }

    function chipText(key) {
        switch (key) {
        case "ev":
            if (exposureControl && exposureControl.fullyManual) {
                return Settings.global.estimatedExposureMetering
                        && exposureControl.meterValid && exposureControl.fresh
                        ? "≈ " + evText(exposureControl.meterEv * 2) : "EV —"
            }
            return evText(exposureControl ? exposureControl.effectiveEv * 2
                                          : Settings.global.exposureCompensation)
        case "shutter":
            if (Settings.mode.rawCaptureShutterNs !== "0")
                return Settings.rawCaptureShutterLabel(Settings.mode.rawCaptureShutterNs)
            return liveFresh() ? Settings.rawCaptureShutterLabel(String(exposureControl.actualTime))
                               : "Auto"
        case "iso":
            return "ISO " + (Settings.mode.rawCaptureIso > 0 ? Settings.mode.rawCaptureIso
                                                            : liveFresh() ? exposureControl.actualIso
                                                                          : "Auto")
        default:
            return focusLabel(focusValue())
        }
    }

    function chipManual(key) {
        switch (key) {
        case "ev": return Settings.global.exposureCompensation !== 0
        case "shutter": return Settings.mode.rawCaptureShutterNs !== "0"
        case "iso": return Settings.mode.rawCaptureIso > 0
        default: return Settings.mode.rawCaptureFocusMode === "manual"
                        || Settings.mode.rawCaptureFocusMode === "infinity"
        }
    }

    function chipEnabled(key) {
        switch (key) {
        case "ev": return evAdjustable
        case "shutter":
        case "iso": return exposureAdjustable
        default: return !captureBusy
        }
    }

    Grid {
        id: chips

        x: root.vertical ? root.width - width : 0
        y: root.vertical ? Math.max(0, (root.height - height) / 2) : 0
        width: root.vertical ? root.chipColumnWidth : root.width
        columns: root.vertical ? 1 : root.keys.length
        spacing: root.spacing

        Repeater {
            model: root.keys

            FloatingChip {
                readonly property bool open: root.openDial === modelData

                width: root.vertical ? chips.width
                                     : (chips.width - (root.keys.length - 1) * chips.spacing) / root.keys.length
                height: root.chipHeight
                text: root.chipText(modelData)
                checked: root.chipManual(modelData)
                enabled: root.chipEnabled(modelData) || open
                onClicked: root.openDial = open ? "" : modelData

                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: "transparent"
                    border.color: Theme.highlightColor
                    border.width: Math.max(2, Math.round(Theme.pixelRatio * 2))
                    visible: parent.open
                }
            }
        }
    }

    Item {
        id: dial

        x: 0
        y: root.vertical ? 0 : root.chipHeight + root.spacing
        width: root.vertical ? root.dialThickness : root.width
        height: root.vertical ? root.height : root.dialThickness
        opacity: root.dialShown ? 1.0 : 0.0
        visible: opacity > 0.0
        Behavior on opacity { FadeAnimation { duration: 150 } }

        // Remembers the last dial while it fades out.
        property string shownDial
        Connections {
            target: root
            onOpenDialChanged: if (root.openDial.length > 0) dial.shownDial = root.openDial
        }

        Rectangle {
            anchors.fill: parent
            radius: Theme.paddingSmall
            color: Theme.rgba("black", 0.4)
        }

        // Focus peaking next to the focus dial, zebras next to exposure.
        PreviewAidToggle {
            id: aidToggle

            x: 0
            y: 0
            width: root.vertical ? parent.width : Theme.itemSizeLarge
            height: root.vertical ? Theme.itemSizeExtraSmall : parent.height
            enabled: !!root.camera2Viewfinder && root.camera2Viewfinder.running
            text: dial.shownDial === "focus" ? "PEAK" : "ZEBRA"
            checked: dial.shownDial === "focus" ? Settings.global.focusPeaking
                                                : Settings.global.exposureZebras
            onClicked: {
                if (dial.shownDial === "focus")
                    Settings.global.focusPeaking = !Settings.global.focusPeaking
                else
                    Settings.global.exposureZebras = !Settings.global.exposureZebras
            }
        }

        Loader {
            x: root.vertical ? 0 : aidToggle.width
            y: root.vertical ? aidToggle.height : 0
            width: parent.width - (root.vertical ? 0 : aidToggle.width)
            height: parent.height - (root.vertical ? aidToggle.height : 0)
            sourceComponent: dial.shownDial === "ev" ? evDial
                           : dial.shownDial === "shutter" ? shutterDial
                           : dial.shownDial === "iso" ? isoDial
                           : dial.shownDial === "focus" ? focusDial : null
        }

        // The selected value: a mark on the dial's centre line.
        Rectangle {
            x: root.vertical ? 0 : aidToggle.width + (parent.width - aidToggle.width - width) / 2
            y: root.vertical ? aidToggle.height + (parent.height - aidToggle.height - height) / 2 : 0
            width: root.vertical ? Theme.paddingSmall : Math.max(2, Math.round(Theme.pixelRatio * 2))
            height: root.vertical ? Math.max(2, Math.round(Theme.pixelRatio * 2)) : Theme.paddingSmall
            color: Theme.highlightColor
        }
    }

    Component {
        id: evDial

        ValueCarousel {
            orientation: root.vertical ? ListView.Vertical : ListView.Horizontal
            visibleItems: 5
            tapered: true
            enabled: root.evAdjustable
            settings: Settings.global
            settingProperty: "exposureCompensation"
            writeThrough: false
            onValueSelected: Settings.global.exposureCompensation = value
            currentValue: Settings.global.exposureCompensation
            displayValue: root.exposureControl ? root.exposureControl.effectiveEv * 2 : currentValue
            model: root.evChoices
            valueLabel: root.evText
        }
    }

    Component {
        id: shutterDial

        ValueCarousel {
            orientation: root.vertical ? ListView.Vertical : ListView.Horizontal
            visibleItems: 5
            tapered: true
            wrap: true
            enabled: root.exposureAdjustable
            settings: Settings.mode
            settingProperty: "rawCaptureShutterNs"
            writeThrough: false
            onValueSelected: { if (!root.exposureControl.setLock("shutter", value)) rejectSelection() }
            currentValue: Settings.mode.rawCaptureShutterNs
            model: Settings.camera2ShutterModel()
            valueLabel: function(value) {
                return value === "0" ? "Auto" : Settings.rawCaptureShutterLabel(value)
            }
        }
    }

    Component {
        id: isoDial

        ValueCarousel {
            orientation: root.vertical ? ListView.Vertical : ListView.Horizontal
            visibleItems: 5
            tapered: true
            wrap: true
            enabled: root.exposureAdjustable
            settings: Settings.mode
            settingProperty: "rawCaptureIso"
            writeThrough: false
            onValueSelected: { if (!root.exposureControl.setLock("iso", value)) rejectSelection() }
            currentValue: Settings.mode.rawCaptureIso
            model: Settings.camera2IsoModel()
            valueLabel: function(value) { return value > 0 ? value : "Auto" }
        }
    }

    Component {
        id: focusDial

        ValueCarousel {
            orientation: root.vertical ? ListView.Vertical : ListView.Horizontal
            visibleItems: 5
            tapered: true
            enabled: !root.captureBusy
            settings: Settings.mode
            settingProperty: "rawCaptureFocusMode"
            writeThrough: false
            onValueSelected: root.selectFocus(value)
            currentValue: root.focusValue()
            model: root.focusModel
            valueLabel: root.focusLabel
        }
    }
}
