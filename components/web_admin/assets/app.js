'use strict';
const $=id=>document.getElementById(id);
let token=sessionStorage.getItem('amityToken')||'',config={version:1,activities:[]},state={},polling=false,dirty=false,saving=false,persistedActivities='[]',savedActivities=[],scanPending=false,scanAfter=null,scanGeneration=null,recommendations=[],autoRecommend=false,editingRecommendation=null,pairingPending=false,matterRemovalPending=false,remotePending=false,remoteState={devices:[]},savedActivityEntries=new Map(),editingActivity=null,activityActionPending=false;
let firmwareUpdating=false,firmwareWaitRunning=false,firmwareUpdateRecord=null;
let homekitPending=false,homekitPolling=false,homekitState={};
let matterState={fabrics:[]};
const firmwareUpdateKey='amityFirmwareUpdate',firmwareResultKey='amityFirmwareUpdateResult';
function message(text,error=false){$('message').textContent=text;$('message').classList.toggle('error',error);}
function activitySnapshot(activities){return JSON.stringify(activities.map(a=>[a.name??'',a.display??'',a.source??'',a.audio??'',a.switch?.device??'',a.switch?.device?(a.switch.input??0):null]));}
function updateActivityButtons(){
 const locked=activityLocked();
 document.querySelectorAll('#activities button, #recommendationEditor button').forEach(b=>{
  b.disabled=locked||(b.hasAttribute('data-add-recommendation')&&config.activities.length>=16)||(b.id==='addActivity'&&config.activities.length>=16);
 });
 if(locked)endActivityDrag(true);updateActivityState();
}
function markDirty(){dirty=activitySnapshot(config.activities)!==persistedActivities;$('saveActivities').hidden=!dirty;$('discardActivities').hidden=!dirty;if(dirty)autoRecommend=false;renderRecommendations();}
function reportError(error){if(!firmwareUpdating&&!error.silent)message(error.message,true);}
function showLogin(){if(firmwareUpdating)return;homekitPending=false;renderHomekit({});if($('activityEditor').open)$('activityEditor').close();if($('recommendationEditor').open)$('recommendationEditor').close();editingActivity=null;token='';sessionStorage.removeItem('amityToken');$('onboarding').hidden=true;$('connection').textContent='';$('login').hidden=false;$('admin').hidden=true;$('logout').hidden=true;}
async function request(path,data,raw=false){
 const headers={Authorization:`Bearer ${token}`};if(data!==undefined)headers['Content-Type']='application/json';
 const response=await fetch('/api/'+path,{method:data===undefined?'GET':'POST',headers,body:data===undefined?undefined:typeof data==='string'?data:JSON.stringify(data)});
 if(response.status===401&&path!=='login'){if(!firmwareUpdating){await showWelcome();message('');}throw Object.assign(Error(),{silent:true});}
 if(!response.ok){let e=await response.json().catch(()=>({error:'Request failed'}));const error=Error(e.error);error.status=response.status;error.retryAfter=Math.max(0,Math.min(60,Number(response.headers.get('Retry-After'))||0));throw error;}
 return raw?response:response.json();
}
function run(action){return async event=>{try{await action(event);}catch(e){reportError(e);}};}
function button(text,handler){const b=document.createElement('button');b.textContent=text;b.addEventListener('click',run(handler));return b;}
function label(text,value,onChange,list){const l=document.createElement('label');l.textContent=text;const i=document.createElement('input');i.value=value??'';i.maxLength=63;if(list)i.setAttribute('list',list);i.addEventListener('input',()=>onChange(i.value));l.append(i);return l;}
function selectTab(id){document.querySelectorAll('.tab').forEach(s=>s.hidden=s.id!==id);document.querySelectorAll('[data-tab]').forEach(b=>b.classList.toggle('selected',b.dataset.tab===id));if(id==='homekit')refreshHomekit().catch(reportError);}
document.querySelectorAll('[data-tab]').forEach(b=>b.addEventListener('click',()=>selectTab(b.dataset.tab)));
function activityFields(a,changed=markDirty){
 const field=(text,value,set,list)=>label(text,value,v=>{set(v);changed();},list);
 const fields=document.createElement('div');fields.className='fields';fields.append(field('Activity name',a.name,v=>a.name=v),...['display','source','audio'].map(k=>field(k[0].toUpperCase()+k.slice(1),a[k],v=>a[k]=v,k+'DeviceNames')));
 fields.append(field('Switch device (optional)',a.switch?.device,v=>{if(v)a.switch={device:v,input:a.switch?.input||0};else delete a.switch;const control=input.querySelector('input');control.disabled=!a.switch;control.value=a.switch?.input??'';},'switchDeviceNames'));
 const input=field('Switch input',a.switch?.input??'',v=>{if(a.switch)a.switch.input=Number(v);});input.querySelector('input').type='number';input.querySelector('input').min=0;input.querySelector('input').max=255;input.querySelector('input').dataset.switchInput='';input.querySelector('input').disabled=!a.switch;fields.append(input);
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
 for(const [role,values] of Object.entries(names)){
  const list=$(role+'DeviceNames');list.replaceChildren();
  for(const name of [...values].sort((a,b)=>a.localeCompare(b))){const option=document.createElement('option');option.value=name;list.append(option);}
 }
}
function rememberSaved(activities=config.activities){
 savedActivityEntries=new Map(activities.map((a,index)=>[a,{index,snapshot:activitySnapshot([a])}]));
 savedActivities=structuredClone(activities);persistedActivities=activitySnapshot(activities);
 renderDeviceNames();
}
function activityActive(activity){const saved=savedEntry(activity);return !!saved&&saved.index===state.current_activity;}
function activityLocked(){return activitiesScanning()||saving||activityActionPending;}
function activitiesScanning(){return !!(scanPending||state.discovering||(scanAfter!==null&&state.discovery_generation===scanAfter));}
function updateActivityState(){
 document.querySelectorAll('#activityList article').forEach((row,index)=>{
  const a=config.activities[index],start=row.querySelector('[data-start]');
  start.textContent=activityActive(a)?'Repair':'Start';
  start.disabled=activityLocked()||!matchesSaved(a);
  start.title=matchesSaved(a)?'':'Save activity changes before starting.';
  row.classList.toggle('active',activityActive(a));
  row.querySelector('[data-drag]').setAttribute('aria-disabled',String(activityLocked()));
 });
 updateActivityEditor();
}
function validateActivity(a,others){
 if(!a.name)throw Error('Activity name is required.');
 if(others.some(other=>other.name===a.name))throw Error('Activity names must be unique.');
 for(const value of [a.name,a.display,a.source,a.audio,a.switch?.device])
  if(value&&(new TextEncoder().encode(value).length>63||/[\x00-\x1f]/.test(value)))throw Error('Activity fields must be at most 63 bytes and contain no control characters.');
 if(a.switch&&(!Number.isInteger(a.switch.input)||a.switch.input<0||a.switch.input>255))throw Error('Switch input must be between 0 and 255.');
}
function openActivityEditor(target=null){
 if(activityLocked())return;
 if(!target&&config.activities.length>=16)return message('Up to 16 activities are supported.',true);
 editingActivity={target,activity:structuredClone(target||{name:'New activity',display:'',source:'',audio:''})};
 $('activityEditorTitle').textContent=target?'Edit activity':'Add activity';$('activityEditorError').textContent='';
 $('activityFields').replaceChildren(activityFields(editingActivity.activity,()=>{$('activityEditorError').textContent='';updateActivityEditor();}));
 updateActivityEditor();$('activityEditor').showModal();$('activityFields').querySelector('input').focus();
}
function updateActivityEditor(){
 if(!editingActivity)return;
 const {target,activity}=editingActivity,needsSave=!matchesSaved(activity,target),active=activityActive(target);
 $('editorStart').textContent=needsSave?(active?'Save and repair':'Save and start'):(active?'Repair':'Start');
 $('editorSaveHint').hidden=!needsSave;
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
  await request('config',submitted);const saved=JSON.parse(submitted).activities;
  if(editor){
   if(editor.target){Object.keys(target).forEach(key=>delete target[key]);Object.assign(target,structuredClone(editor.activity));}
   else {config.activities.push(target);editor.target=target;}
  }
  savedActivityEntries=new Map(references.map((a,index)=>[a,{index,snapshot:activitySnapshot([saved[index]])}]));
  savedActivities=structuredClone(saved);persistedActivities=activitySnapshot(saved);renderDeviceNames();renderActivities();message('Activities saved.');await refresh();
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
  const title=document.createElement('span');title.className='activity-name';title.textContent=a.name;
  const controls=document.createElement('div');controls.className='activity-controls';
  const start=button('Start',()=>startActivity(a));start.dataset.start=index;
  controls.append(button('Edit',()=>openActivityEditor(a)),button('Delete',()=>{if(activityLocked())return;config.activities.splice(index,1);renderActivities();}),start);
  row.append(dragHandle(index,a),title,controls);list.append(row);
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
 recommendations=devices.filter(d=>[1,2,3,4,6,7,8,9,10,11].includes(d.address)).map(d=>({name:((/playstation|nintendo/i.test(d.name)?'Play ':'Watch ')+(d.name||'Unknown')).slice(0,63),display:display?.name||'',source:d.name||'',audio:audio?.name||''}));
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
 if(document.activeElement!==$('hostname')&&!$('hostname').dataset.edited)$('hostname').value=name;
}
function renderFrontPhysicalAddress(address){
 const input=$('frontPhysicalAddress');
 if(document.activeElement!==input&&!input.dataset.edited)input.value=address||'1.0.0.0';
}
function renderDiagnostics(){
 const r=remoteState,connection=r.scanning?'Scanning':r.connecting?'Connecting':r.connected?'Connected':r.last_error?'Connection failed':'Disconnected';
 const detail=r.connected?`${r.name?' · '+r.name:''}${r.battery>=0?' · '+r.battery+'% battery':''}${r.charging===true?' · Charging':''}`:r.last_error&&!r.scanning&&!r.connecting?` (error ${r.last_error})`:'';
 $('diagnostics').textContent=`Firmware ${state.version}\n${state.wifi_connected?'Wi-Fi signal '+state.wifi_rssi+' dBm · channel '+state.wifi_channel:'Wi-Fi disconnected'}\nRemote: ${connection}${detail}`;
}
function renderRemotes(r){
 remoteState=r;const paired=!!r.bonded,busy=remotePending||(!paired&&(!!r.pairing||!!r.connecting||!!r.scanning));
 // A saved bond always displays Paired; live connection state belongs in Diagnostics.
 // Keep this tab text-only: do not add a connection or pairing progress indicator.
 $('remoteStatus').textContent=paired?'Paired':r.connecting?'Pairing with remote…':r.pairing&&r.too_far?'Remote detected. Bring it closer to Amity.':r.pairing||r.scanning||remotePending?'Looking for a nearby remote… Bring it close and put it in pairing mode.':r.last_error===13?'No nearby remote found. Bring it closer and try again.':r.last_error?'Pairing failed. Reset the remote’s pairing and try again.':'Not paired';renderDiagnostics();
 $('toggleRemote').setAttribute('aria-busy',String(busy));
 $('toggleRemote').textContent=busy?(paired?'Unpairing…':'Pairing…'):paired?'Unpair remote':'Pair remote';$('toggleRemote').disabled=busy;
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
function pairingStatus({error,busy,pairing,connected,available=true}){
 return error?'Pairing operation failed. Please try again.':busy?'Updating pairing…':pairing?'Ready to pair':connected?'Connected':available?'Not connected':'Connect Amity to Wi-Fi through Matter before pairing.';
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
 $('homekitStatus').textContent=pairingStatus({error:s.error,busy:s.busy||homekitPending,pairing:s.pairing,connected:s.paired,available:s.available});
 $('toggleHomekit').textContent=s.pairing?'Stop pairing':'Start pairing';
 $('toggleHomekit').disabled=homekitPending||s.busy||!s.available||s.paired;
 $('toggleHomekit').hidden=!!s.paired;
 $('removeHomekit').hidden=!s.paired;$('removeHomekit').disabled=homekitPending||s.busy;
 $('homekitPairingCode').textContent=s.pairing&&s.manual_code?`Pairing code: ${s.manual_code}`:'';
 const rows=s.pairing?s.qr_modules:null,container=$('homekitQr');container.replaceChildren();container.hidden=!rows?.length;
 $('homekitPairingHint').hidden=!s.pairing;
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
async function refresh(){if(!token||polling||firmwareUpdating)return;polling=true;try{
 const latest=await request('status');if(firmwareUpdating)return;
 state=latest;renderHostname(state.hostname);renderFrontPhysicalAddress(state.front_physical_address);updateActivityButtons();$('standby').hidden=!(state.current_activity>=0);$('connection').textContent=state.discovering?'Scanning HDMI…':'';
 renderDiagnostics();
 renderDeviceNames();
 if(scanAfter!==null){
  const complete=state.discovery_generation!==scanAfter&&!state.discovering;
  $('discoveryStatus').textContent=complete?(state.devices.length?'':'No HDMI devices found.'):'Scanning HDMI…';
  $('deviceHeading').hidden=!complete||!state.devices.length;$('deviceList').replaceChildren();
  if(complete&&scanGeneration!==state.discovery_generation){scanGeneration=state.discovery_generation;recommendActivities(state.devices);}if(!complete)$('recommendations').hidden=true;
  if(complete)for(const d of [...state.devices].sort((a,b)=>(a.name||'').localeCompare(b.name||''))){const row=document.createElement('div');row.className='device';const name=document.createElement('span');name.textContent=d.name||'Unknown';row.append(deviceIcon(d),name);$('deviceList').append(row);}
 }
 renderRemotes(await request('remotes'));
 renderMatter(await request('matter'));
 if(!$('homekit').hidden)await refreshHomekit();
 }finally{polling=false;updateActivityButtons();}}
async function enter(){const response=await request('config',undefined,true);config=await response.json();autoRecommend=response.headers.get('X-Amity-Configured')==='false';rememberSaved();markDirty();scanPending=false;scanAfter=null;scanGeneration=null;recommendations=[];renderRecommendations();$('devices').hidden=true;$('onboarding').hidden=true;$('login').hidden=true;$('admin').hidden=false;$('logout').hidden=false;selectTab('activities');renderActivities();await refresh();}
async function showWelcome(){
 if(firmwareUpdating)return;
 const setup=await request('setup');renderHostname(setup.hostname);showLogin();
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
 try{const r=await request('login',{password:$('password').value});token=r.token;sessionStorage.setItem('amityToken',token);$('password').value='';message('');await enter();}
 catch(error){reportError(error);if(error.retryAfter){submit.textContent=`Try again in ${error.retryAfter}s`;await new Promise(resolve=>setTimeout(resolve,error.retryAfter*1000));}}
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
  if(current.bonded||current.connected){if(!confirm('Unpair the remote?'))return;await request('remotes/unpair',{});}
  else if(!current.scanning&&!current.connecting&&!current.pairing)await request('remotes/pair',{});
  renderRemotes(await request('remotes'));
 }finally{remotePending=false;renderRemotes(remoteState);}
}));
async function download(path,name){const r=await request(path,undefined,true),url=URL.createObjectURL(await r.blob());const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
$('backup').addEventListener('click',run(()=>download('backup','amity-config.json')));$('logs').addEventListener('click',run(()=>download('logs','amity.log')));
$('restore').addEventListener('change',run(async e=>{const file=e.target.files[0];if(!file)return;if(file.size>32768)throw Error('Backup must be no more than 32 KiB.');if(confirm('Replace activities and quirks with this backup?')){await request('restore',await file.text());await enter();message('Configuration restored.');}e.target.value='';}));
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
 // ESP image header (24), first segment header (8), then esp_app_desc_t (256).
 const bytes=new Uint8Array(await file.slice(0,288).arrayBuffer()),view=new DataView(bytes.buffer);
 if(bytes.length<288||bytes[0]!==0xe9||view.getUint32(32,true)!==0xabcd5432)throw Error('Select an Amity application firmware binary.');
 const field=bytes.slice(48,80),end=field.indexOf(0);
 if(end<0)throw Error('Invalid firmware version field.');
 return new TextDecoder().decode(field.slice(0,end));
}
function saveFirmwareUpdate(){sessionStorage.setItem(firmwareUpdateKey,JSON.stringify(firmwareUpdateRecord));}
function showFirmwareUpdate(record,text){
 firmwareUpdating=true;firmwareUpdateRecord=record;saveFirmwareUpdate();message('');
 $('admin').hidden=true;$('login').hidden=true;$('onboarding').hidden=true;$('logout').hidden=true;$('connection').textContent='';
 $('firmwareUpdate').hidden=false;$('firmwareUpdateStatus').textContent=text;
 $('retryFirmwareWait').hidden=true;$('firmwareUpdateLogin').hidden=true;$('uploadProgress').hidden=false;
}
function firmwareWaitMessage(text){$('firmwareUpdateStatus').textContent=text;$('uploadProgress').removeAttribute('value');}
function finishFirmwareUpdate(text){
 sessionStorage.removeItem(firmwareUpdateKey);sessionStorage.setItem(firmwareResultKey,text);
 dirty=false;location.reload();
}
async function waitForFirmware(){
 if(firmwareWaitRunning)return;firmwareWaitRunning=true;token='';sessionStorage.removeItem('amityToken');
 const record=firmwareUpdateRecord;record.phase='waiting';saveFirmwareUpdate();
 firmwareWaitMessage(record.uncertain?'Upload connection interrupted. Checking whether Amity restarted…':'Firmware uploaded. Restarting Amity…');
 let lastHealth=null;
 try{
  while(Date.now()-record.startedAt<120000){
   const abort=new AbortController(),timeout=setTimeout(()=>abort.abort(),5000);
   try{
    const response=await fetch('/api/setup',{cache:'no-store',signal:abort.signal});
    if(!response.ok)throw Error('Not ready');
    const health=await response.json();lastHealth=health;
    const legacy=health.boot_id===undefined&&health.ready===undefined;
    const restarted=record.previousBootId?legacy||(health.boot_id&&health.boot_id!==record.previousBootId):
     record.sawDisconnect||(typeof health.version==='string'&&health.version!==record.originalVersion);
    if(restarted){
     if(health.ready===true&&typeof health.version==='string'){
      if(health.version===record.version)return finishFirmwareUpdate(`Firmware update complete. Amity is running version ${health.version}. Please log in.`);
      firmwareWaitMessage(`Update did not complete. Amity is running version ${health.version} instead of ${record.version}.`);
      $('uploadProgress').hidden=true;$('firmwareUpdateLogin').hidden=false;return;
     }
     // Older releases lack boot-health metadata. Do not claim a verified update.
     if(legacy&&(record.previousBootId||record.sawDisconnect)){
      firmwareWaitMessage('Amity is back online. Log in to verify the firmware version.');
      $('uploadProgress').hidden=true;$('firmwareUpdateLogin').hidden=false;return;
     }
     firmwareWaitMessage('Amity is starting. Waiting for it to be ready…');
    }
   }catch{
    lastHealth=null;
    record.sawDisconnect=true;saveFirmwareUpdate();
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
  const version=await firmwareVersion(file),current=(await request('status')).version,comparison=compareFirmwareVersions(version,current);
  const warning=comparison===null?
   `The selected firmware version (${version||'unknown'}) cannot be compared with the running version (${current}). It may be a downgrade. Downgrading is not supported. Continue at your own risk?`:
   comparison<0?`You are installing version ${version} over version ${current}. Downgrading is not supported. Continue at your own risk?`:
   `Install firmware ${version} and restart Amity? Current version: ${current}.`;
  if(!confirm(warning))return;
  const health=await request('setup');
  const record={version,originalVersion:current,previousBootId:health.boot_id||'',startedAt:Date.now(),phase:'uploading'};
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
$('frontPhysicalAddress').addEventListener('input',()=>{$('frontPhysicalAddress').dataset.edited='true';});
$('changeFrontPhysicalAddress').addEventListener('submit',run(async e=>{
 e.preventDefault();const submit=e.target.querySelector('button');if(submit.disabled)return;submit.disabled=true;
 try{
  const r=await request('front-physical-address',{address:$('frontPhysicalAddress').value});
  delete $('frontPhysicalAddress').dataset.edited;renderFrontPhysicalAddress(r.front_physical_address);
  showLogin();message('Front physical address saved. Restarting Amity.');
 }finally{submit.disabled=false;}
}));
$('hostname').addEventListener('input',()=>{$('hostname').dataset.edited='true';});
$('changeHostname').addEventListener('submit',run(async e=>{
 e.preventDefault();const submit=e.target.querySelector('button');if(submit.disabled)return;submit.disabled=true;
 try{
  const r=await request('hostname',{hostname:$('hostname').value});delete $('hostname').dataset.edited;
  renderHostname(r.hostname);showLogin();$('login').hidden=true;
  const link=$('hostnameLink');link.href=`https://${r.hostname}.local`;link.textContent=`Open ${r.hostname}.local`;link.hidden=false;
  message('Hostname saved. Restarting. Use the link below when the device is ready.');
 }finally{submit.disabled=false;}
}));
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
 if(pending&&typeof pending.version==='string'&&Number.isFinite(pending.startedAt)){
  if(pending.phase==='uploading')pending.uncertain=true;
  showFirmwareUpdate(pending,'Waiting for the firmware update…');await waitForFirmware();return;
 }
 const setup=await request('setup');renderHostname(setup.hostname);if(setup.required){showLogin();$('login').hidden=true;$('onboarding').hidden=false;}else if(token)await enter();else showLogin();
 const result=sessionStorage.getItem(firmwareResultKey);if(result){sessionStorage.removeItem(firmwareResultKey);message(result);}
}
initialize().catch(e=>{if(e.silent)return;$('connection').textContent='';message(e.message+' Reload to try again.',true);});
