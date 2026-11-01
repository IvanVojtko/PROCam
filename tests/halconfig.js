// Source regression checks only: no compiled Qt code or device behaviour is exercised.
const assert = require('assert'), fs = require('fs'), path = require('path');
const read = name => fs.readFileSync(path.join(__dirname, '..', name), 'utf8');
const settings = read('settings/SettingsPage.qml');
for (const removed of ['Export compatibility report', 'Generate HAL config', 'DBusInterface',
 'requestHalConfig', 'exportCamera2CompatibilityReport', 'camera2DeviceConfigDir',
 'ensureCamera2GeneratedHalConfig']) assert(!settings.includes(removed), removed);
assert(settings.includes('camera2CompatibilitySummary'));
const page = read('pages/MainCameraPage.qml');
assert(!page.includes('function generateHalConfig'));
assert(!page.includes('function deviceConfigDirectory'));
assert(page.includes('signal showViewfinder'));
assert(page.includes('signal showFrontViewfinder'));
const app = read('camera.qml');
assert.equal((app.match(/ensureCamera2GeneratedHalConfig/g)||[]).length, 1);
assert(app.includes('Component.onCompleted: Settings.camera2Capabilities.ensureCamera2GeneratedHalConfig'));
const cpp = read('src/declarativecameraextensions.cpp');
const ensure = cpp.split('QString DeclarativeCameraExtensions::ensureCamera2GeneratedHalConfig(')[1]
 .split('bool DeclarativeCameraExtensions::loadCamera2Capabilities')[0];
const exists = 'QFileInfo::exists(path) || QFileInfo(path).isSymLink()';
assert.equal(ensure.split(exists).length-1, 2);
assert(ensure.indexOf(exists)<ensure.indexOf('QDir().mkpath'));
assert(ensure.indexOf('lock.tryLock(0)')<ensure.lastIndexOf(exists));
assert(ensure.lastIndexOf(exists)<ensure.indexOf('QProcess probe'));
assert(ensure.indexOf('m_camera2ProbeJson')<ensure.indexOf('probe.start'));
assert(ensure.includes('if (!validProbe(document))'));
assert(ensure.includes('camera.value(QStringLiteral("status")).toString() == QLatin1String("ok")'));
assert(ensure.includes('appendRawCaptureLog(message)'));
assert(!ensure.includes('exit('));
assert(!ensure.includes('procamOverrideHalProfilePath'));
const writer = cpp.split('static bool writeGeneratedHalProfile(')[1].split('static void appendDeviceProfileObject')[0];
assert(writer.includes('QSaveFile file(path)'));
assert(writer.includes('file.write(data) != data.size() || !file.commit()'));
assert(!cpp.includes('backupGeneratedHalProfile'));
assert(!cpp.includes('.local/share/procam'));
assert(cpp.includes('QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)'));
console.log('HAL startup-only routing, existence checks, locking, cached probe and atomic-save source checks passed.');
