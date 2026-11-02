// Exercise the actual QML bracket selection functions without a Qt build.
const assert=require('assert'), fs=require('fs'), vm=require('vm'), path=require('path');
const read=p=>fs.readFileSync(path.join(__dirname,'..',p),'utf8');
const source=read('src/capture/CaptureView.qml');
const c={Settings:{mode:{camera2CaptureFormat:'raw',rawCaptureBracket:'off'}}};vm.createContext(c);
vm.runInContext(source.match(/    function _camera2BracketSteps\(\) \{[\s\S]*?\n    \}/)[0],c);
for(const [setting,expected] of [['off',[]],['ev1',[0,-2]],['ev2',[0,-4]],['ev3',[0,-6]]]) {
 c.Settings.mode.rawCaptureBracket=setting;
 assert.equal(JSON.stringify(c._camera2BracketSteps()),JSON.stringify(expected));
}
// A stale "burst" value from older versions is not a pair.
c.Settings.mode.rawCaptureBracket='burst';
assert.equal(c._camera2BracketSteps().length,0);
c.Settings.mode.rawCaptureBracket='ev3';
// A six-stop pair keeps the base exposure and requests 1/64 of its shutter time.
assert.equal(c._camera2BracketSteps()[0],0);
assert.equal(64000000*Math.pow(2,c._camera2BracketSteps()[1]),1000000);
for(const setting of ['off','ev1','ev2','ev3']) {
 c.Settings.mode.rawCaptureBracket=setting;
 c.Settings.mode.camera2CaptureFormat='jpeg';
 assert.equal(c._camera2BracketSteps().length,0);
}
const labels={};vm.createContext(labels);
vm.runInContext(read('src/settings.qml').match(/    function rawCaptureBracketLabel\(mode\) \{[\s\S]*?\n    \}/)[0],labels);
assert.equal(labels.rawCaptureBracketLabel('ev3'),'6-stop pair');
assert(!source.includes('combineBracketImages'));
assert(source.includes('if (captureView._warmRawReady) {'));
assert(source.includes('onRawBracketFrameReady:'));
const bridge=read('sfos-camera2-bridge/android/preview.c');
assert(!bridge.includes('raw_bracket_next_image'));
assert(bridge.includes('context->raw_bracket_result[i].timestamp_ns == timestamp'));
assert(bridge.includes('command.bracket_count >= 2'));
// Equal-exposure bursts: results fill the first matching slot still waiting.
assert(bridge.includes('raw_bracket_result_status[index],\n                                         memory_order_acquire) == 0'));
assert(!bridge.includes('context->raw_bracket_requests[index] == request'));
assert(bridge.includes('ACaptureRequest_getConstEntry(request, ACAMERA_SENSOR_EXPOSURE_TIME'));
assert(bridge.includes('capture->generation != atomic_load_explicit'));
assert(source.includes('_camera2BracketQueue.length > 0 || _camera2BracketMerging'));
assert(read('src/camera2preview.cpp').includes('if (metadataIndex < 0) return;'));
assert(read('src/camera2preview.cpp').includes('count != 2 || count != metadataPaths.count()'));
const extensions=read('src/declarativecameraextensions.cpp');
assert(!extensions.includes('combineBracketJpegs'));
assert(extensions.includes('model.append(QStringLiteral("ev3"))'));
// Non-build checks: the native helper now accepts -6 and retains range clamping.
const shutterHelper=extensions.split('QString DeclarativeCameraExtensions::camera2BracketShutterNs(')[1].split('QStringList DeclarativeCameraExtensions::camera2ShutterModel')[0];
assert(shutterHelper.includes('ev < -6'));
assert(shutterHelper.includes('qBound(range[0].toDouble(),'));
assert(shutterHelper.includes('time * std::pow(2.0, ev), double(m_camera2MaxShutterNs)'));
assert(source.includes('Shutter limit: reduced bracket separation'));
assert(extensions.includes('if (m_bracketCount >= 2) return stageRawBracketFrame();'));
// HDR is a global switch, not a bracket option; it routes through the RAW burst in any format.
assert(!extensions.includes('model.append(QStringLiteral("burst"))'));
assert(extensions.includes('bool DeclarativeCameraExtensions::camera2BurstSupported('));
assert(source.includes('Settings.mode.camera2CaptureFormat === "jpeg" && !captureView._camera2BurstPlan'));
assert(read('src/camera2preview.cpp').includes('QByteArrayLiteral("capture-raw-burst ")'));
// JPEG mode renders single shots from RAW16 too; the HAL's JPEG is only the fallback.
const settingsSource=read('src/settings.qml'), cameraSource=read('camera.qml');
assert(settingsSource.includes('function camera2JpegFromRaw(cameraId)'));
assert(source.includes('&& !(captureView._camera2JpegFromRaw && captureView._warmRawReady)'));
assert(source.includes('&& (_camera2HdrCapture || _camera2JpegFromRaw'));
assert(source.includes('captureView._camera2JpegFromRaw ? "enhanced" : Settings.mode.rawRenderEngine'));
assert(cameraSource.includes('&& !hdrCapture && !jpegFromRaw'));
assert(cameraSource.includes('&& (hdrCapture || jpegFromRaw'));
// Camera switching: flip + ultra-wide icons from Camera2 roles, preview from sensor orientation.
const overlaySource=read('src/settings/SettingsOverlay.qml');
assert(!overlaySource.includes('CameraLensSelector {'));
assert.equal(overlaySource.split('CameraSwitchButtons {').length - 1, 2);
assert(read('src/settings/CameraSwitchButtons.qml').includes('Settings.camera2CameraRoles()'));
assert(extensions.includes('QVariantMap DeclarativeCameraExtensions::camera2CameraRoles()'));
assert(extensions.includes('.value(QStringLiteral("sensor_orientation")).toInt(-1)'));
assert(bridge.includes('ACAMERA_LENS_FACING') || read('sfos-camera2-bridge/android/camera2_bridge.c').includes('ACAMERA_LENS_FACING'));
assert(cameraSource.includes('lensFacing === "front"'));
console.log('Bracket selection, JPEG exclusion and capture wiring checks passed.');

