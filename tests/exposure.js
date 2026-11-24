// Source-level tests: node tests/exposure.js (no Qt/Android build).
const assert = require('assert'), fs = require('fs'), vm = require('vm'), path = require('path');
const base = path.join(__dirname, '../src/capture/');
const math = {};
vm.createContext(math);
vm.runInContext(fs.readFileSync(base+'ExposureMath.js','utf8').replace(/^\.pragma library$/m,''),math);
const caps = {manual_supported:true, result_exposure_supported:true, iso_range:[50,3200],
 shutter_ns_range:[100000,1000000000], max_frame_duration_ns:1100000000,
 compensation_step_ev:1/3, compensation_range:[-6,6]};
const close = (a,b,t=0.001) => assert(Math.abs(a-b)<t,`${a} != ${b}`);
close(math.compensation(1,caps).ev,1); assert.equal(math.compensation(1,caps).steps,3);
close(math.compensation(20,caps).ev,2); assert.equal(math.compensation(1,{}).ev,0);
const p=100*10000000;
assert.equal(math.solve(p*2,0,10000000,caps,0).iso,200);
assert.equal(math.solve(p/2,0,10000000,caps,0).iso,50);
assert.equal(math.solve(p*2,100,0,caps,0).time,20000000);
assert.equal(math.solve(p/2,100,0,caps,0).time,5000000);
close((1/30)*100,(1/60)*200);
assert.equal(math.solve(p*100,0,10000000,caps,0).limit,'ISO maximum');
assert.equal(math.solve(p*1000,100,0,caps,0).limit,'Shutter maximum');
assert.equal(math.solve(p*2,0,10000000,caps,1).time,10000000); // Never quantize a lock.
assert.equal(math.solve(25000000*100,100,0,caps,1).time,30000000);
assert(!math.validLimits({}));
assert(!math.samePair(100,10000000,200,10000000));
assert(math.samePair(100,10000000,100,10000001));

// Execute actual QML controller methods with reactive lock getters.
const qml=fs.readFileSync(base+'ExposureController.qml','utf8');
let now=1000;
const logs=[];
const c={qsTrId:id=>id,ExposureMath:math,Settings:{global:{advancedMode:true,estimatedExposureMetering:true,exposureCompensation:0},
 mode:{rawCaptureIso:0,rawCaptureShutterNs:'0',rawCaptureScene:'manual'}},
 console:{log:line=>logs.push(line)},sampleReason:"waiting_calibration",lastLogState:"",loggedCamera:"",
 noticeTimer:{restart:()=>{}},lastRejection:"",notice:"",
 cameraId:"0",capabilities:caps,active:true,busy:false,changing:false,
 appliedIso:0,appliedTime:0,actualIso:0,actualTime:0,reference:0,meterEv:0,meterValid:false,
 acknowledged:false,status:'',limitStatus:'',residualEv:0,lastTimestamp:0,lastSampleMs:0,
 lastCommandMs:0,lastCommandTimestamp:0,clockMs:now,filteredError:0,stableSamples:0,
 lastLuminance:0,lastProduct:0,responseFailures:0,responseInvalid:false,Date:{now:()=>now}};
