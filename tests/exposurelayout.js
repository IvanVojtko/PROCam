// Source/geometry checks without a Qt build; device rendering still needs validation.
const assert=require('assert'),fs=require('fs'),vm=require('vm'),path=require('path');
const read=p=>fs.readFileSync(path.join(__dirname,'..',p),'utf8');
const qml=read('src/settings/ValueCarousel.qml');
let positions=[],commits=0,scheduled=0;
const c={model:['0','10000000','20000000'],currentValue:'10000000',wrap:true,baseIndex:75,
 selectionAtTop:false,_userMoving:true,_syncing:false,writeThrough:false,
 syncTimer:{restart:()=>scheduled++},ListView:{Beginning:0,Center:1},
 list:{vertical:true,count:153,forceLayout:()=>{},positionViewAtIndex:(i,p)=>positions.push([i,p]),
 cancelFlick:()=>{if(c._userMoving&&!c._syncing)commits++;}}};
c.root=c;vm.createContext(c);
for(const name of ['syncLayout','syncFromValue','indexForValue','valuesEqual','visualIndexForModelIndex']) {
 vm.runInContext(qml.match(new RegExp('    function '+name+'\\([^)]*\\) \\{[\\s\\S]*?\\n    \\}'))[0],c);
}
for(const enabled of [false,true,false]) {
 c.selectionAtTop=enabled;c._userMoving=true;c.syncLayout();c.syncFromValue();
 assert.equal(c.currentValue,'10000000');assert.equal(c.list.currentIndex,76);
 assert.equal(positions.at(-1)[1],enabled?0:1);assert.equal(commits,0);
}
assert.equal(scheduled,3);
const arc=new Function('list','root','y','height',qml.match(/readonly property real curveOffset: \{([\s\S]*?)\n            \}/)[1]);
for(const deckHeight of [240,360,720]) {
 const row=Math.max(48,Math.round(deckHeight/7));
 const center=deckHeight*.34+deckHeight*.66/2,top=center-row/2;
 const list={vertical:true,height:deckHeight-top,width:100,itemHeight:row,contentY:500};
 assert.equal(top+row/2,center);
 assert.equal(arc(list,{selectionAtTop:true,curvature:0},500,row),0);
 assert.equal(arc(list,{selectionAtTop:true,curvature:0},500+row,row),0);
}
const overlay=read('src/settings/SettingsOverlay.qml');
assert(!overlay.includes('curvature:'));
assert(qml.includes('property real curvature: 0'));
// Pro mode: Auto's deck plus ProControls' chips and one dial, no own grid.
assert(!overlay.includes('camera2ControlGrid'));
assert(overlay.includes('ProControls {'));
assert(overlay.includes('? simpleBottomShutterAnchor'));
assert(!read('src/settings.qml').includes('experimentalExposureLayout'));
const pro=read('src/settings/ProControls.qml');
const p={Settings:{mode:{rawCaptureFocusMode:'auto',rawCaptureFocusDistance:'0'},
 camera2SelectableFocusModeModel:()=>['auto','continuous','manual','infinity'],
 camera2ManualFocusDistanceModel:()=>['0.5','10','2'],
 rawCaptureFocusDistanceLabel:d=>d+'D'}};
p.root=p;vm.createContext(p);
for(const name of ['evText','focusValues','focusValue','selectFocus'])
 vm.runInContext(pro.match(new RegExp('    function '+name+'\\([^)]*\\) \\{[\\s\\S]*?\\n    \\}'))[0],p);
assert.deepEqual([0,1,-1,2/3,-8].map(p.evText),['±0 EV','+0.5 EV','−0.5 EV','+0.3 EV','−4 EV']);
// Near to far: modes, distances by falling diopters, then infinity.
p.focusModel=p.focusValues();
assert.deepEqual(p.focusModel,['auto','continuous','10','2','0.5','infinity']);
p.selectFocus('2');
assert.deepEqual([p.Settings.mode.rawCaptureFocusMode,p.focusValue()],['manual','2']);
p.selectFocus('infinity');
assert.deepEqual([p.Settings.mode.rawCaptureFocusMode,p.focusValue()],['infinity','infinity']);
p.Settings.camera2SelectableFocusModeModel=()=>['auto'];
assert.deepEqual(p.focusValues(),['auto']);
console.log('Straight lower values, selection-preserving toggles and Pro chip labels and focus dial checks passed.');