const parser=read('sfos-camera2-bridge/android/preview_commands.h');
assert(bridge.includes('return preview_parse_capture_command(line, command);'));
assert(!bridge.includes('sscanf(line, "capture-raw '));
assert(parser.includes('!strcmp(name, "capture-raw")'));
assert(parser.includes('!strcmp(name, "capture-raw-bracket")'));
assert(parser.includes('!strcmp(name, "capture-raw-burst")'));
assert(bridge.includes('"command-error"'));
assert(read('src/camera2preview.cpp').includes('frames received)'));

// Capture rotation: rear cameras keep the old table (sensor 90), front cameras
// subtract the device rotation from their own mounting.
{
  const r={Orientation:{Portrait:1,Landscape:2,PortraitInverted:4,LandscapeInverted:8},
           captureView:{orientation:1}, _camera2ResolvedCameraId:'0',
           facing:{'0':'back','1':'front','9':'unknown'}, sensor:{'0':90,'1':270,'9':90},
           Settings:{global:{frontFacingDeviceId:''}}};
  r.Settings.camera2LensFacing=id=>r.facing[id];
  r.Settings.camera2PreviewOrientation=(id,fallback)=>r.sensor[id]!==undefined?r.sensor[id]:fallback;
  vm.createContext(r);
  vm.runInContext(source.match(/    function _camera2DeviceRotation\(\) \{[\s\S]*?\n    \}/)[0],r);
  const O=r.Orientation;
  const rot=(id,o)=>{r._camera2ResolvedCameraId=id;r.captureView.orientation=o;return r._camera2DeviceRotation();};
  assert.deepEqual([O.Portrait,O.Landscape,O.PortraitInverted,O.LandscapeInverted].map(o=>rot('0',o)),[90,0,270,180]);
  assert.deepEqual([O.Portrait,O.Landscape,O.PortraitInverted,O.LandscapeInverted].map(o=>rot('1',o)),[270,0,90,180]);
  r.sensor['1']=90; // a front sensor mounted like a rear one
  assert.equal(rot('1',O.Portrait),90);
  assert.equal(rot('1',O.Landscape),180);
  assert.equal(rot('9',O.Portrait),90); // unknown facing behaves as before
  console.log('Capture rotation checks passed.');
}

