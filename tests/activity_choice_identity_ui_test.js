'use strict';
// Exercise the shipped UI adapter's projection/submission boundary without a
// browser or firmware. Equal presentation labels must not replace scoped IDs.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),crypto=require('node:crypto');
const A='component:'+'a'.repeat(32),B='component:'+'b'.repeat(32),C='component:'+'c'.repeat(32);
const FIRST='1'.repeat(32),SECOND='2'.repeat(32),THIRD='3'.repeat(32);
const label='Twin · Adapter 1';
const choices=[
 {value:A,title:'Twin',location:'Adapter 1',roles:['display','source','audio','switch']},
 {value:B,title:'Twin',location:'Adapter 1',roles:['display','source','audio','switch']},
 {value:C,title:'Unique player',location:'Adapter 2',roles:['source']},
];
const original={version:1,expected_config_revision:'7',expected_presentation_revision:'3',device_choices:choices,activities:[
 {activity_id:FIRST,name:'First activity',display:A,source:B,audio:A,switch:{device:B,input:2}},
 {activity_id:SECOND,name:'Second activity',display:B,source:A,audio:B,switch:{device:A,input:6}},
 {activity_id:THIRD,name:'Third activity',display:'',source:C,audio:''},
]};
const copy=value=>JSON.parse(JSON.stringify(value));
function fixture(){
 const context={window:{},crypto:crypto.webcrypto,structuredClone,Response,readResponseJSON:response=>response.json(),
  config:{expected_config_revision:'7',expected_presentation_revision:'3'},
  $:()=>({addEventListener(){}}),run:action=>action};
 vm.createContext(context);vm.runInContext(fs.readFileSync(require.resolve('../components/web_admin/assets/system.js'),'utf8'),context);
 const adapter=context.window.amityInterface;
 return {adapter,
  load:async document=>{const route=adapter.route('config');return (await route.decode(new Response(JSON.stringify(document)))).json();},
  save:document=>copy(adapter.route('config',document).data),
  refreshChoices:async values=>adapter.route('status').decode(new Response(JSON.stringify({device_choices:values,devices:[]}))),
 };
}
function assertOriginalReferences(document){
 for(const activity of document.activities){
  const saved=original.activities.find(row=>row.activity_id===activity.activity_id);assert.ok(saved);
  for(const role of ['display','source','audio'])assert.equal(activity[role],saved[role],activity.activity_id+' '+role);
  if(saved.switch)assert.equal(activity.switch.device,saved.switch.device,activity.activity_id+' switch');
 }
}
(async()=>{
 const unchanged=fixture(),projected=await unchanged.load(original);
 assert.equal(projected.activities[0].source,label);assert.equal(projected.activities[1].source,label);
 assertOriginalReferences(unchanged.save(projected));
 const edited=copy(projected);edited.activities.reverse();edited.activities[1].name='Renamed only';edited.activities[1].switch.input=4;
 const submitted=unchanged.save(JSON.stringify(edited));assertOriginalReferences(submitted);assert.equal(submitted.activities[1].switch.input,4);

 // Polling can change a label while a draft still carries the original text.
 // Even if that old text now matches only the opposite-side choice, keep the
 // unchanged field's existing stable selector rather than silently retargeting.
 await unchanged.refreshChoices(choices.map(choice=>({...choice,title:choice.value===A?'Renamed Twin':choice.title})));
 assertOriginalReferences(unchanged.save(projected));

 const additions=fixture(),draft=await additions.load(original);
 draft.activities.push({name:'New activity',display:'',source:label,audio:''});
 assert.throws(()=>additions.save(draft),/Choose source/);
 draft.activities.at(-1).activity_id='9'.repeat(32);
 assert.throws(()=>additions.save(draft),/Choose source/);
 delete draft.activities.at(-1).activity_id;draft.activities.at(-1).source='Unique player · Adapter 2';
 assert.equal(additions.save(draft).activities.at(-1).source,C);

 const changed=fixture(),changedDraft=await changed.load(original);
 changedDraft.activities[2].source=label;
 assert.throws(()=>changed.save(changedDraft),/Choose source/);
 changedDraft.activities[2].source=A;
 assert.equal(changed.save(changedDraft).activities[2].source,A); // An exact selector is unambiguous.

 const unavailable=fixture(),oldDraft=await unavailable.load(original);
 await unavailable.refreshChoices(choices.filter(choice=>choice.value!==A));
 assert.throws(()=>unavailable.save(oldDraft),/Choose display/);

 // Reading the latest server config during a save must not overwrite the
 // exact baseline when a concurrent change is invisible in the display text.
 const concurrent=fixture(),oldProjected=await concurrent.load(original),changedServer=copy(original);
 changedServer.activities[0].source=A;changedServer.expected_config_revision='8';
 await assert.rejects(()=>concurrent.load(changedServer),/Activities changed in another session/);
 assert.equal(concurrent.save(oldProjected).activities[0].source,B);

 // A confirmed editor save adopts exactly the selectors we submitted, so an
 // intentional equal-label change can reload, while a later hidden overwrite
 // from another session is still rejected.
 const deliberate=fixture(),deliberateDraft=await deliberate.load(original);
 deliberateDraft.activities[0].source=A;
 const pending=deliberate.adapter.route('config',deliberateDraft);
 await pending.decode(new Response(JSON.stringify({stage:'applied',saved:true})));
 const applied=copy(original);applied.activities[0].source=A;applied.expected_config_revision='8';
 const appliedProjected=await deliberate.load(applied);
 assert.equal(deliberate.save(appliedProjected).activities[0].source,A);
 await assert.rejects(()=>deliberate.load({...original,expected_config_revision:'9'}),/Activities changed in another session/);

 const failed=fixture(),failedDraft=await failed.load(original);failedDraft.activities[0].source=A;
 await failed.adapter.route('config',failedDraft).decode(new Response(JSON.stringify({stage:'failed',saved:false})));
 await assert.rejects(()=>failed.load(applied),/Activities changed in another session/);

 const restore=fixture(),imported=await restore.load(original);
 const restored=restore.adapter.route('restore',{configuration:{version:3,activities:[]},expected_boot_id:'c'.repeat(32)});
 assert.equal(restored.path,'activity-editor/restore');
 assert.equal(restored.data.configuration.version,3);
 assert.equal(restored.data.expected_boot_id,'c'.repeat(32));
 restore.adapter.close();
 await restore.load({...original,activities:[]});
 assert.throws(()=>restore.save(imported),/Choose display/);
 console.log('PASS activity choices: unchanged scoped selectors survive equal labels, row reorder and unrelated edits; new ambiguity and stale references reject');
})().catch(error=>{console.error(error);process.exitCode=1;});
