/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */


'use strict';
// Provisional customer terms; internal identifiers stay in data only.
window.amityTerms=Object.freeze({device:'Amity device',devices:'Amity devices',adapter:'Adapter',adapters:'Adapters',system:'Amity system',systems:'Amity systems'});
function customerText(value){return String(value??'').replace(/\boctopuses\b/gi,window.amityTerms.devices).replace(/\boctopus\b/gi,window.amityTerms.device).replace(/\btentacles\b/gi,window.amityTerms.adapters).replace(/\btentacle\b/gi,window.amityTerms.adapter).replace(/\bconsortiums\b/gi,window.amityTerms.systems).replace(/\bconsortium\b/gi,window.amityTerms.system);}

const $=id=>document.getElementById(id);
let token=sessionStorage.getItem('amityToken')||'',config={version:1,activities:[]},state={},polling=false,dirty=false,saving=false,persistedActivities='[]',savedActivities=[],scanPending=false,scanAfter=null,scanGeneration=null,recommendations=[],autoRecommend=false,editingRecommendation=null,pairingPending=false,matterRemovalPending=false,remotePending=false,remoteState={devices:[]},savedActivityEntries=new Map(),editingActivity=null,activityActionPending=false;
let firmwareUpdating=false,firmwareWaitRunning=false,firmwareUpdateRecord=null;
let hostnameReconnect=null;
let homekitPending=false,homekitPolling=false,homekitState={};
let matterState={fabrics:[]};
const firmwareUpdateKey='amityFirmwareUpdate',firmwareResultKey='amityFirmwareUpdateResult';
function message(text,error=false){$('message').textContent=customerText(text);$('message').classList.toggle('error',error);}
function activitySnapshot(activities){return JSON.stringify(activities.map(a=>[a.name??'',a.display??'',a.source??'',a.audio??'',a.switch?.device??'',a.switch?.device?(a.switch.input??0):null,a.activity_id??'']));}
function updateActivityButtons(){
 const locked=activityLocked();
 document.querySelectorAll('#activities button, #recommendationEditor button').forEach(b=>{
  b.disabled=locked||(b.hasAttribute('data-add-recommendation')&&config.activities.length>=16)||(b.id==='addActivity'&&config.activities.length>=16);
 });
 if(locked)endActivityDrag(true);updateActivityState();
}
function markDirty(){dirty=activitySnapshot(config.activities)!==persistedActivities;$('saveActivities').hidden=!dirty;$('discardActivities').hidden=!dirty;if(dirty)autoRecommend=false;renderRecommendations();}
function reportError(error){if(!firmwareUpdating&&!hostnameReconnect&&!error.silent)message(error.message,true);}
function showLogin(){if(firmwareUpdating)return;window.amityInterface?.close();homekitPending=false;renderHomekit({});if($('activityEditor').open)$('activityEditor').close();if($('recommendationEditor').open)$('recommendationEditor').close();editingActivity=null;token='';sessionStorage.removeItem('amityToken');$('onboarding').hidden=true;$('connection').textContent='';$('login').hidden=false;$('admin').hidden=true;$('logout').hidden=true;}
async function request(path,data,raw=false){
 const headers={Authorization:`Bearer ${token}`};if(data!==undefined)headers['Content-Type']='application/json';
 const mapped=window.amityInterface?.route(path,data)||{path,data};
 let response=await fetch('/api/'+mapped.path,{method:mapped.data===undefined?'GET':'POST',headers,body:mapped.data===undefined?undefined:typeof mapped.data==='string'?mapped.data:JSON.stringify(mapped.data)});
 if(response.ok&&mapped.decode)response=await mapped.decode(response);

 if(response.status===401&&path!=='login'){if(!firmwareUpdating&&!hostnameReconnect){await showWelcome();if(!hostnameReconnect)message('');}throw Object.assign(Error(),{silent:true});}
 if(!response.ok){let e=await response.json().catch(()=>({error:'Request failed'}));const error=Error(customerText(e.error||e.message||'Request failed'));error.status=response.status;error.retryAfter=Math.max(0,Math.min(60,Number(response.headers.get('Retry-After'))||0));throw error;}
 return raw?response:response.json();
}
function run(action){return async event=>{try{await action(event);}catch(e){reportError(e);}};}
function button(text,handler){const b=document.createElement('button');b.textContent=text;b.addEventListener('click',run(handler));return b;}
function cancelOnBackdrop(dialog,cancel){
 let pressedOutside=false;
 const outside=event=>{const box=dialog.getBoundingClientRect();return event.clientX<box.left||event.clientX>=box.right||event.clientY<box.top||event.clientY>=box.bottom;};
 dialog.addEventListener('pointerdown',event=>{pressedOutside=event.target===dialog&&outside(event);});
 dialog.addEventListener('pointercancel',()=>{pressedOutside=false;});
 dialog.addEventListener('click',event=>{const dismiss=pressedOutside&&event.target===dialog&&outside(event);pressedOutside=false;if(dismiss)cancel();});
}
function label(text,value,onChange,list){
 if(list)return deviceField(text,value,onChange,list);
 const l=document.createElement('label');l.textContent=text;const i=document.createElement('input');i.value=value??'';i.maxLength=63;i.addEventListener('input',()=>onChange(i.value));l.append(i);return l;
}
let deviceFieldSequence=0;
function deviceField(text,value,onChange,list){
 const root=document.createElement('div'),l=document.createElement('label'),i=document.createElement('input'),options=document.createElement('div');
 root.className='device-field';l.textContent=text;i.value=value??'';i.maxLength=512;i.autocomplete='off';i.dataset.deviceList=list;i.dataset.deviceRole=list.replace('DeviceNames','');
 options.id='deviceOptions'+(++deviceFieldSequence);options.className='device-options';options.hidden=true;options.setAttribute('role','listbox');options.setAttribute('aria-label',text+' equipment');
 i.setAttribute('role','combobox');i.setAttribute('aria-autocomplete','list');i.setAttribute('aria-controls',options.id);i.setAttribute('aria-expanded','false');
 let active=-1,filter=false;
 const close=()=>{options.hidden=true;i.setAttribute('aria-expanded','false');i.removeAttribute('aria-activedescendant');active=-1;};
 const choose=value=>{i.value=value;onChange(value);filter=false;close();};
 const highlight=()=>{[...options.children].forEach((b,n)=>b.setAttribute('aria-selected',String(n===active)));if(active>=0){const b=options.children[active];i.setAttribute('aria-activedescendant',b.id);b.scrollIntoView({block:'nearest'});}else i.removeAttribute('aria-activedescendant');};
 const render=()=>{
  if(i.disabled)return close();
  const highlighted=active>=0?options.children[active]?.textContent:null;
  options.replaceChildren();active=-1;
  const add=(title,action)=>{const b=document.createElement('button');b.type='button';b.textContent=title;b.id=options.id+'-'+options.children.length;b.setAttribute('role','option');b.setAttribute('aria-selected','false');b.tabIndex=-1;b.addEventListener('mousedown',e=>e.preventDefault());b.addEventListener('click',run(action));options.append(b);};
  const query=filter?i.value.toLocaleLowerCase():'';
  for(const option of $(list).options)if(!query||option.value.toLocaleLowerCase().includes(query))add(option.value,()=>choose(option.value));
  if(window.amityInterface?.enabled)add('Add component…',async()=>{close();await window.amityInterface.addComponent(i.dataset.deviceRole,i.value,choose,i);});
  options.hidden=!options.childElementCount;i.setAttribute('aria-expanded',String(!options.hidden));i.removeAttribute('aria-activedescendant');
  if(highlighted!==null){active=[...options.children].findIndex(b=>b.textContent===highlighted);highlight();}
 };
 i.addEventListener('focus',()=>{filter=false;active=-1;render();});i.addEventListener('click',()=>{if(options.hidden){filter=false;render();}});
 i.addEventListener('input',()=>{onChange(i.value);filter=true;active=-1;render();});
 i.addEventListener('keydown',e=>{
  if(e.key==='Escape'&&!options.hidden){e.preventDefault();e.stopPropagation();close();return;}
  if(['ArrowDown','ArrowUp'].includes(e.key)){e.preventDefault();if(options.hidden)render();if(options.childElementCount){active=active<0?(e.key==='ArrowDown'?0:options.childElementCount-1):(active+(e.key==='ArrowDown'?1:-1)+options.childElementCount)%options.childElementCount;highlight();}}
  else if(e.key==='Enter'&&!options.hidden&&active>=0){e.preventDefault();options.children[active].click();}
 });
 root.addEventListener('focusout',e=>{if(!root.contains(e.relatedTarget))close();});
 root.addEventListener('devicechoiceschange',()=>{if(!options.hidden)render();});
 l.append(i);root.append(l,options);return root;
}
function selectTab(id){document.querySelectorAll('.tab').forEach(s=>s.hidden=s.id!==id);document.querySelectorAll('[data-tab]').forEach(b=>{const selected=b.dataset.tab===id;b.classList.toggle('selected',selected);if(selected)b.setAttribute('aria-current','page');else b.removeAttribute('aria-current');});if(id==='homekit')refreshHomekit().catch(reportError);if(id==='amityDevices')window.amityInterface?.refresh().catch(reportError);}
document.querySelectorAll('[data-tab]').forEach(b=>b.addEventListener('click',()=>selectTab(b.dataset.tab)));
function activityFields(a,changed=markDirty){
 const field=(text,value,set,list)=>label(text,value,v=>{set(v);changed();},list);
 const fields=document.createElement('div');fields.className='fields';fields.append(field('Name',a.name,v=>a.name=v),...['display','source','audio'].map(k=>field(k[0].toUpperCase()+k.slice(1),a[k],v=>a[k]=v,k+'DeviceNames')));
 fields.append(field('Switch Device',a.switch?.device,v=>{if(v)a.switch={device:v,input:a.switch?.input||0};else delete a.switch;input.hidden=!a.switch?.device;const control=input.querySelector('input');control.disabled=!a.switch;control.value=a.switch?.input??'';},'switchDeviceNames'));
 const input=field('Input',a.switch?.input??'',v=>{if(a.switch)a.switch.input=Number(v);});input.hidden=!a.switch?.device;input.querySelector('input').type='number';input.querySelector('input').min=0;input.querySelector('input').max=255;input.querySelector('input').dataset.switchInput='';input.querySelector('input').disabled=!a.switch;fields.append(input);
 return fields;
}
function savedEntry(activity){return savedActivityEntries.get(activity);}
function matchesSaved(activity,reference=activity){const saved=savedEntry(reference);return !!saved&&activitySnapshot([activity])===saved.snapshot;}
function deviceRoles(device){
 switch(device.type){
  case 0:return ['display','audio'];
  case 1:case 3:case 4:return ['source'];
  case 5:return ['audio','switch'];
  case 6:case 7:return ['switch'];
  default:return [];
 }
}
function renderDeviceNames(){
 const names={display:new Set(),source:new Set(),audio:new Set(),switch:new Set()},knownRoles=new Map();
 const add=(role,name)=>{if(typeof name==='string'&&name.trim())names[role].add(name);};
 for(const device of state.devices||[]){
  const roles=deviceRoles(device);if(!roles.length)continue;
  if(!knownRoles.has(device.name))knownRoles.set(device.name,new Set());
  for(const role of roles){knownRoles.get(device.name).add(role);add(role,device.name);}
 }
 for(const activity of savedActivities)for(const role of Object.keys(names)){
  const name=role==='switch'?activity.switch?.device:activity[role];
  if(!knownRoles.has(name)||knownRoles.get(name).has(role))add(role,name);
 }
 if(window.amityInterface?.enabled)for(const role of Object.keys(names))for(const name of window.amityInterface.deviceNames(role))add(role,name);
 for(const [role,values] of Object.entries(names)){
  const list=$(role+'DeviceNames');list.replaceChildren();
  for(const name of [...values].sort((a,b)=>a.localeCompare(b))){const option=document.createElement('option');option.value=name;list.append(option);}
 }
 document.querySelectorAll('.device-field').forEach(field=>field.dispatchEvent(new Event('devicechoiceschange')));
}
function relabelActivityEquipment(resolve){
 const update=activity=>{if(!activity)return;for(const role of ['display','source','audio','switch']){const value=role==='switch'?activity.switch?.device:activity[role];if(!value)continue;const next=resolve(role,value,activity.activity_id);if(role==='switch')activity.switch.device=next;else activity[role]=next;}};
 config.activities.forEach(update);savedActivities.forEach(update);recommendations.forEach(update);update(editingActivity?.activity);update(editingRecommendation?.activity);
 for(const saved of savedActivityEntries.values())if(savedActivities[saved.index])saved.snapshot=activitySnapshot([savedActivities[saved.index]]);
 persistedActivities=activitySnapshot(savedActivities);
 for(const [id,activity] of [['activityFields',editingActivity?.activity],['recommendationFields',editingRecommendation?.activity]])if(activity)for(const i of $(id).querySelectorAll('[data-device-role]')){const role=i.dataset.deviceRole;i.value=role==='switch'?activity.switch?.device||'':activity[role]||'';}
 markDirty();
}
function rememberSaved(activities=config.activities){
 savedActivityEntries=new Map(activities.map((a,index)=>[a,{index,snapshot:activitySnapshot([a])}]));
 savedActivities=structuredClone(activities);persistedActivities=activitySnapshot(activities);
 renderDeviceNames();
}
function activityActive(activity){const saved=savedEntry(activity);return !!saved&&saved.index===state.current_activity;}
function activityLocked(){return activitiesScanning()||saving||activityActionPending||!!window.amityInterface?.componentPending;}
function activitiesScanning(){return !!(scanPending||state.discovering||(scanAfter!==null&&state.discovery_generation===scanAfter));}
function updateActivityState(){
 document.querySelectorAll('#activityList article').forEach((row,index)=>{
  const a=config.activities[index],start=row.querySelector('[data-start]');
  start.textContent=activityActive(a)?'Repair':'Start';
  start.disabled=activityLocked()||!matchesSaved(a);
  start.title=matchesSaved(a)?'':'Save activity changes before starting.';
  row.classList.toggle('active',activityActive(a));
  row.querySelector('.activity-badge').hidden=!activityActive(a);
  row.querySelector('[data-drag]').setAttribute('aria-disabled',String(activityLocked()));
 });
 updateActivityEditor();
}
function validateActivity(a,others){
 if(!a.name)throw Error('Activity name is required.');
 if(others.some(other=>other.name===a.name))throw Error('Activity names must be unique.');
 if(new TextEncoder().encode(a.name).length>63)throw Error('Activity fields must be at most 63 bytes and contain no control characters.');
 for(const value of [a.name,a.display,a.source,a.audio,a.switch?.device])
  if(value&&(!window.amityInterface?.isDeviceChoice(value)&&(new TextEncoder().encode(value).length>63)||/[\x00-\x1f]/.test(value)))throw Error('Activity fields must be at most 63 bytes and contain no control characters.');
 if(a.switch&&(!Number.isInteger(a.switch.input)||a.switch.input<0||a.switch.input>255))throw Error('Switch input must be between 0 and 255.');
}
function openActivityEditor(target=null){
 if(activityLocked())return;
 if(!target&&config.activities.length>=16)return message('Up to 16 activities are supported.',true);
 editingActivity={target,activity:structuredClone(target||{name:'New activity',display:'',source:'',audio:''})};
 $('activityEditorTitle').textContent=target?'Edit activity':'Add activity';$('activityEditorError').textContent='';
 $('activityForm').querySelector('button[type=submit]').textContent=target?'OK':'Add';
 $('activityFields').replaceChildren(activityFields(editingActivity.activity,()=>{$('activityEditorError').textContent='';updateActivityEditor();}));
 updateActivityEditor();$('activityEditor').showModal();$('activityFields').querySelector('input').focus();
}
function updateActivityEditor(){
 if(!editingActivity)return;
 const {target,activity}=editingActivity,needsSave=!matchesSaved(activity,target),active=activityActive(target);
 $('editorStart').textContent=needsSave?(active?'Save and repair':'Save and start'):(active?'Repair':'Start');
 $('activityEditor').querySelectorAll('button').forEach(b=>b.disabled=activityLocked());
 $('activityEditor').querySelectorAll('input').forEach(i=>i.disabled=activityLocked()||(i.hasAttribute('data-switch-input')&&!activity.switch));
}
function applyActivityDraft(){
 const {target,activity}=editingActivity;
 validateActivity(activity,config.activities.filter(a=>a!==target));
 if(target){Object.keys(target).forEach(key=>delete target[key]);Object.assign(target,structuredClone(activity));}
 else {if(config.activities.length>=16)throw Error('Up to 16 activities are supported.');config.activities.push(structuredClone(activity));editingActivity.target=config.activities.at(-1);}
 renderActivities();return editingActivity.target;
}
async function currentSavedConfiguration(){
 const latest=await request('config');
 if(activitySnapshot(latest.activities)!==persistedActivities)throw Error('Activities changed in another session. Reload the page before continuing.');
 return latest;
}
async function refreshActivityConfiguration(){
 const latest=await request('config');
 if(dirty){message('Activities changed. Reload the page before saving your edits.',true);return false;}
 config=latest;rememberSaved();renderActivities();await refresh();return true;
}
async function persistActivities(editor=null){
 if(saving)throw Error('Activities are being saved.');
 const references=[...config.activities];let target=editor?.target;
 if(editor){
  validateActivity(editor.activity,references.filter(a=>a!==target));
  if(!target){if(references.length>=16)throw Error('Up to 16 activities are supported.');target=structuredClone(editor.activity);references.push(target);}
 }
 saving=true;updateActivityButtons();
 try{
  const latest=await currentSavedConfiguration(),submitted=JSON.stringify({...latest,activities:references.map(a=>editor&&a===target?editor.activity:a)});
  const result=await request('config',submitted);const applied=window.amityInterface?.enabled?await request('config'):JSON.parse(submitted),saved=applied.activities;
  config.expected_config_revision=applied.expected_config_revision;config.expected_presentation_revision=applied.expected_presentation_revision;
  if(editor){
   if(editor.target){Object.keys(target).forEach(key=>delete target[key]);Object.assign(target,structuredClone(editor.activity));}
   else {config.activities.push(target);editor.target=target;}
  }
  references.forEach((a,index)=>{if(saved[index]?.activity_id)a.activity_id=saved[index].activity_id;});
  if(editor&&target.activity_id)editor.activity.activity_id=target.activity_id;
  savedActivityEntries=new Map(references.map((a,index)=>[a,{index,snapshot:activitySnapshot([saved[index]])}]));
  savedActivities=structuredClone(saved);persistedActivities=activitySnapshot(saved);renderDeviceNames();renderActivities();message('Activities saved.'+(result.unresolved?.length?' '+result.unresolved.join(' '):''),!!result.unresolved?.length);await refresh();
 }finally{saving=false;updateActivityButtons();}
}
async function startActivity(activity,draft=activity){
 if(activityLocked()||!matchesSaved(draft,activity))return;
 const index=savedEntry(activity).index;activityActionPending=true;updateActivityButtons();
 try{await currentSavedConfiguration();await request('activity',{index});message('');await refresh();}finally{activityActionPending=false;updateActivityButtons();}
}
function modalAction(action){return async event=>{
 event.preventDefault();if(activityLocked())return;
 try{$('activityEditorError').textContent='';await action();}
 catch(error){if(!error.silent)$('activityEditorError').textContent=error.message;}
};}
$('activityForm').addEventListener('submit',modalAction(()=>{applyActivityDraft();$('activityEditor').close();}));
$('cancelActivity').addEventListener('click',()=>{if(!activityLocked())$('activityEditor').close();});
cancelOnBackdrop($('activityEditor'),()=>{if(!activityLocked())$('activityEditor').close();});
$('activityEditor').addEventListener('cancel',e=>{if(activityLocked())e.preventDefault();});
$('activityEditor').addEventListener('close',()=>{editingActivity=null;});
$('editorStart').addEventListener('click',modalAction(async()=>{
 let activity=editingActivity.target;
 if(!matchesSaved(editingActivity.activity,activity)){await persistActivities(editingActivity);activity=editingActivity.target;}
 await startActivity(activity,editingActivity.activity);
}));
function moveActivity(from,to){
 if(activityLocked()||from===to||to<0||to>=config.activities.length)return;
 const [a]=config.activities.splice(from,1);config.activities.splice(to,0,a);renderActivities();
 $('activityOrderStatus').textContent=`${a.name} moved to position ${to+1}.`;
}
let activityDrag=null;
function endActivityDrag(cancel=false){
 if(!activityDrag)return;const drag=activityDrag;activityDrag=null;cancelAnimationFrame(drag.animation);
 drag.handle.classList.remove('dragging');document.querySelectorAll('.drop-target').forEach(row=>row.classList.remove('drop-target'));
 if(drag.handle.hasPointerCapture(drag.pointer))drag.handle.releasePointerCapture(drag.pointer);
 if(!cancel&&drag.moved)moveActivity(drag.from,drag.to);
}
function positionActivityDrag(){
 const drag=activityDrag;if(!drag?.moved)return;
 const rows=[...$('activityList').children];
 drag.to=rows.reduce((best,row,index)=>{const box=row.getBoundingClientRect(),distance=Math.abs(drag.y-(box.top+box.height/2));return distance<best.distance?{index,distance}:best;},{index:0,distance:Infinity}).index;
 rows.forEach((row,index)=>row.classList.toggle('drop-target',index===drag.to));
}
function scrollActivityDrag(){
 const drag=activityDrag;if(!drag)return;
 if(drag.moved){if(drag.y<70)window.scrollBy(0,-10);else if(drag.y>innerHeight-70)window.scrollBy(0,10);positionActivityDrag();}
 drag.animation=requestAnimationFrame(scrollActivityDrag);
}
function dragHandle(index,a){
 const handle=document.createElement('span');handle.className='drag-handle';handle.dataset.drag='';handle.tabIndex=0;handle.setAttribute('role','button');handle.textContent='⠿';
 handle.setAttribute('aria-label',`Reorder ${a.name}`);handle.setAttribute('aria-describedby','activityDragHelp');
 handle.addEventListener('keydown',e=>{if(e.key==='Escape'){endActivityDrag(true);return;}if(!['ArrowUp','ArrowDown'].includes(e.key))return;e.preventDefault();if(activityLocked())return;const to=index+(e.key==='ArrowUp'?-1:1);moveActivity(index,to);$('activityList').children[Math.max(0,Math.min(to,config.activities.length-1))]?.querySelector('[data-drag]').focus();});
 handle.addEventListener('pointerdown',e=>{
  if(activityLocked()||e.button!==0)return;e.preventDefault();handle.focus();handle.setPointerCapture(e.pointerId);
  activityDrag={handle,pointer:e.pointerId,from:index,to:index,y:e.clientY,startY:e.clientY,moved:false};scrollActivityDrag();
 });
 handle.addEventListener('pointermove',e=>{
  if(!activityDrag||activityDrag.pointer!==e.pointerId)return;
  if(activityLocked())return endActivityDrag(true);
  const drag=activityDrag;if(Math.abs(e.clientY-drag.startY)>5)drag.moved=true;drag.y=e.clientY;if(!drag.moved)return;
  handle.classList.add('dragging');positionActivityDrag();
 });
 handle.addEventListener('pointerup',()=>endActivityDrag());handle.addEventListener('pointercancel',()=>endActivityDrag(true));
 handle.addEventListener('lostpointercapture',()=>endActivityDrag(true));
 return handle;
}
function renderActivities(){
 endActivityDrag(true);const list=$('activityList');list.replaceChildren();config.activities.forEach((a,index)=>{
  const row=document.createElement('article');row.className='activity';
  const heading=document.createElement('div');heading.className='activity-title';
  const title=document.createElement('span');title.className='activity-name';title.textContent=a.name;
  const badge=document.createElement('span');badge.className='activity-badge';badge.textContent='Active';badge.hidden=true;
  heading.append(title,badge);
  const controls=document.createElement('div');controls.className='activity-controls';
  const edit=button('Edit',()=>openActivityEditor(a));edit.className='quiet';
  const remove=button('Delete',()=>{if(activityLocked())return;config.activities.splice(index,1);renderActivities();});remove.className='danger quiet';
  const start=button('Start',()=>startActivity(a));start.dataset.start=index;start.className='primary';
  controls.append(edit,remove,start);
  row.append(dragHandle(index,a),heading,controls);list.append(row);
 });markDirty();updateActivityState();
}
// Match management/activities.py: names and manual switch settings do not affect equivalence.
function is_equivalent(a,b){return ['display','source','audio'].every(role=>(a[role]??'')===(b[role]??''));}
function addRecommendation(a){
 if(config.activities.length>=16)return message('Up to 16 activities are supported.',true);
 const added=structuredClone(a),base=(a.name||'New activity').slice(0,63);added.name=base;
 for(let n=2;config.activities.some(existing=>existing.name===added.name);n++){const suffix=` (${n})`;added.name=base.slice(0,63-suffix.length)+suffix;}
 config.activities.push(added);renderActivities();
}
function renderRecommendations(){
 $('recommendationList').replaceChildren();
 recommendations.forEach((a,index)=>{if(config.activities.some(existing=>is_equivalent(a,existing)))return;const row=document.createElement('div');row.className='recommendation actions';row.dataset.recommendation=index;const name=document.createElement('span');name.className='recommendation-name';name.textContent=a.name;
 const edit=button('Edit',()=>{editingRecommendation={index,activity:structuredClone(a)};$('recommendationFields').replaceChildren(activityFields(editingRecommendation.activity));$('recommendationEditor').showModal();});
 const add=button('Add',()=>addRecommendation(a));add.dataset.addRecommendation='';add.disabled=config.activities.length>=16;row.append(name,edit,add);$('recommendationList').append(row);});$('recommendations').hidden=!$('recommendationList').childElementCount;updateActivityButtons();
}
function recommendActivities(devices){
 const display=devices.find(d=>d.bus==='front'&&d.address===0),audio=devices.find(d=>d.address===5)||display;
 recommendations=devices.filter(d=>[1,2,3,4,6,7,8,9,10,11].includes(d.address)).map(d=>({name:((/playstation|nintendo/i.test(d.title||d.name)?'Play ':'Watch ')+(d.title||d.name||'Unknown')).slice(0,63),display:display?.name||'',source:d.name||'',audio:audio?.name||''}));
 if(autoRecommend&&recommendations.length){autoRecommend=false;for(const a of recommendations.slice(0,16))addRecommendation(a);}
 renderRecommendations();
}
$('recommendationForm').addEventListener('submit',run(async e=>{e.preventDefault();const {index,activity}=editingRecommendation;if(!activity.name)throw Error('Activity name is required.');recommendations[index]=activity;$('recommendationEditor').close();renderRecommendations();}));
$('cancelRecommendation').addEventListener('click',()=>$('recommendationEditor').close());
function deviceIcon(device){
 const sourceAddresses=[1,2,3,4,6,7,8,9,10,11];
 const role=device.type===0||device.address===0?'TV':device.type===5||device.address===5?'Audio':device.type===6?'HDMI switch':[1,3,4].includes(device.type)||sourceAddresses.includes(device.address)?'Source':'Device';
 const paths={
  TV:'M4 5h16v12H4z M8 21h8 M12 17v4 M9 2l3 3 3-3',
  Source:'M3 5h18v14H3z M10 9l5 3-5 3z',
  Audio:'M4 9h4l5-4v14l-5-4H4z M16 8q5 4 0 8 M18 5q8 7 0 14',
  'HDMI switch':'M3 9h18v6H3z M12 3v6 M6 15v6 M18 15v6',
  Device:'M6 3h12v18H6z M10 17h4'
 };
 const ns='http://www.w3.org/2000/svg',svg=document.createElementNS(ns,'svg'),path=document.createElementNS(ns,'path');
 svg.setAttribute('viewBox','0 0 24 24');svg.setAttribute('role','img');svg.setAttribute('aria-label',role);svg.classList.add('device-icon');
 path.setAttribute('d',paths[role]);path.setAttribute('fill','none');path.setAttribute('stroke','currentColor');path.setAttribute('stroke-width','1.7');path.setAttribute('stroke-linecap','round');path.setAttribute('stroke-linejoin','round');svg.append(path);return svg;
}
function renderHostname(name){
 if(!name)return;$('deviceName').textContent=name;document.title=name;
}
function renderDiagnostics(){
 const r=remoteState,connection=r.scanning?'Scanning':r.connecting?'Connecting':r.connected?'Connected':r.last_error?'Connection failed':r.waiting||r.bonded&&!r.unpairing?'Waiting for remote':'Disconnected';
 const detail=r.connected?`${r.name?' · '+r.name:''}${r.battery>=0?' · '+r.battery+'% battery':''}${r.charging===true?' · Charging':''}`:r.last_error&&!r.scanning&&!r.connecting?` (error ${r.last_error})`:'';
 const rows=[['Firmware ',String(state.version)],['Wi-Fi ',state.wifi_connected?'signal '+state.wifi_rssi+' dBm · channel '+state.wifi_channel:'disconnected'],['Remote: ',connection+detail]],diagnostics=$('diagnostics');
 if(!diagnostics.children.length)for(const [label] of rows){const term=document.createElement('dt'),value=document.createElement('dd');term.textContent=label;diagnostics.append(term,value);}
 rows.forEach(([,value],index)=>{const entry=diagnostics.children[index*2+1];if(entry.textContent!==value)entry.textContent=value;});
}
function renderRemotes(r){
 remoteState=r;const paired=!!r.bonded,unpairing=remotePending==='unpair'||!!r.unpairing,busy=!!remotePending||unpairing||(!paired&&(!!r.pairing||!!r.connecting||!!r.scanning));
 // A saved bond displays Paired outside unpairing; live connection state belongs in Diagnostics.
 // Keep this tab text-only: do not add a connection or pairing progress indicator.
 $('remoteStatus').textContent=unpairing?'Unpairing...':paired?'Paired':r.connecting?'Pairing with remote…':r.pairing&&r.too_far?'Remote detected. Bring it closer to Amity.':r.pairing||r.scanning||remotePending?'Looking for a nearby remote… Bring it close and put it in pairing mode.':r.last_error===13?'No nearby remote found. Bring it closer and try again.':r.last_error?'Pairing failed. Reset the remote’s pairing and try again.':'Not paired';renderDiagnostics();
 $('toggleRemote').setAttribute('aria-busy',String(busy));
 $('toggleRemote').textContent=unpairing?'Unpairing...':busy?(paired?'Unpair remote':'Pairing…'):paired?'Unpair remote':'Pair remote';$('toggleRemote').disabled=busy;
}
// Apple Keychain is hidden; firmware handles cleanup after the last Home is removed.
function matterConnections(fabrics){
 const names={0x1349:'Apple Home',0x6006:'Google Home',0x1217:'Amazon Alexa',0x110a:'SmartThings',0x134b:'Home Assistant'};
 return fabrics.filter(f=>f.vendor!==0x1384).map(f=>{
  let name=names[f.vendor]||f.label?.trim()||'Matter connection';
  // Do not merge distinct homes that happen to use the same platform.
  if(names[f.vendor]&&fabrics.filter(other=>other.vendor===f.vendor).length>1)name+=` (${f.label?.trim()||f.index})`;
  return {name,fabrics:[f]};
 });
}
async function removeMatterConnection(connection){
 if(matterRemovalPending||!confirm(`Remove ${connection.name}?`))return;
 matterRemovalPending=true;renderMatter(matterState);
 try{for(const f of connection.fabrics)await request('matter/remove',{index:f.index});}
 finally{matterRemovalPending=false;renderMatter(await request('matter'));}
}
function pairingStatus({error,busy,pairing,connected}){
 return error?'Pairing operation failed. Please try again.':busy?'Updating pairing…':pairing?'Ready to pair':connected?'Connected':'Not paired';
}
function renderMatter(m){
 matterState=m;
 const connections=matterConnections(m.fabrics);
 $('matterStatus').textContent=pairingStatus({error:m.error,busy:pairingPending||matterRemovalPending,pairing:m.commissioning,connected:connections.length>0});
 $('toggleMatter').textContent=m.commissioning?'Stop pairing':'Start pairing';$('toggleMatter').disabled=pairingPending||matterRemovalPending;
 $('pairingCode').textContent=m.commissioning&&m.manual_code?`Pairing code: ${m.manual_code.replace(/^(\d{4})(\d{3})(\d{4})$/,'$1-$2-$3')}`:'';
 const rows=m.commissioning?m.qr_modules:null;$('matterQr').replaceChildren();$('matterQr').hidden=!rows?.length;
 $('matterPairingHint').hidden=!m.commissioning;
 if(rows?.length){const ns='http://www.w3.org/2000/svg',svg=document.createElementNS(ns,'svg'),background=document.createElementNS(ns,'rect'),path=document.createElementNS(ns,'path');
  svg.setAttribute('viewBox',`-4 -4 ${rows.length+8} ${rows.length+8}`);svg.setAttribute('role','img');svg.setAttribute('aria-label','Matter pairing QR code');svg.setAttribute('shape-rendering','crispEdges');
  background.setAttribute('x','-4');background.setAttribute('y','-4');background.setAttribute('width',rows.length+8);background.setAttribute('height',rows.length+8);background.setAttribute('fill','white');
  let pixels='';rows.forEach((row,y)=>{for(let x=0;x<row.length;x++)if(row[x]==='1')pixels+=`M${x},${y}h1v1h-1z`;});path.setAttribute('d',pixels);path.setAttribute('fill','black');svg.append(background,path);$('matterQr').append(svg);
 }
 $('fabricList').replaceChildren();for(const connection of connections){const remove=button(`Remove ${connection.name}`,()=>removeMatterConnection(connection));remove.disabled=matterRemovalPending;$('fabricList').append(remove);}
}
function renderHomekit(s){
 homekitState=s;
 $('homekitStatus').textContent=pairingStatus({error:s.error,busy:s.busy||homekitPending,pairing:s.pairing,connected:s.paired});
 $('toggleHomekit').textContent=s.pairing?'Stop pairing':'Start pairing';
 $('toggleHomekit').disabled=homekitPending||s.busy||!s.available||s.paired;
 $('toggleHomekit').hidden=!!s.paired;
 $('removeHomekit').hidden=!s.paired;$('removeHomekit').disabled=homekitPending||s.busy;
 $('homekitPairingCode').textContent=s.pairing&&s.manual_code?`Pairing code: ${s.manual_code}`:'';
 const rows=s.pairing?s.qr_modules:null,container=$('homekitQr');container.replaceChildren();container.hidden=!rows?.length;
 const prerequisite=!s.available&&!s.paired;
 $('homekitPairingHint').textContent=prerequisite?'Connect Amity to Wi-Fi through Matter before pairing.':'In your home app, scan this QR code or enter the pairing code. Pairing closes after five minutes.';
 $('homekitPairingHint').hidden=!s.pairing&&!prerequisite;
 if(rows?.length){
  const ns='http://www.w3.org/2000/svg',svg=document.createElementNS(ns,'svg'),background=document.createElementNS(ns,'rect'),path=document.createElementNS(ns,'path');
  svg.setAttribute('viewBox',`-4 -4 ${rows.length+8} ${rows.length+8}`);svg.setAttribute('role','img');svg.setAttribute('aria-label','HomeKit pairing QR code');svg.setAttribute('shape-rendering','crispEdges');
  background.setAttribute('x','-4');background.setAttribute('y','-4');background.setAttribute('width',rows.length+8);background.setAttribute('height',rows.length+8);background.setAttribute('fill','white');
  let pixels='';rows.forEach((row,y)=>{for(let x=0;x<row.length;x++)if(row[x]==='1')pixels+=`M${x},${y}h1v1h-1z`;});path.setAttribute('d',pixels);path.setAttribute('fill','black');svg.append(background,path);container.append(svg);
 }
}
async function refreshHomekit(){
 if(!token||homekitPolling||firmwareUpdating)return;
 homekitPolling=true;try{const state=await request('homekit');if(token&&!firmwareUpdating)renderHomekit(state);}finally{homekitPolling=false;}
}
async function refresh(){if(!token||polling||firmwareUpdating||hostnameReconnect)return;polling=true;try{
 const latest=await request('status');if(firmwareUpdating||hostnameReconnect)return;
 state=latest;if(state.activity_error)message(state.activity_error,true);renderHostname(state.hostname);updateActivityButtons();$('standby').hidden=!(state.current_activity>=0);$('connection').textContent=state.discovering?'Scanning HDMI…':'';
 renderDiagnostics();
 renderDeviceNames();
 if(scanAfter!==null){
  const complete=state.discovery_generation!==scanAfter&&!state.discovering;
  $('discoveryStatus').textContent=complete?(state.devices.length?'':'No HDMI devices found.'):'Scanning HDMI…';
  $('deviceHeading').hidden=!complete||!state.devices.length;$('deviceList').replaceChildren();
  if(complete&&scanGeneration!==state.discovery_generation){scanGeneration=state.discovery_generation;recommendActivities(state.devices);}if(!complete)$('recommendations').hidden=true;
  if(complete)for(const d of [...state.devices].sort((a,b)=>(a.name||'').localeCompare(b.name||''))){const row=document.createElement('div');row.className='device';const name=document.createElement('span');name.textContent=d.title||d.name||'Unknown';row.append(deviceIcon(d),name);if(d.location){const location=document.createElement('span');location.className='device-location';location.textContent=d.location;row.append(location);}$('deviceList').append(row);}
 }
 renderRemotes(await request('remotes'));
 renderMatter(await request('matter'));
 if(!$('homekit').hidden)await refreshHomekit();
 if(!$('amityDevices').hidden)await window.amityInterface?.refresh();
 }finally{polling=false;updateActivityButtons();}}