// Cameras without RAW: HDR bursts stream YUV (I420) through the same burst path.
{
  const preview=read('sfos-camera2-bridge/android/preview.c'), qml=read('camera.qml');
  assert(preview.includes(': context->raw_yuv ? AIMAGE_FORMAT_YUV_420_888'));
  assert(preview.includes('context->raw_jpeg ? "JPEG" : context->raw_yuv ? "I420" : "RAW16"'));
  // A refused second YUV stream falls back to JPEG before dropping the stream.
  assert(preview.includes('failed (status %d); trying JPEG'));
  // The requested burst size is validated against the stream's own format
  // (device 2026-10-31: a YUV size checked against RAW16 dropped the stream).
  assert(preview.includes('context.raw_yuv ? AIMAGE_FORMAT_YUV_420_888 : AIMAGE_FORMAT_RAW16,\n                raw_width, raw_height)'));
  assert(preview.includes('raw_error=%s raw_size=%dx%d raw_format=%s'));
  assert(preview.includes('frame_context.noise_reduction = 0;'));
  assert(preview.includes('ACAMERA_EDGE_MODE, ACAMERA_EDGE_MODE_OFF'));
  assert(read('src/camera2preview.cpp').includes('QStringLiteral("--raw-format") << m_rawCaptureFormat'));
  assert(qml.includes('rawCaptureFormat: Settings.camera2BurstFormat(cameraId) === "yuv" ? "yuv" : "raw16"'));
  assert(read('src/rawpipeline.cpp').includes('return renderYuvImage(metadataPaths, settings, image, info);'));
  console.log('YUV burst wiring checks passed.');
}

// Bursts are merged in a background queue: the capture completes once the
// frames are staged, the photo is added when its merge finishes.
{
  const ext=read('src/declarativecameraextensions.cpp');
  assert(ext.includes('m_burstJobs.append(job);'));
  assert(ext.includes('job.directory = QSharedPointer<QTemporaryDir>(m_rawBracketDirectory.take());'));
  assert(ext.includes('emit burstRendered(job.target);'));
  assert(source.includes('camera._completeQueuedCamera2Capture()'));
  assert(source.includes('&& extensions.pendingRenders < 3'));
  assert(source.includes('onBurstRendered: {'));
  console.log('Burst queue wiring checks passed.');
}

// Android-style UI: instant thumbnail, tap brightness, swipes + mode tabs, motion.
{
  const overlay=read('src/settings/SettingsOverlay.qml'), capture=read('src/capture/CaptureOverlay.qml');
  const preview=read('src/camera2preview.cpp'), exposure=read('src/capture/ExposureController.qml');
  assert(preview.includes('QString Camera2Preview::saveThumbnail(int captureRotation)'));
  assert(source.includes('camera2Viewfinder.saveThumbnail(_camera2CaptureRotation())'));
  assert(overlay.includes('pendingSource: captureView ? captureView.pendingThumbnail : ""'));
  assert(exposure.includes('+ tapEv'));
  assert(capture.includes('captureView.setTapExposure('));
  assert(overlay.includes('overlay.flipCamera()') && overlay.includes('if (swiped) return'));
  assert(overlay.includes('CaptureModeTabs {'));
  assert(read('src/CameraPage.qml').includes('&& page.galleryActive'));
  assert(capture.includes('scale: pressed && containsMouse ? 0.88 : 1.0'));
  assert(preview.includes('Keep a blurred, dimmed last frame'));
  console.log('Android-style UI checks passed.');
}

