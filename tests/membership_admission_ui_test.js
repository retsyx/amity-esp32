'use strict';
// Execute the actual admission UI with deterministic DOM, HTTP and timer seams.
// No Bluetooth, network interface, device or credential store is touched.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),crypto=require('node:crypto');
class Element {
 constructor(tag,document){this.tag=tag;this.document=document;this.children=[];this.parent=null;this.listeners={};this.attributes={};this._text='';this.value='';this.hidden=false;this.disabled=false;this.open=false;}
 set textContent(value){this._text=String(value);this.children=[];}
 get textContent(){return this._text+this.children.map(child=>child.textContent).join('');}
 append(...items){for(const child of items){child.parent=this;this.children.push(child);}}
 replaceChildren(...items){for(const child of this.children)child.parent=null;this.children=[];this._text='';this.append(...items);}
 addEventListener(type,listener){(this.listeners[type]??=[]).push(listener);}
 async dispatch(type){for(const listener of this.listeners[type]||[])await listener({target:this});}
 setAttribute(name,value){this.attributes[name]=String(value);}
 get isConnected(){return this===this.document.body||!!this.parent?.isConnected;}
 showModal(){this.open=true;}
 close(){if(!this.open)return;this.open=false;for(const listener of this.listeners.close||[])void listener({target:this});}
 remove(){if(this.parent){this.parent.children=this.parent.children.filter(child=>child!==this);this.parent=null;}}
 focus(){this.document.activeElement=this;}
 reportValidity(){return (!this.required||!!this.value)&&(!this.maxLength||this.value.length<=this.maxLength)&&(!this.pattern||new RegExp('^(?:'+this.pattern+')$').test(this.value));}
}
const walk=node=>[node,...node.children.flatMap(walk)];
const flush=async()=>{for(let n=0;n<8;n++)await new Promise(resolve=>setImmediate(resolve));};
function fixture(options={}){
 const document={createElement(tag){return new Element(tag,this);}};document.body=new Element('body',document);const addButton=document.createElement('button');addButton.id='addAmity';document.body.append(addButton);
 let now=0,nextTimer=0;const timers=new Map(),posts=[],messages=[],errors=[],held=[],heldScans=[];
 const state={scan_id:null,phase:'idle',error:'',requires_setup_code:!!options.strict,candidates:options.candidates??[{token:'opaque-one',label:'Amity A1B2'}],candidate_id:null};
 let operationStage='admitted',operationId=null,holdStatus=!!options.holdStatus,admissionReads=0;
 const main='1'.repeat(32),member='2'.repeat(32),management=document.createElement('div');management.id='managedAmityList';document.body.append(management);
 const request=async(path,data)=>{
  if(data!==undefined)posts.push([path,structuredClone(data)]);
  if(path==='backup')return {controller_octopus_id:main,octopuses:[{octopus_id:main,supported_tentacles:1}]};
  if(path==='system')return {octopus_id:main,hostname:'amity-main',membership:{peers:[{octopus_id:member,hostname:'amity-office',enrolled:true,ready:true,supported_tentacles:1}]}};
  if(path==='membership/scan'){const activate=()=>{state.scan_id=data.request_id;state.phase=options.scanning?'scanning':'found';return {operation_id:data.request_id,stage:'complete'};};if(options.holdScanStart)return new Promise(resolve=>heldScans.push(()=>resolve(activate())));return activate();}
  if(path==='membership/admission'){admissionReads++;const copy=structuredClone(state);if(holdStatus)return new Promise(resolve=>held.push(()=>resolve(copy)));return copy;}
  if(path==='membership/cancel'){if(data.scan_id!==state.scan_id)throw Object.assign(Error('This search is no longer active.'),{status:409});state.phase='cancelled';return {stage:'complete'};}
  if(path==='membership/add'){if(options.failAdd)throw Object.assign(Error('The setup code could not be verified'),{status:409});state.phase='connecting';operationId=data.request_id;return {operation_id:operationId,stage:'admitted'};}
  if(path==='operations/'+operationId)return {operation_id:operationId,stage:operationStage,message:state.error};
  throw Error('Unexpected HTTP request '+path);
 };
 const context={window:{},document,crypto:crypto.webcrypto,structuredClone,Response,token:'fixture-session',console,
  $:id=>walk(document.body).find(node=>node.id===id),customerText:value=>String(value??''),message:(value,error)=>messages.push([value,!!error]),request,
  setTimeout:(callback,delay)=>{const id=++nextTimer;timers.set(id,{callback,at:now+delay});return id;},clearTimeout:id=>timers.delete(id),
  run:action=>async event=>{try{await action(event);}catch(error){errors.push(error.message);}},
 };
 context.button=(title,action)=>{const button=document.createElement('button');button.textContent=title;button.addEventListener('click',context.run(action));return button;};
 vm.createContext(context);vm.runInContext(fs.readFileSync(require.resolve('../components/web_admin/assets/system.js'),'utf8'),context);
 const click=async node=>{if(!node.disabled)await node.dispatch('click');await flush();};
 const fill=async(node,value)=>{node.value=value;await node.dispatch('input');await flush();};
 const dialog=()=>walk(document.body).find(node=>node.tag==='dialog'&&node.open);
 const label=title=>walk(dialog()).find(node=>node.tag==='label'&&node._text===title);
 const input=title=>label(title).children.find(node=>node.tag==='input');
 const button=title=>walk(dialog()).find(node=>node.tag==='button'&&node.textContent===title);
 const tick=async()=>{now+=400;const due=[...timers].filter(([,timer])=>timer.at<=now);for(const [id,timer] of due){timers.delete(id);await timer.callback();}await flush();};
 return {context,document,state,posts,messages,errors,held,dialog,label,input,button,click,fill,tick,clock:()=>now,pollCount:()=>admissionReads,open:async()=>{await click(addButton);return dialog();},setStage:value=>operationStage=value,releaseScanStart:()=>{for(const resolve of heldScans.splice(0))resolve();},releaseStatus:()=>{holdStatus=false;for(const resolve of held.splice(0))resolve();}};
}
(async()=>{
 const normal=fixture();await normal.open();assert.match(normal.dialog().textContent,/Hold the Reset button on the new Amity for 3 seconds\./);
 const scan=normal.posts[0][1].request_id;assert.equal(normal.posts[0][0],'membership/scan');assert.equal(scan.length,32);assert.equal(normal.label('Matter setup code').hidden,true);assert.equal(normal.label('Name').hidden,false);
 for(const bad of ['', 'bad name','-bad','bad-','café']){await normal.fill(normal.input('Name'),bad);await normal.click(normal.button('Add'));assert.equal(normal.posts.filter(([path])=>path.endsWith('/add')).length,0);}
 await normal.fill(normal.input('Name'),'  AMITY-OFFICE  ');await normal.click(normal.button('Add'));
 const addition=normal.posts.find(([path])=>path.endsWith('/add'))[1];assert.equal(addition.scan_id,scan);assert.equal(addition.candidate_token,'opaque-one');assert.equal(addition.name,'amity-office');assert.equal(Object.hasOwn(addition,'setup_code'),false);
 assert.notEqual(addition.request_id,scan);normal.state.phase='finishing';await normal.tick();assert.match(normal.dialog().textContent,/Finishing setup/);assert.equal(normal.button('Add').disabled,true);
 normal.state.phase='complete';normal.setStage('complete');await normal.tick();assert.equal(normal.dialog(),undefined);assert.equal(normal.posts.filter(([path])=>path.endsWith('/cancel')).length,0);assert.equal(normal.messages.at(-1)[0],'Amity added.');
 const strict=fixture({strict:true});await strict.open();assert.equal(strict.label('Matter setup code').hidden,false);assert.equal(strict.button('Add').disabled,true);
 await strict.fill(strict.input('Name'),'bedroom');await strict.fill(strict.input('Matter setup code'),' 1234-567-8901 ');await strict.click(strict.button('Add'));
 assert.equal(strict.posts.find(([path])=>path.endsWith('/add'))[1].setup_code,' 1234-567-8901 ');assert.equal(strict.input('Matter setup code').value,'');await strict.click(strict.button('Close'));
 assert.equal(strict.posts.filter(([path])=>path.endsWith('/cancel')).length,0);assert.match(strict.messages.at(-1)[0],/continues/);
 const refused=fixture({strict:true,failAdd:true});await refused.open();await refused.fill(refused.input('Name'),'office');await refused.fill(refused.input('Matter setup code'),'MT:WRONG-CODE');await refused.click(refused.button('Add'));assert.equal(refused.input('Matter setup code').value,'');assert.equal(refused.button('Add').disabled,true);assert.deepEqual(refused.errors,['The setup code could not be verified']);await refused.fill(refused.input('Matter setup code'),'1234-567-8901');assert.equal(refused.button('Add').disabled,false);
 const multiple=fixture({candidates:[{token:'first-token',label:'Amity A1B2'},{token:'second-token',label:'Amity C3D4'}]});await multiple.open();assert.equal(multiple.button('Add').disabled,true);assert.equal(multiple.label('Name').hidden,true);
 const select=walk(multiple.dialog()).find(node=>node.tag==='select');assert.deepEqual(select.children.map(option=>option.textContent),['Choose an Amity','Amity A1B2','Amity C3D4']);select.value='second-token';await select.dispatch('change');await multiple.fill(multiple.input('Name'),'den');await multiple.click(multiple.button('Add'));assert.equal(multiple.posts.find(([path])=>path.endsWith('/add'))[1].candidate_token,'second-token');
 const cancel=fixture({scanning:true});await cancel.open();const cancelled=cancel.state.scan_id;await cancel.click(cancel.button('Cancel'));assert.equal(cancel.posts.find(([path])=>path.endsWith('/cancel'))[1].scan_id,cancelled);await cancel.tick();assert.equal(cancel.dialog(),undefined);
 const lateScan=fixture({holdScanStart:true});await lateScan.open();const lateId=lateScan.posts[0][1].request_id;await lateScan.click(lateScan.button('Cancel'));assert.equal(lateScan.state.scan_id,null);lateScan.releaseScanStart();await flush();assert.equal(lateScan.state.phase,'cancelled');assert.equal(lateScan.posts.filter(([path])=>path.endsWith('/cancel')).length,2);assert.ok(lateScan.posts.filter(([path])=>path.endsWith('/cancel')).every(([,body])=>body.scan_id===lateId));
 const stale=fixture({holdStatus:true});await stale.open();const old=stale.state.scan_id;await stale.click(stale.button('Cancel'));await stale.open();const current=stale.state.scan_id;assert.notEqual(old,current);stale.releaseStatus();await flush();assert.ok(stale.dialog());assert.equal(stale.state.scan_id,current);assert.equal(stale.label('Name').hidden,false);assert.equal(stale.posts.filter(([path])=>path.endsWith('/add')).length,0);
 stale.state.scan_id='f'.repeat(32);await stale.tick();assert.match(stale.dialog().textContent,/search changed/);assert.equal(stale.button('Add').disabled,true);
 const unknown=fixture();await unknown.open();await unknown.fill(unknown.input('Name'),'office');await unknown.click(unknown.button('Add'));unknown.state.scan_id='e'.repeat(32);unknown.setStage('unknown');await unknown.tick();assert.match(unknown.dialog().textContent,/Check the Amity devices list/);assert.equal(unknown.button('Add').disabled,true);assert.equal(unknown.posts.filter(([path])=>path.endsWith('/add')).length,1);
 for(const error of ['The new Amity reported that its setup security check failed. Restart it and try again.','This Amity failed its setup security check. Restart it and try again.'])for(const stage of ['admitted','failed']){
  const security=fixture();await security.open();await security.fill(security.input('Name'),'office');await security.click(security.button('Add'));
  security.state.phase='failed';security.state.error=error;security.setStage(stage);await security.tick();
  assert.ok(security.dialog().textContent.includes(error));assert.equal(security.button('Add').disabled,true);
  assert.equal(security.posts.filter(([path])=>path==='membership/add').length,1);assert.deepEqual(security.errors,[]);
 }
 const listed=fixture();await listed.context.window.amityInterface.route('config').decode(new Response(JSON.stringify({version:1,device_choices:[],activities:[]})));await listed.context.window.amityInterface.refresh();assert.equal(listed.document.body.children.find(node=>node.id==='managedAmityList').children.length,2);assert.match(listed.document.body.textContent,/amity-office/);
 const collecting=fixture({scanning:true});await collecting.open();assert.equal(collecting.button('Add').disabled,true);collecting.state.phase='found';await collecting.tick();assert.equal(collecting.label('Name').hidden,false);
 const liveChoices=fixture({scanning:true,candidates:[]});await liveChoices.open();assert.equal(liveChoices.button('Add').disabled,true);
 liveChoices.state.phase='found';liveChoices.state.candidates=[{token:'early-board',label:'Amity A1B2'}];await liveChoices.tick();await liveChoices.fill(liveChoices.input('Name'),'office');assert.equal(liveChoices.button('Add').disabled,false);assert.ok(liveChoices.clock()<20000);
 liveChoices.state.candidates.push({token:'later-board',label:'Amity C3D4'});await liveChoices.tick();
 const liveSelect=walk(liveChoices.dialog()).find(node=>node.tag==='select');assert.deepEqual(liveSelect.children.map(option=>option.value),['','early-board','later-board']);assert.equal(liveSelect.value,'early-board');assert.equal(liveChoices.input('Name').value,'office');
 liveSelect.value='later-board';await liveSelect.dispatch('change');await liveChoices.click(liveChoices.button('Add'));assert.equal(liveChoices.posts.find(([path])=>path==='membership/add')[1].candidate_token,'later-board');assert.ok(liveChoices.clock()<20000);
 const ongoing=fixture({scanning:true,candidates:[]});await ongoing.open();ongoing.state.phase='found';ongoing.state.candidates=[{token:'first',label:'Amity A1B2'}];await ongoing.tick();const initialPolls=ongoing.pollCount();for(let n=0;n<51;n++)await ongoing.tick();assert.ok(ongoing.pollCount()>initialPolls+49);assert.equal(ongoing.dialog().open,true);
 ongoing.state.candidates.push({token:'second',label:'Amity C3D4'});await ongoing.tick();assert.equal(walk(ongoing.dialog()).find(node=>node.tag==='select').children.length,3);await ongoing.click(ongoing.button('Cancel'));ongoing.state.phase='found';ongoing.state.candidates.push({token:'late-after-close',label:'Amity E5F6'});await ongoing.tick();assert.equal(ongoing.dialog(),undefined);assert.equal(ongoing.posts.filter(([path])=>path==='membership/add').length,0);assert.equal(ongoing.posts.filter(([path])=>path==='membership/cancel').length,1);
 assert.deepEqual([...normal.errors,...strict.errors,...multiple.errors,...cancel.errors,...stale.errors,...collecting.errors,...unknown.errors,...listed.errors,...lateScan.errors,...liveChoices.errors,...ongoing.errors],[]);
 console.log('PASS actual admission UI: immediate scan/early selection, live later candidates throughout the bounded search, normal/strict code profile, validated names, scoped cancel, late/stale dialogs, and admitted connection cleanup');
})().catch(error=>{console.error(error);process.exitCode=1;});