const getters={isoLock:()=>c.Settings.global.advancedMode?c.Settings.mode.rawCaptureIso:0,
 timeLock:()=>c.Settings.global.advancedMode?Number(c.Settings.mode.rawCaptureShutterNs):0,
 fullyManual:()=>c.isoLock>0&&c.timeLock>0,partial:()=>(c.isoLock>0)!==(c.timeLock>0),
 manualSupported:()=>c.capabilities.manual_supported===true&&c.capabilities.result_exposure_supported===true&&math.validLimits(c.capabilities),
 estimatedEnabled:()=>c.Settings.global.estimatedExposureMetering&&c.capabilities.result_exposure_supported===true,
 selectedEv:()=>c.Settings.global.exposureCompensation/2,
 hardwareEv:()=>math.compensation(c.selectedEv,c.capabilities),
 effectiveEv:()=>c.fullyManual?0:c.partial?c.selectedEv:c.hardwareEv.ev,
 compensationSteps:()=>c.isoLock>0||c.timeLock>0?0:c.hardwareEv.steps,
 fresh:()=>c.lastSampleMs>0&&c.clockMs-c.lastSampleMs<Math.max(1500,c.appliedTime/1000000+1000),
 meteringReady:()=>c.estimatedEnabled&&c.reference>0&&c.fresh&&!c.responseInvalid&&c.sampleReason==='valid',
 canLock:()=>c.active&&!c.busy&&c.manualSupported&&(c.fullyManual||(c.fresh&&c.actualIso>0&&c.actualTime>0))};
for(const key in getters)Object.defineProperty(c,key,{get:getters[key]});
let commands=[];
c.preview={setExposurePair:(iso,time,ev)=>commands.push({iso,time,ev})};
c.Settings.mode=new Proxy(c.Settings.mode,{set(target,key,value){target[key]=value;
 if(!c.changing&&['rawCaptureIso','rawCaptureShutterNs'].includes(key))c.reconcile();return true;}});
vm.createContext(c);
for(const f of qml.matchAll(/    function \w+\([^)]*\) \{[\s\S]*?\n    \}/g))vm.runInContext(f[0],c);
function sample(iso=100,time=10000000,luminance=0.2,other={}){
 now+=250;
 c.sample(Object.assign({camera:"0",timestamp:now*1000000,iso,shutter:time,ae:2,ae_mode:c.appliedIso>0?0:1,ev_steps:0,flicker:0,luminance,clipped:0},other));
}
c.reset();sample();sample();sample();assert(c.reference>0);assert(c.canLock);
c.setLock('shutter',10000000);assert(c.partial);assert.equal(c.isoLock,0);assert.equal(c.appliedIso,100);
c.Settings.global.exposureCompensation=2;
for(let i=0;i<30;i++)sample(c.appliedIso,c.appliedTime,0.2*c.appliedIso/100);
assert.equal(c.timeLock,10000000);close(math.stops(c.appliedIso / 200), 0, 0.11);
c.setLock('iso',200);assert(c.fullyManual);assert.equal(c.effectiveEv,0);assert.equal(c.selectedEv,1);
sample(200,10000000,0.4);close(c.meterEv,1);
c.setLock('iso',400);assert.equal(c.timeLock,10000000);assert.equal(c.appliedIso,400);
c.setLock('iso',0);assert(c.partial);assert.equal(c.effectiveEv,1);
const before=c.appliedIso;sample(c.appliedIso,c.appliedTime,0,{clipped:1});assert(!c.meterValid);assert.equal(c.appliedIso,before);
const count=commands.length;c.busy=true;sample();assert.equal(commands.length,count);c.busy=false;
c.Settings.global.estimatedExposureMetering=false;c.freezeManual();assert(c.fullyManual);
c.setLock('iso',0);assert(!c.partial);assert.equal(c.appliedIso,0);assert.equal(c.appliedTime,0);
c.reset();assert.equal(c.reference,0);assert.equal(c.Settings.mode.rawCaptureIso,0);
sample();c.setLock("shutter",20000000);assert(c.fullyManual);assert.equal(c.appliedIso,100);
assert.equal(c.appliedTime,20000000);c.setLock("iso",200);assert.equal(c.appliedTime,20000000);
assert.equal(c.meterReason(),"disabled");c.setLock("shutter",0);assert.equal(c.appliedIso,0);
c.capabilities={};c.reset();assert.equal(c.setLock("iso",100),false);assert.equal(c.appliedIso,0);assert(!c.canLock);