// Video shares the floating photo layout (simple deck, tabs, zoom pill,
// fullscreen viewfinder) instead of the legacy Jolla controls.
{
  const overlay=read('src/settings/SettingsOverlay.qml');
  assert(overlay.includes('readonly property bool active: Settings.global.captureMode === "image" || video'));
  assert(overlay.includes('visible: opacity > 0.0 && !camera2BottomDeck.active\n'));
  assert(overlay.includes('qtCamera: camera2BottomDeck.video'));
  assert(!overlay.includes('onClicked: Settings.global.captureMode = "video"'));
  assert(read('camera.qml').includes('fillMode: Settings.global.viewfinderFill ? VideoOutput.PreserveAspectCrop'));
  assert(read('src/capture/CaptureOverlay.qml').includes('captureView.showVideoBrightness()'));
  console.log('Video layout checks passed.');
}

// Warm captures lock continuous AF first (AF trigger), release after, and
// record the per-frame focus state.
{
  const preview=read('sfos-camera2-bridge/android/preview.c');
  assert.equal(preview.split('preview_lock_focus(\n').length - 1, 2);
  assert(preview.includes('ACAMERA_CONTROL_AF_TRIGGER_START') && preview.includes('ACAMERA_CONTROL_AF_TRIGGER_CANCEL'));
  assert(preview.includes('\\"lens_focus_distance\\":%.6f'));
  console.log('Focus lock checks passed.');
}

// Night mode: a moon offered in dark scenes, a long-frame burst plan and the
// renderer's night lift; RAW files and the photo aspect in Normal mode too.
{
  const overlay=read('src/settings/SettingsOverlay.qml'), settings=read('src/settings.qml');
  assert(settings.includes('property bool nightMode: false'));
  assert(overlay.includes('(captureView.sceneDark || Settings.nightMode)') && overlay.includes('MoonIcon {'));
  assert(source.includes('&& (Settings.global.hdrBurst || Settings.nightMode)'));
  assert(source.includes('_camera2NightCapture, _camera2SceneInfo())') && source.includes('metadata.night_mode = _camera2NightCapture'));
  assert(source.includes('exposure >= 3.5e11') && source.includes('exposure >= 2e11'));
  assert(read('src/capture/CaptureOverlay.qml').includes('captureView.nightCaptureRunning'));
  assert(source.includes('onSceneDarkChanged: if (!sceneDark) Settings.nightMode = false'));
  assert(read('src/capture/MoonIcon.qml').includes('source: lit ? "moon-lit.svg" : "moon.svg"'));
  assert(read('src/declarativecameraextensions.cpp').includes('night ? 16 : 8, yuv, night, burstScene);'));
  assert(read('sfos-camera2-bridge/android/preview_commands.h').includes('#define PREVIEW_MAX_RAW_BRACKET 16'));
  assert(read('src/camera2preview.cpp').includes('count > 16 ||'));
  assert(read('src/rawpipeline.cpp').includes('options.night = m.value(QStringLiteral("night_mode")).toBool();'));
  // RAW files follow the settings in Normal mode; Pro's JPEG format keeps none.
  assert(source.includes('(Settings.global.advancedMode && Settings.mode.camera2CaptureFormat === "jpeg")'));
  assert(settings.includes('property bool saveRawCaptureFiles: false'));
  assert(!read('settings/SettingsPage.qml').includes('label: "RAW capture files"\n        visible: Settings.global.advancedMode'));
  assert(overlay.includes('case "rawSave": return rawSaveSettingComponent'));
  // 16:9 from the per-camera aspect ratio, cropped by the renderer and shown by the viewfinder.
  assert(settings.includes('aspectRatio === CameraConfigs.AspectRatio_16_9 ? "16:9" : "4:3"'));
  assert(source.includes('metadata.photo_aspect = Settings.photoAspect'));
  // Photo mode uses the photo aspect; video recorded from the viewfinder is 16:9.
  assert(read('camera.qml').includes('photoAspect: videoMode ? "16:9" : Settings.photoAspect'));
  assert(read('src/rawpipeline.cpp').includes('cropToAspect(image, aspect);'));
  console.log('Night mode, RAW and aspect checks passed.');
}