async function enter(){const response=await request('config',undefined,true);config=await response.json();autoRecommend=response.headers.get('X-Amity-Configured')==='false';rememberSaved();markDirty();scanPending=false;scanAfter=null;scanGeneration=null;recommendations=[];renderRecommendations();$('devices').hidden=true;$('onboarding').hidden=true;$('login').hidden=true;$('admin').hidden=false;$('logout').hidden=false;selectTab('activities');renderActivities();await refresh();await window.amityInterface?.open();}
async function showWelcome(){
 if(firmwareUpdating||hostnameReconnect)return;
 const setup=await request('setup');if(hostnameReconnect)return;renderHostname(setup.hostname);showLogin();
 if(setup.required){$('login').hidden=true;$('onboarding').hidden=false;}
}
$('setupForm').addEventListener('submit',run(async e=>{
 e.preventDefault();const password=$('setupPassword').value;
 if(password!==$('confirmPassword').value)throw Error('Passwords do not match.');
 if(new TextEncoder().encode(password).length>128)throw Error('Password must be no more than 128 bytes.');
 const submit=e.target.querySelector('button');submit.disabled=true;
 try{
  await request('setup',{password});$('setupForm').reset();showLogin();
  message('Password set.');$('password').focus();
 }catch(error){if(error.status===409){$('setupForm').reset();await showWelcome();}throw error;}
 finally{submit.disabled=false;}
}));
$('loginForm').addEventListener('submit',run(async e=>{
 e.preventDefault();const submit=e.target.querySelector('button');if(submit.disabled)return;submit.disabled=true;
 try{const r=await request('login',{password:$('password').value});token=r.token;sessionStorage.setItem('amityToken',token);$('password').value='';message('');
  if(firmwareUpdating&&firmwareUpdateRecord?.authRequired){firmwareUpdateRecord.authRequired=false;firmwareUpdateRecord.startedAt=Date.now();$('login').hidden=true;saveFirmwareUpdate();await waitForFirmware();}
  else await enter();
 }
 catch(error){if(firmwareUpdating&&firmwareUpdateRecord?.authRequired)firmwareWaitMessage(error.message||'Could not log in. Try again.');else reportError(error);if(error.retryAfter){submit.textContent=`Try again in ${error.retryAfter}s`;await new Promise(resolve=>setTimeout(resolve,error.retryAfter*1000));}}
 finally{submit.disabled=false;submit.textContent='Log in';}
}));
$('logout').addEventListener('click',run(async()=>{await request('logout',{});showLogin();}));
$('addActivity').addEventListener('click',()=>openActivityEditor());
$('saveActivities').addEventListener('click',run(async()=>{if(!activityLocked())await persistActivities();}));
$('discardActivities').addEventListener('click',()=>{if(activityLocked())return;config.activities=structuredClone(savedActivities);rememberSaved();renderActivities();message('');});
$('standby').addEventListener('click',run(async()=>{await request('activity',{index:-1});message('');await refresh();}));
$('discover').addEventListener('click',run(async()=>{
 if(scanPending||state.discovering)return;scanPending=true;updateActivityButtons();
 try{const before=await request('status');await request('discover',{});scanAfter=before.discovery_generation;$('deviceList').replaceChildren();recommendations=[];renderRecommendations();$('deviceHeading').hidden=true;$('discoveryStatus').textContent='Scanning HDMI…';$('devices').hidden=false;await refresh();}
 finally{scanPending=false;updateActivityButtons();}
}));
$('toggleMatter').addEventListener('click',run(async()=>{
 if(pairingPending||matterRemovalPending)return;pairingPending=true;renderMatter(matterState);
 try{const current=await request('matter');await request(current.commissioning?'matter/close':'matter/open',{});renderMatter(await request('matter'));}
 finally{pairingPending=false;renderMatter(matterState);}
}));
$('toggleHomekit').addEventListener('click',run(async()=>{
 if(homekitPending||homekitState.busy)return;
 homekitPending=true;renderHomekit(homekitState);
 try{const current=await request('homekit');await request(current.pairing?'homekit/close':'homekit/open',{});renderHomekit(await request('homekit'));}
 finally{homekitPending=false;renderHomekit(homekitState);}
}));
$('removeHomekit').addEventListener('click',run(async()=>{
 if(homekitPending||homekitState.busy||!confirm('Remove Apple Home?'))return;
 homekitPending=true;renderHomekit(homekitState);
 try{await request('homekit/remove',{confirm:true});renderHomekit(await request('homekit'));}
 finally{homekitPending=false;renderHomekit(homekitState);}
}));
$('toggleRemote').addEventListener('click',run(async()=>{
 if(remotePending)return;remotePending=true;renderRemotes(remoteState);
 try{
  const current=await request('remotes');renderRemotes(current);
  if(current.unpairing)return;
  const approval=current.remote_generation===undefined?{}:{request_id:crypto.randomUUID().replaceAll('-',''),expected_boot_id:current.boot_id,expected_remote_generation:current.remote_generation};
  if(current.bonded||current.connected){if(!confirm('Unpair the remote?'))return;remotePending='unpair';renderRemotes(current);await request('remotes/unpair',approval);}
  else if(!current.scanning&&!current.connecting&&!current.pairing)await request('remotes/pair',approval);
  renderRemotes(await request('remotes'));
 }finally{remotePending=false;renderRemotes(remoteState);}
}));
async function download(path,name){const r=await request(path,undefined,true),url=URL.createObjectURL(await r.blob());const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
$('backup').addEventListener('click',run(()=>download('backup','amity-config.json')));$('logs').addEventListener('click',run(()=>download('logs','amity.log')));
$('restore').addEventListener('change',run(async e=>{
 const file=e.target.files[0];if(!file)return;
 try{
  if(file.size>16384)throw Error('Backup must be no more than 16 KiB.');
  const contents=await file.text();
  if(!confirm('Replace saved configuration with this backup? If replacing a failed main Amity, confirm that the old main is powered off and disconnected.'))return;
  const data={configuration:JSON.parse(contents),expected_boot_id:(await request('status')).boot_id};
  const result=await request('restore',data),warning=result.warning||(result.unresolved||[]).join(' ');await enter();message(warning||'Configuration restored.',!!warning);
 }finally{e.target.value='';}
}));
// Match firmware_version.hpp: semantic precedence, ignoring build metadata.
function compareFirmwareVersions(candidate,running){
 const parse=s=>{
  const m=/^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$/.exec(s);
  if(!m||m[0]!==s)return null;
  const pre=m[4]?.split('.')||[];
  if(pre.some(p=>/^[0-9]+$/.test(p)&&p.length>1&&p[0]==='0'))return null;
  return {core:m.slice(1,4),pre};
 };
 const a=parse(candidate),b=parse(running);if(!a||!b)return null;
 const cmp=(x,y,numeric)=>numeric&&x.length!==y.length?(x.length<y.length?-1:1):x===y?0:x<y?-1:1;
 for(let i=0;i<3;i++){const c=cmp(a.core[i],b.core[i],true);if(c)return c;}
 if(!a.pre.length||!b.pre.length)return !a.pre.length===!b.pre.length?0:!a.pre.length?1:-1;
 for(let i=0;i<Math.min(a.pre.length,b.pre.length);i++){
  const an=/^[0-9]+$/.test(a.pre[i]),bn=/^[0-9]+$/.test(b.pre[i]);
  if(an!==bn)return an?-1:1;const c=cmp(a.pre[i],b.pre[i],an);if(c)return c;
 }
 return a.pre.length===b.pre.length?0:a.pre.length<b.pre.length?-1:1;
}
async function firmwareVersion(file){
 return (await firmwareImage(file)).version;
}
async function firmwareImage(file){
 // A release container keeps the same choose-one-file interface and carries
 // exact compatibility metadata. USB/debug images retain their native header.
 let offset=0;
 const header=new Uint8Array(await file.slice(0,32).arrayBuffer());
 if(header.length===32&&new TextDecoder().decode(header.slice(0,16))==='AMITY-UPDATE-V1\n'){
  const view=new DataView(header.buffer),manifest=view.getUint32(16),signature=view.getUint32(20),image=view.getBigUint64(24);
  if(manifest<1||manifest>8192||![0,64].includes(signature)||image<288n||image>0x3cd000n)throw Error('Invalid Amity update file.');
  offset=32+manifest+signature;
  if(BigInt(file.size)!==BigInt(offset)+image)throw Error('Incomplete Amity update file.');
 }
 const bytes=new Uint8Array(await file.slice(offset,offset+288).arrayBuffer()),view=new DataView(bytes.buffer);
 if(bytes.length<288||bytes[0]!==0xe9||view.getUint32(32,true)!==0xabcd5432)throw Error('Select an Amity application firmware binary.');
 const field=bytes.slice(48,80),end=field.indexOf(0);
 if(end<0)throw Error('Invalid firmware version field.');
 return {version:new TextDecoder().decode(field.slice(0,end)),imageIdentity:Array.from(bytes.slice(176,208),b=>b.toString(16).padStart(2,'0')).join(''),release:offset>0};
}

function saveFirmwareUpdate(){sessionStorage.setItem(firmwareUpdateKey,JSON.stringify(firmwareUpdateRecord));}
function validFirmwareHealth(health){return !!health&&typeof health.boot_id==='string'&&health.boot_id.length>0&&typeof health.version==='string'&&health.version.length>0&&typeof health.ready==='boolean';}
function showFirmwareUpdate(record,text){
 firmwareUpdating=true;firmwareUpdateRecord=record;saveFirmwareUpdate();message('');
 $('admin').hidden=true;$('login').hidden=true;$('onboarding').hidden=true;$('logout').hidden=true;$('connection').textContent='';
 $('firmwareUpdate').hidden=false;$('firmwareUpdateStatus').textContent=text;
 $('retryFirmwareWait').hidden=true;$('firmwareUpdateLogin').hidden=true;$('uploadProgress').hidden=false;
 $('firmwareUpdateTargets').replaceChildren();
 for(const target of record.targets||[])renderFirmwareTarget(target,record);
}
function firmwareTargetText(target,bytes){
 const offset=Number(target.offset),size=Number(bytes),percent=Number.isFinite(offset)&&Number.isFinite(size)&&size>0?Math.max(0,Math.min(100,Math.floor(100*offset/size))):0;
 switch(target.phase){
 case 'uploading':case 'receiving':return `Uploading: ${percent}%`;
 case 'staged':return 'Ready to restart';
 case 'skipped':return 'Could not update now; will retry when available';
 case 'failed':return 'Firmware update failed';
 case 'activating':case 'boot_checking':return 'Restarting';
 case 'complete':return 'Updated';
 default:return 'Waiting';
 }
}
function renderFirmwareTarget(target,record){
 const item=document.createElement('li'),name=document.createElement('strong');
 name.textContent=record.deviceNames?.[target.octopus_id]||'Amity device';
 item.append(name,document.createTextNode(' — '+firmwareTargetText(target,record.imageBytes)));
 $('firmwareUpdateTargets').append(item);
}
function renderFirmwareDistribution(job,record){
 if(job.last_failed_upload?.image_elf_sha256===record.imageIdentity)job=job.last_failed_upload;
 if(job.automatic||job.image_elf_sha256!==record.imageIdentity)return false;
 record.targets=Array.isArray(job.targets)?job.targets:[];record.imageBytes=job.image_bytes;
 $('firmwareUpdateTargets').replaceChildren();for(const target of record.targets)renderFirmwareTarget(target,record);
 if(job.phase==='distributing'||job.phase==='uploading'){
  record.updatePhase='distributing';
  $('firmwareUpdateStatus').textContent='Uploading firmware to your Amity devices. They will restart together when the upload is complete.';
  const total=Number(job.image_bytes)*record.targets.length;
  const sent=record.targets.reduce((sum,target)=>sum+(['staged','activating','complete','skipped','failed'].includes(target.phase)?Number(job.image_bytes):Math.max(0,Math.min(Number(job.image_bytes),Number(target.offset)||0))),0);
  if(total>0)$('uploadProgress').value=100*sent/total;else $('uploadProgress').removeAttribute('value');
 }else if(job.phase==='activating'||job.phase==='complete'){
  if(record.updatePhase!=='activating'){record.startedAt=Date.now();record.updatePhase='activating';}
  firmwareWaitMessage('Firmware is ready. Restarting your Amity devices…');
 }else if(job.phase==='failed'){
  firmwareWaitMessage(customerText(job.error||'The firmware update could not complete. Upload the update again to retry.'));
  $('uploadProgress').hidden=true;$('firmwareUpdateLogin').hidden=false;saveFirmwareUpdate();return true;
 }
 saveFirmwareUpdate();return false;
}
function firmwareWaitMessage(text){$('firmwareUpdateStatus').textContent=text;$('uploadProgress').removeAttribute('value');}
function requireFirmwareLogin(record){
 token='';sessionStorage.removeItem('amityToken');record.authRequired=true;saveFirmwareUpdate();
 firmwareWaitMessage('Your session ended. Log in to view update progress. The update continues on Amity.');
 $('login').hidden=false;$('retryFirmwareWait').hidden=true;$('firmwareUpdateLogin').hidden=true;$('password').focus();
}
function finishFirmwareUpdate(text){
 sessionStorage.removeItem(firmwareUpdateKey);sessionStorage.setItem(firmwareResultKey,text);
 token='';sessionStorage.removeItem('amityToken');
 dirty=false;location.reload();
}
async function waitForFirmware(){
 if(firmwareWaitRunning)return;firmwareWaitRunning=true;
 const record=firmwareUpdateRecord;record.phase='waiting';saveFirmwareUpdate();
 firmwareWaitMessage(record.uncertain?'Upload connection interrupted. Checking the update…':'Firmware uploaded. Preparing your Amity devices…');
 let lastHealth=null;
 try{
  while(Date.now()-record.startedAt<(record.updatePhase==='activating'?120000:2700000)){
   const abort=new AbortController(),timeout=setTimeout(()=>abort.abort(),5000);
   try{
    const response=await fetch('/api/setup',{cache:'no-store',signal:abort.signal});
    if(!response.ok)throw Error('Not ready');
    const health=await response.json();if(!validFirmwareHealth(health))throw Error('Invalid boot status');lastHealth=health;
    if(health.boot_id!==record.previousBootId){
     if(health.ready){
      if(health.version===record.version&&health.image_elf_sha256===record.imageIdentity){
       const skipped=(record.targets||[]).filter(target=>target.phase==='skipped'||target.phase==='failed').length;
       return finishFirmwareUpdate(`Firmware update complete. Amity is running version ${health.version}. ${skipped?'Unavailable devices will update automatically when they reconnect. ':''}Please log in.`);
      }
      if(health.version===record.version){firmwareWaitMessage('The restarted Amity did not report the expected firmware image. Upload the update again to retry.');$('uploadProgress').hidden=true;$('firmwareUpdateLogin').hidden=false;return;}
      firmwareWaitMessage(`Update did not complete. Amity is running version ${health.version} instead of ${record.version}.`);
      $('uploadProgress').hidden=true;$('firmwareUpdateLogin').hidden=false;return;
     }
     firmwareWaitMessage('Amity is starting. Waiting for it to be ready…');
    }else if(token){
     try{
      const status=await fetch('/api/updates/cohort',{cache:'no-store',headers:{Authorization:'Bearer '+token},signal:abort.signal});
      if(status.status===401){requireFirmwareLogin(record);return;}
      if(status.ok&&renderFirmwareDistribution(await status.json(),record))return;
     }catch{}
    }else{
     requireFirmwareLogin(record);return;
    }
   }catch{
    lastHealth=null;
    firmwareWaitMessage('Waiting for Amity to reconnect… Keep this page open.');
   }finally{clearTimeout(timeout);}
   await new Promise(resolve=>setTimeout(resolve,2000));
  }
  firmwareWaitMessage('Could not confirm the update. Check Amity’s power and Wi-Fi connection, then try reconnecting.');
  $('uploadProgress').hidden=true;$('retryFirmwareWait').hidden=false;
  $('firmwareUpdateLogin').hidden=lastHealth?.ready!==true;
 }finally{firmwareWaitRunning=false;}
}
$('retryFirmwareWait').addEventListener('click',run(async()=>{
 if(firmwareWaitRunning)return;
 firmwareUpdateRecord.startedAt=Date.now();showFirmwareUpdate(firmwareUpdateRecord,'Reconnecting to Amity…');await waitForFirmware();
}));
$('firmwareUpdateLogin').addEventListener('click',()=>finishFirmwareUpdate($('firmwareUpdateStatus').textContent));
$('firmware').addEventListener('change',run(async e=>{
 const input=e.target,file=input.files[0];if(!file)return;
 const progress=$('uploadProgress');input.disabled=true;progress.hidden=true;progress.value=0;
 try{
  const image=await firmwareImage(file),version=image.version,current=(await request('status')).version,comparison=compareFirmwareVersions(version,current);
  const warning=comparison===null?
   `The selected firmware version (${version||'unknown'}) cannot be compared with the running version (${current}). It may be a downgrade. Downgrading is not supported. Continue at your own risk?`:
   comparison<0?`You are installing version ${version} over version ${current}. Downgrading is not supported. Continue at your own risk?`:
   `Install firmware ${version} on all Amity devices and restart them together? Current version: ${current}.`;
  if(!confirm(warning))return;
  const health=await request('setup');
  if(!validFirmwareHealth(health))throw Error('Could not read Amity’s boot status. Reload and try again.');
  if(health.update_manifest_required&&!image.release)throw Error('Choose amity-update.bin, which includes the release information.');
  const system=window.amityInterface?.enabled?await request('system'):null,deviceNames={};
  if(system?.octopus_id)deviceNames[system.octopus_id]=system.hostname||'Main Amity';
  for(const device of [...(system?.membership?.peers||[]),...(system?.named_devices||[])])if(device.hostname)deviceNames[device.octopus_id]=device.hostname;
  const record={version,imageIdentity:image.imageIdentity,deviceNames,previousBootId:health.boot_id,startedAt:Date.now(),phase:'uploading',updatePhase:'distributing'};
  showFirmwareUpdate(record,'Uploading firmware… Keep Amity powered on.');progress.value=0;
  const result=await new Promise((resolve,reject)=>{
   const xhr=new XMLHttpRequest();xhr.open('POST','/api/firmware');xhr.setRequestHeader('Authorization','Bearer '+token);xhr.setRequestHeader('Content-Type','application/octet-stream');
   if(comparison===null||comparison<0)xhr.setRequestHeader('X-Amity-Confirm-Version',current);
   xhr.upload.onprogress=p=>{if(p.lengthComputable){progress.value=p.loaded/p.total*100;$('firmwareUpdateStatus').textContent=`Uploading firmware… ${Math.round(progress.value)}%. Keep Amity powered on.`;}};
   xhr.onload=()=>{if(xhr.status===200)return resolve({});let error=xhr.responseText;try{error=JSON.parse(error).error||error;}catch{}reject(Object.assign(Error(error||'Firmware upload failed.'),{status:xhr.status}));};
   xhr.onerror=xhr.ontimeout=()=>resolve({uncertain:true});xhr.timeout=120000;xhr.send(file);
  });
  record.uncertain=!!result.uncertain;record.startedAt=Date.now();await waitForFirmware();
 }catch(error){
  if(firmwareUpdating){firmwareUpdating=false;sessionStorage.removeItem(firmwareUpdateKey);$('firmwareUpdate').hidden=true;$('admin').hidden=false;$('logout').hidden=false;}
  if(error.status===401)await showWelcome();throw error;
 }finally{input.disabled=false;input.value='';if(!firmwareUpdating)progress.hidden=true;}
}));
$('reboot').addEventListener('click',run(async()=>{if(confirm('Restart Amity?')){await request('reboot',{});showLogin();message('Restarting Amity.');}}));
function stopHostnameReconnect(){
 const flow=hostnameReconnect;hostnameReconnect=null;
 if(!flow)return;
 clearTimeout(flow.timer);clearTimeout(flow.timeout);flow.abort?.abort();
}
function hostnameReady(health,flow){
 return !!health&&health.device_id===flow.deviceId&&health.hostname===flow.hostname&&
  health.boot_id===flow.bootId&&health.ready===true;
}
async function probeHostnameReconnect(flow){
 if(hostnameReconnect!==flow)return;
 if(Date.now()-flow.startedAt>=120000){
  message('Still reconnecting. Check Amity’s power and Wi-Fi, or open the new address below if your browser requires certificate approval.');
 }
 const abort=new AbortController();flow.abort=abort;
 flow.timeout=setTimeout(()=>abort.abort(),2000);
 try{
  // This is public health only: never send the old origin's login token or
  // cookies to another hostname, or accept an opaque/redirected response.
  const response=await fetch(flow.origin+'/api/ready',{mode:'cors',credentials:'omit',referrerPolicy:'no-referrer',cache:'no-store',redirect:'error',signal:abort.signal});
  if(response.ok&&response.type!=='opaque'){
   const health=await response.json();
   if(hostnameReconnect===flow&&hostnameReady(health,flow)){
    stopHostnameReconnect();message('Amity is ready. Opening its new address…');location.replace(flow.origin);return;
   }
  }
 }catch{
  // Browsers report DNS, offline, CORS and certificate failures alike. Keep
  // checking and leave the new address available for certificate approval.
 }finally{
  clearTimeout(flow.timeout);flow.timeout=null;
  if(flow.abort===abort)flow.abort=null;
 }
 if(hostnameReconnect===flow)flow.timer=setTimeout(()=>{flow.timer=null;probeHostnameReconnect(flow);},2000);
}
function startHostnameReconnect({hostname,bootId,deviceId}){
 stopHostnameReconnect();
 if(typeof hostname!=='string'||!/^[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$/.test(hostname)||
    typeof bootId!=='string'||!bootId||typeof deviceId!=='string'||!deviceId)
  throw Error('Could not read Amity’s reconnect details. Open its new address to reconnect.');
 const flow={origin:`https://${hostname}.local`,hostname,bootId,deviceId,startedAt:Date.now(),timer:null,timeout:null,abort:null};
 hostnameReconnect=flow;renderHostname(hostname);showLogin();$('login').hidden=true;
 const link=$('hostnameLink');link.href=flow.origin;link.textContent=`Open ${hostname}.local`;link.hidden=false;
 message(`Reconnecting to ${hostname}.local… If your browser requires certificate approval, open the link below.`);
 probeHostnameReconnect(flow);
}
window.addEventListener('pagehide',stopHostnameReconnect);
$('factoryReset').addEventListener('click',run(async()=>{
 if(!confirm('This will factory reset Amity. You will need to reconfigure it again. Are you sure?'))return;
 const button=$('factoryReset');button.disabled=true;
 try{await request('factory-reset',{confirm:true});dirty=false;showLogin();$('login').hidden=true;message('Factory reset started. Add the device to your home again using its original Matter code.');}
 finally{button.disabled=false;}
}));
$('changePassword').addEventListener('submit',run(async e=>{e.preventDefault();await request('password',{password:$('newPassword').value});$('newPassword').value='';showLogin();message('Password changed.');}));
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
setInterval(()=>{if(token)refresh().catch(reportError);},4000);
async function initialize(){
 let pending;try{pending=JSON.parse(sessionStorage.getItem(firmwareUpdateKey));}catch{sessionStorage.removeItem(firmwareUpdateKey);}
 if(pending&&typeof pending.version==='string'&&typeof pending.previousBootId==='string'&&pending.previousBootId.length>0&&Number.isFinite(pending.startedAt)){
  if(pending.phase==='uploading')pending.uncertain=true;
  showFirmwareUpdate(pending,'Waiting for the firmware update…');await waitForFirmware();return;
 }
 const setup=await request('setup');renderHostname(setup.hostname);if(setup.required){showLogin();$('login').hidden=true;$('onboarding').hidden=false;}else if(token)await enter();else showLogin();
 const result=sessionStorage.getItem(firmwareResultKey);if(result){sessionStorage.removeItem(firmwareResultKey);message(result);}
}
initialize().catch(e=>{if(e.silent)return;$('connection').textContent='';message(e.message+' Reload to try again.',true);});