// Applied results must acknowledge both locks; stale/wrong-camera samples are ignored.
c.capabilities=caps;c.Settings.global.estimatedExposureMetering=true;c.Settings.global.exposureCompensation=0;
c.reset();sample();sample();sample();c.setLock('iso',100);
let frozenTime=c.appliedTime;
sample(200,frozenTime,0.2);assert(!c.acknowledged);assert.equal(c.appliedTime,frozenTime);
const stamp=c.lastTimestamp;sample(100,frozenTime,0.2,{camera:'different'});assert.equal(c.lastTimestamp,stamp);
// Fixed-ISO convergence under flicker quantization must not stick at the old period.
sample(100,frozenTime,0.2);
c.Settings.global.exposureCompensation=2;
for(let i=0;i<40;i++)sample(c.appliedIso,c.appliedTime,0.2*c.appliedTime/10000000,{flicker:1});
assert.equal(c.appliedTime,20000000);assert.equal(c.appliedIso,100);
// A small sensor limit must be visible without changing the locked shutter.
c.Settings.global.exposureCompensation=0;c.reset();c.capabilities=Object.assign({},caps,{iso_range:[50,120]});sample();sample();sample();
c.setLock('shutter',10000000);c.Settings.global.exposureCompensation=4;
for(let i=0;i<15;i++)sample(c.appliedIso,c.appliedTime,0.2*c.appliedIso/100);
assert.equal(c.appliedIso,120);assert.equal(c.timeLock,10000000);assert.equal(c.limitStatus,'procam-exposure-limit-iso-maximum');
assert(c.residualEv>1);

// Diagnostic transitions do not repeat for ordinary frames or changing actual values.
c.logState();const logCount=logs.length;
for(let i=0;i<10;i++){sample(c.appliedIso,c.appliedTime,0.2*c.appliedIso/100);c.logState();}
assert.equal(logs.length,logCount);
assert.equal(c.diagnostics().metering_reason,'valid');
c.clockMs+=5000;assert.equal(c.meterReason(),'stale_results');c.logState();
assert.equal(logs.length,logCount+1);c.logState();assert.equal(logs.length,logCount+1);
c.reset();assert.equal(c.meterReason(),'missing_results');
assert.equal(c.setLock('iso',100),false); // No invented companion value.
c.Settings.global.estimatedExposureMetering=false;
assert.equal(c.setLock('shutter',10000000),false);
sample();assert(c.setLock('iso',100));assert(c.fullyManual);
assert(logs.some(line=>line.includes('fallback:manual_from_actual')));
assert(logs.some(line=>line.includes('calibration_acquired')));
c.Settings.global.estimatedExposureMetering=true;
sample(c.appliedIso,c.appliedTime);c.invalidateCalibration('test');assert.equal(c.meterReason(),'waiting_calibration');
c.capabilities={};assert.equal(c.meterReason(),'unsupported_results');