// Viewfinder: frames stay in sensor orientation; the GPU rotates, mirrors,
// sharpens and reuses one texture.
{
  const preview=read('src/camera2preview.cpp'), node=read('src/previewnode.cpp');
  assert(!preview.includes('frame = frame.transformed(QTransform().rotate(m_orientation));\n        }\n        if (m_mirror)'));
  assert(preview.includes('PreviewNode *node = static_cast<PreviewNode *>(oldNode);'));
  assert(preview.includes('m_histogramTimer.elapsed() >= 150'));
  assert(preview.includes('preview-stats camera='));
  assert(node.includes('glTexSubImage2D') && node.includes('new PreviewTexture(GL_RGB)'));
  // YUV transport: the bridge sends I420 (SF2Y), the shader converts it.
  assert(preview.includes('<< QStringLiteral("--preview-format") << QStringLiteral("yuv")'));
  assert(preview.includes('static const QByteArray yuvMagic("SF2Y", 4);'));
  assert(preview.includes('node->setYuvFrame(m_yuvFrame, m_yuvSize);'));
  assert(node.includes('y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u'));
  const bridge=read('sfos-camera2-bridge/android/preview.c');
  assert(bridge.includes("unsigned char header[16] = { 'S', 'F', '2', 'Y' };"));
  assert(bridge.includes('context.preview_yuv = (raw_format & SFOS_CAMERA2_PREVIEW_YUV_FRAMES) != 0;'));
  assert(read('sfos-camera2-bridge/sailfish/main.c').includes('raw_format | (preview_yuv ? SFOS_CAMERA2_PREVIEW_YUV_FRAMES : 0)'));
  assert(read('src/declarativecameraextensions.cpp').includes('m_camera2PreviewSizes, QSize(1440, 1080));'));
  assert(node.includes('case 90: return QPointF(v, 1.0 - u);'));
  assert(read('src/src.pro').includes('previewnode.cpp'));
  console.log('Viewfinder rendering checks passed.');
}

// Landscape: the top bar stops at the deck column, so the PHOTO | VIDEO tabs
// at the deck's top are never covered by HDR and the aspect chip.
{
  const overlay=read('src/settings/SettingsOverlay.qml');
  assert(overlay.includes('+ (overlay.isPortrait || !camera2BottomDeck.active ? 0 : camera2BottomDeck.width)'));
  console.log('Landscape top bar checks passed.');
}

// Keep every burst frame (diagnostics, training data) and the pipeline's
// vectorizing build flags.
{
  const ext=read('src/declarativecameraextensions.cpp');
  assert(read('src/declarativecameraextensions.h').includes('Q_PROPERTY(bool keepBurstFrames MEMBER m_keepBurstFrames'));
  assert(ext.includes('if (m_bracketBurst && m_keepBurstFrames) {'));
  assert(ext.includes('(yuvFrame ? QLatin1String(".yuv")'));
  assert(source.includes('keepBurstFrames: Settings.global.keepBurstFrames'));
  assert(read('src/settings.qml').includes('property bool keepBurstFrames: false'));
  assert(read('settings/SettingsPage.qml').includes('text: "Keep every burst frame"'));
  assert(read('dconf/00-procam.txt').includes('saveRawCaptureFiles=false'));
  assert(read('src/src.pro').includes('QMAKE_CXXFLAGS_RELEASE += -O3 -fno-math-errno'));
  console.log('Burst frames and build flag checks passed.');
}

// Super-resolved detail is swapped in only above the measured noise, and not
// next to clipped samples (real burst: SR had doubled the grain on a wall).
{
  const math=read('src/rawpipelinemath.cpp');
  assert(math.includes('static const float DetailCoring = 2.0f;'));
  assert(math.includes('std::max(0.0f, 1.0f - 4.0f * clippedShare)'));
  console.log('Super-resolution noise checks passed.');
}

