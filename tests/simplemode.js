// Run with node tests/simplemode.js. No Qt build required.
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const source = fs.readFileSync(path.join(__dirname, '../src/settings.qml'), 'utf8');
const simple = {};
vm.createContext(simple);
vm.runInContext(fs.readFileSync(path.join(__dirname, '../src/settings/SimpleMode.js'), 'utf8')
    .replace(/^\.pragma library$/m, ''), simple);
const defaults = {};
for (const match of source.matchAll(/"(\w+)": ("[^"]*"|\d+|true|false),/g)) {
    defaults[match[1]] = JSON.parse(match[2]);
}
let overrides = {};
const choices = {
    camera2CaptureFormatModel: ['jpeg', 'raw'], rawCaptureRawFormatModel: ['raw16', 'raw10'],
    camera2SizeModel: ['4096x3072', '3264x2448'], camera2SceneModel: ['manual', 'hdr'],
    camera2SpeedModel: ['balanced', 'quality'], camera2FocusModeModel: ['auto', 'manual'],
    camera2FocusDistanceModel: ['0', '0.25', '1'], rawCaptureFocusTimeoutModel: [1, 3, 5],
    rawCaptureExposureModel: ['0.5', '1.0', '2.0'], camera2IsoModel: [0, 100, 200],
    camera2ShutterModel: ['0', '10000000'], camera2NoiseReductionModel: [0, 1],
    camera2BracketModel: ['off', 'ev1'], rawRenderEngineModel: ['enhanced', 'internal', 'fastjpeg'],
    rawCaptureRotationModel: [0, 90, 180, 270]
};
const context = {
    SimpleMode: simple, settingsDefaults: defaults, deviceId: '3',
    globalSettings: {advancedMode: false}, _normalizingCamera2Settings: false,
    _lastAppliedAdvancedMode: false,
    camera2Capabilities: {camera2SimpleModeOverrides: () => overrides},
    camera2PreferredCaptureSize: () => '3264x2448', console
};
for (const name in choices) context[name] = () => choices[name];
for (const key in simple.schema) {
    const property = simple.schema[key][0];
    context.globalSettings['advanced' + property[0].toUpperCase() + property.slice(1)] = defaults[property];
}
context.globalSettings.advancedRawCaptureRotation = 90;
const savedProperties = ['rawCaptureRotation'].concat(Object.values(simple.schema).map(rule => rule[0]));
assert(source.includes('modeSettings[property + "Changed"].connect(function() { saveAdvancedSetting(property) })'));
const qualityHandler = source.match(/            onRawCaptureJpegQualityChanged: \{([\s\S]*?)\n            \}/);
assert(qualityHandler);
// Emulate the QML change handlers, including synchronous re-entry.
context.modeSettings = new Proxy({...defaults}, {set(target, key, value) {
    const changed = target[key] !== value;
    target[key] = value;
    if (changed && ['camera2CaptureFormat', 'rawCaptureRawFormat', 'rawCaptureFocusMode'].includes(key))
        context.normalizeCamera2Settings();
    if (changed && key === 'rawCaptureJpegQuality')
        vm.runInContext(qualityHandler[1], context);
    else if (changed && savedProperties.includes(key))
        context.saveAdvancedSetting(key);
    return true;
}});
vm.createContext(context);
vm.runInContext(source.match(/    function saveAdvancedSetting\(property\) \{[\s\S]*?\n    \}/)[0], context);
for (const name of ['applyAdvancedMode', 'applySimpleModeSettings', 'normalizeCamera2Settings', 'camera2ManualFocusDistanceModel']) {
    const match = source.match(new RegExp('    function ' + name + '\\(\\) \\{[\\s\\S]*?\\n    \\}'));
    assert(match, name);
    vm.runInContext(match[0], context);
}
context.applyAdvancedMode();
assert.strictEqual(context.modeSettings.rawCaptureScene, 'hdr');
assert.strictEqual(context.modeSettings.rawCaptureSize, '3264x2448');
overrides = {capture_format: 'jpeg', scene: 'manual', capture_size: '4096x3072',
    exposure_multiplier: 1, iso: 200, shutter_ns: 10000000, progressive_jpeg: true};
context.normalizeCamera2Settings();
assert.strictEqual(context.modeSettings.rawCaptureScene, 'manual');
assert.strictEqual(context.modeSettings.rawCaptureSize, '4096x3072');
assert.strictEqual(context.modeSettings.rawCaptureExposure, '1.0');
assert.strictEqual(context.modeSettings.rawCaptureIso, 0); // Simple always uses AE.
assert.strictEqual(context.modeSettings.rawCaptureShutterNs, '0');
assert.strictEqual(context.modeSettings.rawCaptureProgressiveJpeg, true);
// Wrong types, out-of-range and unsupported values fall back, not to previous overrides.
overrides = {scene: false, iso: '200', shutter_ns: '9999999999', jpeg_quality: 101,
    progressive_jpeg: 'false', exposure_multiplier: 0, capture_size: '1x1', unknown: true};
context.normalizeCamera2Settings();
assert.strictEqual(context.modeSettings.rawCaptureScene, 'hdr');
assert.strictEqual(context.modeSettings.rawCaptureIso, 0);
assert.strictEqual(context.modeSettings.rawCaptureShutterNs, '0');
assert.strictEqual(context.modeSettings.rawCaptureJpegQuality, 92);
assert.strictEqual(context.modeSettings.rawCaptureProgressiveJpeg, false);
assert.strictEqual(context.modeSettings.unknown, undefined);
// A second camera without overrides must not inherit the first camera's configuration.
overrides = {};
context.deviceId = '4';
choices.camera2SceneModel = ['manual'];
context.normalizeCamera2Settings();
assert.strictEqual(context.modeSettings.rawCaptureScene, 'manual');
assert.strictEqual(context.modeSettings.rawCaptureSize, '3264x2448');
// RAW10 rejects the incompatible renderer (falls back to the RAW10-capable default); format changes exercise the recursion guard.
overrides = {capture_format: 'raw', raw_format: 'raw10', render_engine: 'fastjpeg'};
context.normalizeCamera2Settings();
assert.strictEqual(context.modeSettings.camera2CaptureFormat, 'raw');
assert.strictEqual(context.modeSettings.rawCaptureRawFormat, 'raw10');
assert.strictEqual(context.modeSettings.rawRenderEngine, 'enhanced');
assert.strictEqual(context._normalizingCamera2Settings, false);
// Advanced settings survive Simple overrides and repeated Simple startup application.
context.globalSettings.advancedMode = true;
context.applyAdvancedMode();
context.modeSettings.rawCaptureAperture = 18;
context.modeSettings.rawCaptureFocusFailure = 'abort';
context.modeSettings.rawCaptureJpegQuality = 80;
context.modeSettings.rawCaptureScene = 'manual';
context.globalSettings.advancedMode = false;
context.applyAdvancedMode();
context.applyAdvancedMode();
assert.strictEqual(context.modeSettings.rawCaptureAperture, 0);
context.globalSettings.advancedMode = true;
context.applyAdvancedMode();
assert.strictEqual(context.modeSettings.rawCaptureAperture, 18);
assert.strictEqual(context.modeSettings.rawCaptureFocusFailure, 'abort');
assert.strictEqual(context.modeSettings.rawCaptureJpegQuality, 80);
assert.strictEqual(context.modeSettings.rawCaptureScene, 'manual');
// Closing directly in Advanced must preserve the latest quality, without a mode switch.
context.modeSettings.rawCaptureJpegQuality = 96;
assert.strictEqual(context.globalSettings.advancedRawCaptureJpegQuality, 96);
context._lastAppliedAdvancedMode = false; // Fresh startup before restoration.
context.modeSettings.rawCaptureJpegQuality = 92;
assert.strictEqual(context.globalSettings.advancedRawCaptureJpegQuality, 96);
context.applyAdvancedMode();
assert.strictEqual(context.modeSettings.rawCaptureJpegQuality, 96);
// Any Advanced edit (not only quality) survives a restart without a mode switch.
context.modeSettings.camera2CaptureFormat = 'raw';
context.modeSettings.rawCaptureAperture = 22;
assert.strictEqual(context.globalSettings.advancedCamera2CaptureFormat, 'raw');
context._lastAppliedAdvancedMode = false; // Fresh startup before restoration.
context.applyAdvancedMode();
assert.strictEqual(context.modeSettings.camera2CaptureFormat, 'raw');
assert.strictEqual(context.modeSettings.rawCaptureAperture, 22);
// Simple HAL overrides must not replace the saved Advanced quality.
overrides = {jpeg_quality: 75};
context.globalSettings.advancedMode = false;
context.applyAdvancedMode();
assert.strictEqual(context.modeSettings.rawCaptureJpegQuality, 75);
assert.strictEqual(context.globalSettings.advancedRawCaptureJpegQuality, 96);
context._lastAppliedAdvancedMode = false;
context.applyAdvancedMode();
context.globalSettings.advancedMode = true;
context.applyAdvancedMode();
assert.strictEqual(context.modeSettings.rawCaptureJpegQuality, 96);
// A refreshed scene model must replace an excluded Advanced selection.
context.modeSettings.rawCaptureScene = 'face-priority';
choices.camera2SceneModel = ['manual', 'auto'];
context.normalizeCamera2Settings();
assert.strictEqual(context.modeSettings.rawCaptureScene, 'manual');
// An excluded Simple override must also fall back to an available scene.
overrides = {scene: 'face-priority'};
context.globalSettings.advancedMode = false;
context.applyAdvancedMode();
assert.strictEqual(context.modeSettings.rawCaptureScene, 'manual');
// Manual distance excludes infinity and preserves a valid positive selection.
context.globalSettings.advancedMode = true;
context.applyAdvancedMode();
choices.camera2FocusModeModel = ['auto', 'manual', 'infinity'];
context.modeSettings.rawCaptureFocusDistance = '0';
context.modeSettings.rawCaptureFocusMode = 'manual';
assert.strictEqual(context.modeSettings.rawCaptureFocusDistance, '0.25');
context.modeSettings.rawCaptureFocusDistance = '1';
context.modeSettings.rawCaptureFocusMode = 'infinity';
context.modeSettings.rawCaptureFocusMode = 'manual';
assert.strictEqual(context.modeSettings.rawCaptureFocusDistance, '1');
// Legacy None selections normalize to an available mode.
context.modeSettings.rawCaptureFocusMode = 'none';
assert.strictEqual(context.modeSettings.rawCaptureFocusMode, 'auto');
const focusContext = {deviceId: '0', camera2Capabilities: {
    camera2FocusModeModel: () => ['auto', 'manual', 'infinity', 'none']
}};
vm.createContext(focusContext);
for (const name of ['camera2FocusModeModel', 'camera2SelectableFocusModeModel']) {
    vm.runInContext(source.match(new RegExp('    function ' + name + '\\(\\) \\{[\\s\\S]*?\\n    \\}'))[0], focusContext);
}
assert.deepStrictEqual(Array.from(focusContext.camera2SelectableFocusModeModel()), ['auto', 'manual', 'infinity']);
focusContext.camera2Capabilities.camera2FocusModeModel = () => ['none'];
assert.strictEqual(focusContext.camera2SelectableFocusModeModel().length, 0);
assert.deepStrictEqual(Array.from(focusContext.camera2FocusModeModel()), ['none']);
// Labels must distinguish actual Manual from unrecognized HAL names.
const labelFunction = source.match(/    function rawCaptureSceneLabel\(scene\) \{[\s\S]*?\n    \}/);
assert(labelFunction);
vm.runInContext(labelFunction[0], context);
assert.strictEqual(context.rawCaptureSceneLabel('manual'), 'Manual');
assert.strictEqual(context.rawCaptureSceneLabel('auto'), 'Auto');
assert.strictEqual(context.rawCaptureSceneLabel('face-priority'), 'face-priority');
assert.strictEqual(context.rawCaptureSceneLabel('unknown'), 'unknown');
console.log('Simple mode validation, camera switching, recursion, Advanced restoration and scene normalization/labels passed.');