// Readiness and calibration share all three stable AE states, including flash-required (4).
c.capabilities=caps;c.Settings.global.exposureCompensation=0;
for(const state of [2,3,4]) {
 c.reset();sample(100,10000000,0.2,{ae:state});assert(c.autoReady);assert.equal(c.reference,0);
 sample(100,10000000,0.2,{ae:state});assert.equal(c.reference,0);
 sample(100,10000000,0.2,{ae:state});assert(c.meteringReady);
 assert.equal(c.diagnostics().ae_state,state);assert.equal(c.diagnostics().calibration_samples,3);
 assert(c.setLock('shutter',10000000));assert(c.partial);
}
for(const invalid of [{ae:1},{ae:0},{ae:5},{ae_mode:0},{ev_steps:1},{clipped:1},{luminance:0}]) {
 c.reset();sample();sample();sample(100,10000000,0.2,invalid);
 assert.equal(c.reference,0);assert.equal(c.stableSamples,0);
}
// Fresh actual values permit full manual even with estimation enabled but uncalibrated.
for(const wheel of ['iso','shutter']) {
 c.reset();sample(150,12000000,0.2,{ae:1});
 assert(c.setLock(wheel,wheel==='iso'?200:20000000));assert(c.fullyManual);
 assert.equal(c.isoLock,wheel==='iso'?200:150);
 assert.equal(c.timeLock,wheel==='shutter'?20000000:12000000);
 sample(c.appliedIso,c.appliedTime);assert(c.fullyManual); // No automatic mode promotion.
 assert(c.setLock(wheel,0));assert.equal(c.isoLock,0);assert.equal(c.timeLock,0);
}
// A lost estimator preserves the existing manual lock when changing either wheel.
c.reset();sample();sample();sample();c.setLock('shutter',10000000);
sample(c.appliedIso,c.appliedTime);c.invalidateCalibration('test');
assert(c.setLock('iso',200));assert.equal(c.timeLock,10000000);assert(c.fullyManual);
// Out-of-range companion values must never alter settings or submit a request.
c.reset();sample(11737,49992000,0.2,{ae:1});
let commandCount=commands.length;
assert(!c.setLock('shutter',20000000));assert.equal(c.isoLock,0);assert.equal(commands.length,commandCount);
// Repeated identical rejections are logged once; a different selection is reported.
let rejectionCount=logs.length;
for(let i=0;i<20;i++)assert(!c.setLock('shutter',20000000));
assert.equal(logs.length,rejectionCount);
assert(!c.setLock('shutter',30000000));assert.equal(logs.length,rejectionCount+1);
c.clockMs+=5000;assert(!c.setLock('iso',200));assert.equal(c.isoLock,0);
c.reset();assert.equal(c.diagnostics().ae_state,-1);assert.equal(c.diagnostics().sample_age_ms,null);

// Exercise the actual carousel rejection method with synchronous movement-ended callbacks.
const carouselSource=fs.readFileSync(path.join(__dirname,'../src/settings/ValueCarousel.qml'),'utf8');
let commits=0,restores=0;
const carousel={_userMoving:true,_syncing:false,
 list:{cancelFlick:()=>{if(carousel._userMoving&&!carousel._syncing)commits++;}},
 syncFromValue:()=>{assert(!carousel._userMoving);assert(carousel._syncing);restores++;}};
vm.createContext(carousel);
vm.runInContext(carouselSource.match(/    function rejectSelection\(\) \{[\s\S]*?\n    \}/)[0],carousel);
carousel.rejectSelection();assert.equal(commits,0);assert.equal(restores,1);assert(!carousel._userMoving);

// Meter presentation and carousel access remain independent of calibration.
const overlay=fs.readFileSync(path.join(__dirname,'../src/settings/SettingsOverlay.qml'),'utf8');
assert(!overlay.includes('text: "Full manual"'));
assert(!overlay.includes('text: "Auto"'));
assert(!overlay.includes('"Meter unavailable"'));
assert(!overlay.includes('overlay.exposureControl.canLock'));

// Protocol and capture invariants that can be checked without compiling.
const read=name=>fs.readFileSync(path.join(__dirname,'..',name),'utf8');
assert(read('sfos-camera2-bridge/sailfish/main.c').match(/--ev-steps/g).length>=2);
assert(read('src/declarativecameraextensions.cpp').match(/arguments << QStringLiteral\("--ev-steps"\)/g).length===2);
assert(read('sfos-camera2-bridge/android/preview.c').includes('AImage_getTimestamp(image, &timestamp)'));
assert(read('sfos-camera2-bridge/android/preview.c').includes('ACAMERA_SENSOR_TIMESTAMP'));
assert(!read('sfos-camera2-bridge/android/camera2_common.c').includes('sensitivity = 100;'));
assert(!read('sfos-camera2-bridge/android/camera2_common.c').includes('exposure_time_ns = 16666667;'));
console.log('Exposure arithmetic, controller transitions, EV suspension, limits, loss of metering and protocol checks passed.');