// Scene-aware bursts: motion and faces from the viewfinder, faces from the
// camera (bridge) and the base frame picked on detailed areas.
{
  const preview=read('src/camera2preview.cpp'), bridge=read('sfos-camera2-bridge/android/preview.c');
  assert(bridge.includes('ACAMERA_STATISTICS_FACE_DETECT_MODE,') && bridge.includes('"faces=%d rects=%s\\n"'));
  assert(preview.includes('m_motionMeter.update(luma, m_motionClock.elapsed());'));
  assert(preview.includes('QVariantMap Camera2Preview::sceneInfo() const'));
  assert(source.includes('metadata.faces = camera2Viewfinder && camera2Viewfinder.faces'));
  assert(read('src/rawpipeline.cpp').includes('options.faces = facesFrom(m);'));
  assert(read('src/rawpipelinemath.cpp').includes('const std::size_t top = std::max<std::size_t>(1, energy.size() / 10);'));
  assert(read('.github/workflows/tests.yml').includes('previewanalysis'));
  console.log('Scene-aware burst checks passed.');
}

// Refined tone for every camera; a refused or stalled shutter is announced
// and logged instead of doing nothing.
{
  assert(read('src/rawpipeline.h').includes('bool toneRefinements = true;'));
  const pipeline=read('src/rawpipeline.cpp');
  assert(pipeline.split('options.saturation').length >= 3 && pipeline.includes('options.saturation = 1.05;'));
  assert(read('src/rawpipelinemath.cpp').includes('applyClarity(y, outWidth, outHeight, clarity'));
  assert(source.includes('extensions.logCaptureEvent("capture refused " + _captureState())'));
  assert(source.includes('extensions.logCaptureEvent("capture without exposure results "'));
  assert(read('src/declarativecameraextensions.h').includes('Q_INVOKABLE void logCaptureEvent(const QString &message);'));
  console.log('Tone default and shutter diagnostics checks passed.');
}

// Auto exposure that keeps adjusting (front camera) does not block the
// shutter for more than a second; the refusal log carries the AE state.
{
  assert(read('src/capture/ExposureController.qml').includes('(fresh && (autoReady || clockMs - lastCommandMs > 1000))'));
  assert(source.includes('" ae=" + exposureLogic.resultAeState'));
  console.log('Auto exposure readiness checks passed.');
}

// Selfies: faces kept 1.5 s after detection drops, a backlit centre counts
// as the face on the front camera, and the face sets YUV exposure.
{
  const preview=read('src/camera2preview.cpp'), math=read('src/rawpipelinemath.cpp');
  assert(preview.includes('m_faceSeen.elapsed() < 1500') && preview.includes('PreviewAnalysis::backlitSubject(luma, &subject)'));
  assert(read('camera.qml').includes('selfieCamera: frontFacing'));
  assert(math.includes('under = std::min(under, std::max(-2.0, 2.2 * std::log2(std::max(0.03, scene.faceLuma) / 0.3)));'));
  assert(math.includes('std::pow(ratio, ratio > 1.0f ? 0.5f : 0.8f)'));
  assert(math.includes('fitFace(faceLevel, &shortGain, &longGain, std::max(longGain, 8.0));'));
  console.log('Selfie face handling checks passed.');
}

// Front / ultra-wide white balance pulled 60% towards neutral on both axes,
// faces left out of the measurement.
{
  const math=read('src/rawpipelinemath.cpp');
  assert(math.includes('std::pow(1.0 / ratio, 0.6)') && math.includes('if (faceMask && inFace(x, row)) continue;'));
  assert(math.includes('channelGain[2] = gain(blue / green);'));
  console.log('Neutral white balance checks passed.');
}

// Tone ends on the smooth layer and noise-aware clarity (no extra grain).
{
  const math=read('src/rawpipelinemath.cpp');
  assert(math.includes('toneEnds(smoothLuma[i]) + (curved - smoothLuma[i])'));
  assert(math.includes('const float floor = deviation.empty() ? 0.0f : 1.5f * 1.4826f'));
  console.log('Tone noise checks passed.');
}
