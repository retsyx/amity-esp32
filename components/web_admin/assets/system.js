'use strict';
// Preserve the original interface. This adapter carries scoped identities and
// revisions privately; only friendly attachment locations enter the controls.
window.amityInterface=(()=>{
 let enabled=false,choices=[],configuration=null,system=null,openDialog=null,originalSelections=new Map();
 const roles=['display','source','audio','switch'];
 const identifier=()=>crypto.randomUUID().replaceAll('-','');
 const attachment=c=>[c.title,c.location].filter(Boolean).join(' · ');
 function choiceFor(role,value){return choices.find(c=>(c.roles||[]).includes(role)&&c.value===value);}
 function visible(role,value){if(!value)return '';const c=choiceFor(role,value);return c?attachment(c):value;}
 function selections(document){return new Map((document.activities||[]).filter(a=>a.activity_id).map(a=>[a.activity_id,Object.fromEntries(roles.map(role=>{const value=role==='switch'?a.switch?.device:a[role];return [role,{value,visible:visible(role,value)}];}))]));}
 function reference(role,value,previous){
  if(!value)return '';
  // Equal display text can represent different CEC sides. An unchanged field
  // keeps its saved selector; a new ambiguous choice still needs resolution.
  if(previous&&value===previous.visible){const retained=choiceFor(role,previous.value);if(retained)return retained.value;throw Error('Choose '+role+' from the device list, including its location.');}
  const matches=choices.filter(c=>(c.roles||[]).includes(role)&&(attachment(c)===value||c.value===value));
  if(matches.length===1)return matches[0].value;
  const titles=choices.filter(c=>(c.roles||[]).includes(role)&&c.title===value);
  if(titles.length===1)return titles[0].value;
  throw Error('Choose '+role+' from the device list, including its location.');
 }
 function project(document){
  choices=document.device_choices||choices;
  const next=selections(document);
  for(const [id,current] of next){const previous=originalSelections.get(id);if(previous&&roles.some(role=>previous[role].visible===current[role].visible&&previous[role].value!==current[role].value))throw Error('Activities changed in another session. Reload the page before continuing.');}
  originalSelections=next;
  const result=structuredClone(document);
  result.activities=(result.activities||[]).map(a=>{for(const role of roles){if(role==='switch'){if(a.switch)a.switch.device=visible(role,a.switch.device);}else a[role]=visible(role,a[role]);}return a;});
  return result;
 }
 function submit(document){
  return {version:1,request_id:identifier(),expected_config_revision:document.expected_config_revision,expected_presentation_revision:document.expected_presentation_revision,
   activities:(document.activities||[]).map(a=>{const previous=originalSelections.get(a.activity_id);const row={name:a.name,display:reference('display',a.display,previous?.display),source:reference('source',a.source,previous?.source),audio:reference('audio',a.audio,previous?.audio)};if(a.activity_id)row.activity_id=a.activity_id;if(a.switch)row.switch={device:reference('switch',a.switch.device,previous?.switch),input:a.switch.input};return row;})};
 }
 function response(value,original){return new Response(JSON.stringify(value),{status:original.status,headers:original.headers});}
 async function wait(original){
  const result=await original.json();
  if(result.warning)message(result.warning,true);
  if(!result.operation_id||!['admitted','preparing','executing','configuring','activating','stopping'].includes(result.stage))return response(result,original);
  for(let n=0;n<160;n++){
   await new Promise(resolve=>setTimeout(resolve,250));
   const next=await request('operations/'+result.operation_id);
   if(['failed','rejected','cancelled'].includes(next.stage))throw Error(customerText(next.message||'The operation could not complete.'));
   if(!['admitted','preparing','executing','configuring','activating','stopping'].includes(next.stage)){if(next.warning)message(next.warning,true);return response(next,original);}
  }
  throw Error('Amity is still applying the change. Check its status before trying again.');
 }
 async function saved(original,submitted){const completed=await wait(original),result=await completed.clone().json();if(result.stage==='applied'||result.saved===true)originalSelections=selections(submitted);return completed;}
 function route(path,data){
  if(path==='config'&&data===undefined)return {path:'activity-editor',decode:async original=>{
   const document=await original.json();
   if(document.version!==1||!Array.isArray(document.device_choices))throw Error('Amity returned an invalid activity document. Reload before continuing.');
   enabled=true;return response(project(document),original);
  }};
  if(!enabled)return {path,data};
  if(path==='config'){const submitted=submit(typeof data==='string'?JSON.parse(data):data);return {path:'activity-editor',data:submitted,decode:original=>saved(original,submitted)};}
  if(path==='status')return {path:'activity-editor/status',decode:async original=>{
   const value=await original.json();
   if(Array.isArray(value.device_choices))choices=value.device_choices;
   value.state_revision=value.expected_state_revision;value.boot_id=value.expected_boot_id;
   value.devices=(value.devices||[]).map(d=>{const c=choices.find(c=>c.value===d.name),title=d.title||c?.title||d.name,location=d.location||c?.location||'';return {...d,title,location,name:[title,location].filter(Boolean).join(' · ')};});
   return response(value,original);
  }};
  if(path==='discover')return {path:'activity-editor/discover',data:{request_id:identifier()},decode:wait};
  if(path==='activity')return {path:'activity-editor/activity',data:{request_id:identifier(),activity_id:data.index<0?null:savedActivities[data.index]?.activity_id,expected_config_revision:config.expected_config_revision,expected_state_revision:String(state.state_revision||'0'),expected_boot_id:state.boot_id},decode:wait};
  if(path==='backup')return {path:'config'};
  if(path==='restore'){
   const document=typeof data==='string'?JSON.parse(data):data,imported=document.configuration||document;
   const submitted={request_id:identifier(),expected_config_revision:config.expected_config_revision,expected_presentation_revision:config.expected_presentation_revision,expected_boot_id:document.expected_boot_id,confirm:true,previous_controller_isolated:true,configuration:imported};return {path:'activity-editor/restore',data:submitted,decode:original=>saved(original,submitted)};
  }
  return {path,data};
 }
 function close(){enabled=false;configuration=null;system=null;choices=[];originalSelections.clear();openDialog?.close();openDialog?.remove();openDialog=null;}
 function dialog(title){
  openDialog?.close();openDialog?.remove();const d=document.createElement('dialog');openDialog=d;const heading=document.createElement('h3');heading.textContent=title;d.append(heading);document.body.append(d);d.showModal();d.addEventListener('close',()=>{if(openDialog===d)openDialog=null;d.remove();});return d;
 }
 function text(value){const p=document.createElement('p');p.textContent=value;return p;}
 function field(title,value=''){const l=document.createElement('label');l.textContent=title;const input=document.createElement('input');input.value=value;l.append(input);return {label:l,input};}
 async function mutation(path,data){return request(''+path,{request_id:identifier(),...data});}
 async function completed(path,data){
  const result=await mutation(path,data);
  if(!result.operation_id)return result;
  const fake=new Response(JSON.stringify(result),{headers:{'Content-Type':'application/json'}});return (await wait(fake)).json();
 }
 function boardName(board,snapshot=system){
  if(!board)return 'Name unavailable';
  if(board.octopus_id===snapshot?.octopus_id)return snapshot.hostname||'Name unavailable';
  return snapshot?.named_devices?.find(device=>device.octopus_id===board.octopus_id)?.hostname||snapshot?.membership?.peers?.find(peer=>peer.octopus_id===board.octopus_id)?.hostname||'Name unavailable';
 }
 function deviceHeader(row,title){
  const header=document.createElement('div'),heading=document.createElement('h3'),actions=document.createElement('div');
  header.className='amity-device-header';actions.className='amity-device-actions';heading.textContent=title;
  header.append(heading,actions);row.append(header);return actions;
 }
 async function refresh(){
  if(!enabled||!token)return;
  [configuration,system]=await Promise.all([request('backup'),request('system')]);
  const root=$('managedAmityList');root.replaceChildren();
  const original=configuration.controller_octopus_id||system.octopus_id;
  const localRow=document.createElement('article'),localActions=deviceHeader(localRow,(system.hostname||'Name unavailable')+' (this device)');
  localActions.append(button('Settings',()=>deviceSettings({octopus_id:system.octopus_id})));
  root.append(localRow);
  const boards=[...(configuration.octopuses||[])];
  for(const peer of system.membership?.peers||[])if((peer.enrolled||peer.provisional)&&!boards.some(board=>board.octopus_id===peer.octopus_id))boards.push({octopus_id:peer.octopus_id,supported_tentacles:peer.supported_tentacles||1});
  for(const known of system.named_devices||[])if(!boards.some(board=>board.octopus_id===known.octopus_id))boards.push({octopus_id:known.octopus_id});
  for(const board of boards){
   if(board.octopus_id===original)continue;
   const peer=system.membership?.peers?.find(p=>p.octopus_id===board.octopus_id),row=document.createElement('article'),actions=deviceHeader(row,boardName(board));row.append(text(peer?.ready?'Connected':peer?.management_ready?'Firmware update required':peer?.enrolled?'Not responding':'Not added'));
   actions.append(button('Settings',()=>deviceSettings(board)));
   if((peer?.ready||peer?.management_ready)&&(peer.capabilities&512)){
    const upload=document.createElement('input');upload.type='file';upload.accept='.bin';upload.hidden=true;
    upload.addEventListener('change',run(async()=>{const file=upload.files[0];upload.value='';if(file)await updateDevice(board,file);}));
    row.append(button('Update firmware',()=>upload.click()),upload);
   }

   actions.append(button('Remove',()=>remove(board)),button('Replace',()=>replace(board)));
   root.append(row);
  }
 }
 function physicalAddress(value){
  value=value.trim().toUpperCase();if(!value)return '1.0.0.0';
  if(!/^[0-9A-F](\.[0-9A-F]){3}$/.test(value)||value==='F.F.F.F')return null;
  let zero=false;for(const digit of value.split('.')){if(digit==='0')zero=true;else if(zero)return null;}return value;
 }
 async function deviceSettings(board){
  const name=boardName(board),d=dialog(name+' settings'),status=text('Reading adapter settings…'),rows=document.createElement('div');
  d.classList.add('adapter-settings');status.setAttribute('role','status');status.setAttribute('aria-live','polite');
  d.append(text('Leave blank to use 1.0.0.0.'),rows,status);
  let snapshot=null,inputs=[],busy=false,loading=false,needsReview=false;
  const path='devices/'+board.octopus_id+'/settings';
  const save=button('Save and restart Amity',async()=>{
   if(busy||!snapshot)return;
   const changes=[];
   for(const {input,row,editable} of inputs){
    if(!editable)continue;
    const address=physicalAddress(input.value);
    input.setCustomValidity(address===null?'Use a dotted hexadecimal address such as 2.0.0.0; zeros must follow all nonzero digits.':'');
    if(!input.reportValidity())return;
    if(address!==row.default_front_physical_address)changes.push({tentacle_id:row.tentacle_id,expected_presence_generation:row.presence_generation,default_front_physical_address:address});
   }
   if(!changes.length){status.textContent='No settings changed.';return;}
   busy=true;save.disabled=true;reload.disabled=true;inputs.forEach(({input})=>input.disabled=true);
   try{
    status.textContent='Saving settings…';
    let result=await request(path,{request_id:identifier(),expected_boot_id:snapshot.boot_id,expected_settings_revision:snapshot.settings_revision,confirm_restart:true,changes});
    for(let n=0;result.stage==='admitted'&&n<80;n++){
     await new Promise(resolve=>setTimeout(resolve,250));if(!d.isConnected)return;
     result=await request('operations/'+result.operation_id);
    }
    if(result.stage!=='applied'){
     needsReview=result.stage==='unknown'||result.stage==='admitted';
     const error=Error(needsReview?'The save outcome is unknown. Refresh and check the saved values before trying again.':customerText(result.message||'Settings were not saved. Refresh and check that the adapters are connected.'));error.settingsResult=true;throw error;
    }
    status.textContent='Settings saved. Restarting '+name+'. Activities will not resume automatically.';
    inputs=[];snapshot=null;
   }catch(error){
    if(!error.settingsResult&&(!error.status||error.status>=500)){needsReview=true;status.textContent='The save outcome is unknown. Reconnect, refresh and check the saved values before trying again.';throw Error(status.textContent);}
    status.textContent=customerText(error.message);throw error;
   }
   finally{busy=false;reload.disabled=false;save.disabled=needsReview||!snapshot||!inputs.some(x=>x.editable);inputs.forEach(({input,editable})=>input.disabled=!editable);}
  });
  const reload=button('Refresh',()=>load()),cancel=button('Close',()=>d.close());save.disabled=true;
  d.append(save,reload,cancel);
  async function load(){
   if(loading||busy)return;loading=true;reload.disabled=true;save.disabled=true;
   try{
    let result;
    for(let n=0;n<24;n++){
     result=await request(path);if(!d.isConnected)return;
     if(result.status!=='reading')break;
     status.textContent='Reading adapter settings…';await new Promise(resolve=>setTimeout(resolve,250));
    }
    snapshot=result.settings;needsReview=false;inputs=[];rows.replaceChildren();
    const fresh=result.status==='ready',named=name!=='Name unavailable';
    status.textContent=fresh?'Only connected adapters with confirmed presence can be edited.':result.status==='unsupported'?'Update this Amity to configure its adapters.':result.status==='offline'?'This Amity is offline. Saved information is read only.':'Current settings are unavailable. Refresh after reconnecting the Amity.';
    for(const row of snapshot?.adapters||[]){
     const section=document.createElement('fieldset'),heading=document.createElement('legend');section.dataset.adapterId=String(row.tentacle_id);heading.textContent=row.name||'HDMI adapter '+(row.tentacle_id+1);section.append(heading);
     const presence=!row.configured?'Not configured':row.presence==='present'?'Connected':row.presence==='absent'?'Not attached':'Presence unknown';section.append(text(presence));
     const setting=field('Default front physical address',row.default_front_physical_address),editable=fresh&&named&&row.configured&&row.presence==='present'&&!!row.presence_generation;
     setting.input.maxLength=7;setting.input.placeholder='1.0.0.0';setting.input.autocomplete='off';setting.input.spellcheck=false;setting.input.disabled=!editable;setting.input.addEventListener('input',()=>setting.input.setCustomValidity(''));
     const selected=row.effective_front_physical_address,source=selected?(selected===row.observed_front_physical_address?' (detected)':' (saved default)'):'';
     section.append(setting.label,text('Detected front address: '+(row.observed_front_physical_address||'Unavailable')),text('Selected front address: '+(selected||'Unavailable')+source));
     inputs.push({input:setting.input,row,editable});rows.append(section);
    }
    if(!inputs.length)rows.append(text('No adapter settings are available.'));
    save.disabled=!inputs.some(x=>x.editable);
   }catch(error){status.textContent=customerText(error.message);throw error;}
   finally{loading=false;reload.disabled=false;}
  }
  await load();
 }
 async function remove(board){
  let review=null,name=boardName(board);
  const d=dialog('Remove '+name+'?'),warning=text(''),activities=document.createElement('ul'),status=text('Reading activities…');status.setAttribute('role','status');status.setAttribute('aria-live','polite');
  d.append(text('Removing this Amity deletes its saved equipment and settings. If it responds, it will also factory reset. If it does not respond, factory reset it using its physical button before adding it again.'),warning,activities,status);
  const close=button('Cancel',()=>d.close()),remove=button('Remove Amity',async()=>{
   if(remove.disabled||!review)return;remove.disabled=true;close.textContent='Close';let admitted=false;
   try{
    status.textContent='Removing '+name+'…';let result=await mutation('membership/remove',review);admitted=true;
    for(let n=0;['admitted','preparing','executing','configuring','activating','stopping'].includes(result.stage)&&n<160;n++){
     await new Promise(resolve=>setTimeout(resolve,250));if(!d.isConnected)return;
     result=await request('operations/'+result.operation_id);
    }
    if(result.stage!=='complete')throw Error('Removal could not be confirmed. Refresh the device list. If this Amity was removed and reset was not confirmed, factory reset it using its physical button before adding it again.');
    status.textContent=result.reset==='scheduled'?name+' removed. Factory reset accepted; it will restart and forget its Wi-Fi and settings.':result.reset==='not_needed'?name+' removed from this system.':name+' removed. '+(result.reset==='offline'?'It did not respond. ':'Factory reset was not confirmed. ')+'Hold its physical button for at least 10 seconds to factory reset it before adding it again.';
    remove.hidden=true;await refresh();await refreshActivityConfiguration();
   }catch(error){
    status.textContent=admitted||!error.status||error.status>=500?'Removal could not be confirmed. Refresh the device list. If this Amity was removed and reset was not confirmed, factory reset it using its physical button before adding it again.':customerText(error.message);
    // A rejected review must be reopened against fresh activities, revisions
    // and boot identity. Never replay the original destructive request.
    remove.hidden=true;
   }
  });remove.disabled=true;d.append(remove,close);
  try{
   const [snapshot,current]=await Promise.all([request('backup'),request('system')]);
   if(!d.isConnected||openDialog!==d)return;
   name=boardName(board,current);
   const peer=current.membership?.peers?.find(value=>value.octopus_id===board.octopus_id);
   if(board.octopus_id===(snapshot.controller_octopus_id||current.octopus_id))throw Error('This Amity cannot remove itself.');
   if(!(snapshot.octopuses||[]).some(value=>value.octopus_id===board.octopus_id)&&!peer?.enrolled&&!peer?.provisional&&!(current.named_devices||[]).some(value=>value.octopus_id===board.octopus_id))throw Error('This Amity is no longer in this system. Close and refresh the device list.');
   const equipment=new Set((snapshot.components||[]).filter(component=>(component.qualified_names||[]).some(value=>value.octopus_id===board.octopus_id)).map(component=>component.component_id));
   const affected=(snapshot.activities||[]).filter(activity=>['display','source','audio','switch_device'].some(role=>equipment.has(activity[role])));
   for(const activity of affected){const item=document.createElement('li');item.textContent=activity.label||activity.generated_label||'Unnamed activity';activities.append(item);}
   activities.hidden=!affected.length;
   warning.textContent=affected.length?'The following activities will be deleted if you confirm removal:':'No saved activities reference this Amity.';
   review={octopus_id:board.octopus_id,confirm:true,expected_membership_revision:current.membership?.membership_revision,
    expected_target_boot_id:peer?.boot_id||null,expected_config_revision:snapshot.config_revision,
    expected_presentation_revision:snapshot.presentation_revision,affected_activity_ids:affected.map(activity=>activity.activity_id)};
   status.textContent='';remove.disabled=false;
  }catch(error){if(d.isConnected)status.textContent=customerText(error.message);}
 }
 function add(){
  const d=dialog('Add Amity'),scanId=identifier(),name=field('Name'),code=field('Matter setup code'),status=text('Searching for Amity devices…');
  const selection=document.createElement('label'),select=document.createElement('select'),found=text('');selection.textContent='Amity device';selection.append(select);selection.hidden=true;
  name.input.required=true;name.input.maxLength=32;name.input.pattern='[A-Za-z0-9]((?:[A-Za-z0-9]|-){0,30}[A-Za-z0-9])?';name.input.title='Use 1–32 letters, digits or hyphens, beginning and ending with a letter or digit.';name.input.autocomplete='off';name.input.spellcheck=false;
  code.input.type='password';code.input.maxLength=256;code.input.autocomplete='off';code.input.spellcheck=false;code.input.placeholder='Printed Matter setup code';code.input.title='Enter the printed Matter setup code or the text from its Matter QR code.';
  name.label.hidden=code.label.hidden=true;found.hidden=true;
  d.append(text('Hold the Reset button on the new Amity for 3 seconds. Keep it powered on and nearby.'),status,found,selection,name.label,code.label);
  let closed=false,timer=null,polling=false,candidates=[],selected='',requiresCode=false,adding=false,admitted=false,finished=false,ended=false,operationId=null,phase='scanning';
  const live=()=>!closed&&openDialog===d&&d.isConnected;
  const controls=()=>{
   const choose=phase==='found'&&!!selected&&!adding&&!ended;name.label.hidden=!selected;name.input.disabled=!choose;
   code.label.hidden=!selected||!requiresCode;code.input.disabled=!choose;code.input.required=requiresCode;
   select.disabled=adding||ended;start.disabled=!choose||(requiresCode&&!code.input.value);
  };
  const showCandidates=state=>{
   const next=Array.isArray(state.candidates)?state.candidates:[];
   if(JSON.stringify(next)!==JSON.stringify(candidates)){
    candidates=next;if(!candidates.some(candidate=>candidate.token===selected))selected='';
    select.replaceChildren();const empty=document.createElement('option');empty.value='';empty.textContent='Choose an Amity';select.append(empty);
    for(const candidate of candidates){const option=document.createElement('option');option.value=candidate.token;option.textContent=candidate.label;select.append(option);}
   }
   if(phase==='found'&&candidates.length===1)selected=candidates[0].token;
   select.value=selected;selection.hidden=candidates.length<2;found.hidden=candidates.length!==1;found.textContent=candidates.length===1?candidates[0].label:'';
  };
  const cancelSearch=async()=>{
   try{await mutation('membership/cancel',{scan_id:scanId});}
   catch(error){try{const state=await request('membership/admission');if(state.scan_id===scanId&&['connecting','finishing'].includes(state.phase))message('Adding Amity is still finishing. Check the Amity devices list shortly.');else if(state.scan_id===scanId&&state.phase==='complete')message('Amity was added. Check the Amity devices list.');}catch(ignored){}}
  };
  d.addEventListener('close',()=>{
   closed=true;if(timer!==null)clearTimeout(timer);code.input.value='';
   if(!finished&&!admitted)void cancelSearch();
   else if(admitted&&!finished&&!ended)message('Adding Amity continues. Check the Amity devices list shortly.');
  });
  async function poll(){
   if(!live()||polling||ended)return;polling=true;
   try{
    const state=await request('membership/admission');if(!live())return;
    if(operationId){
     const operation=await request('operations/'+operationId);if(!live())return;
     if(['failed','rejected','cancelled','unknown'].includes(operation.stage)){ended=true;adding=false;status.textContent=customerText(operation.message||'The result could not be confirmed. Check the Amity devices list before trying again.');controls();return;}
     if(operation.stage==='complete'){
      finished=true;ended=true;code.input.value='';await refresh();if(live()){d.close();message('Amity added.');}return;
     }
    }
    if(state.scan_id!==scanId){ended=true;status.textContent='The search changed. Check the Amity devices list before trying again.';controls();return;}
    phase=state.phase;requiresCode=state.requires_setup_code===true;if(!requiresCode)code.input.value='';
    showCandidates(state);
    if(['failed','cancelled'].includes(state.phase)){ended=true;adding=false;status.textContent=customerText(state.error||'The search ended. Close this window and try again.');controls();return;}
    if(state.phase==='found')status.textContent=candidates.length>1?'Choose the Amity you want to add, then give it a name.':candidates.length===1?'Give this Amity a name.':'No Amity was found. Close this window and try again.';
    else if(state.phase==='connecting')status.textContent='Connecting to Amity…';
    else if(state.phase==='finishing'||state.phase==='complete')status.textContent='Finishing setup…';
    else status.textContent='Searching for Amity devices…';

    controls();
   }catch(error){if(live()){ended=true;status.textContent=customerText(error.message||'The search status is unavailable. Close this window and try again.');controls();}}
   finally{polling=false;if(live()&&!ended)timer=setTimeout(poll,400);}
  }
  select.addEventListener('change',()=>{selected=select.value;controls();if(selected)name.input.focus();});code.input.addEventListener('input',controls);
  const start=button('Add',async()=>{
   if(adding||ended||phase!=='found'||!selected)return;
   const hostname=name.input.value.trim().toLowerCase();name.input.value=hostname;if(!name.input.reportValidity())return;
   if(requiresCode&&!code.input.reportValidity())return;
   adding=true;status.textContent='Connecting to Amity…';controls();let secret=code.input.value;code.input.value='';
   try{
    const body={scan_id:scanId,candidate_token:selected,name:hostname};if(requiresCode)body.setup_code=secret;
    const result=await mutation('membership/add',body);secret='';admitted=true;operationId=result.operation_id;
    if(live()){closeButton.textContent='Close';status.textContent='Connecting to Amity…';if(timer!==null)clearTimeout(timer);void poll();}
   }catch(error){secret='';if(live()){adding=false;ended=!error.status||error.status>=500;status.textContent=ended?'The result could not be confirmed. Check the Amity devices list before trying again.':customerText(error.message);controls();}throw error;}
   finally{secret='';}
  });
  const closeButton=button('Cancel',()=>d.close());d.append(start,closeButton);controls();
  void (async()=>{try{await request('membership/scan',{request_id:scanId});if(live())await poll();else await cancelSearch();}catch(error){if(live()){ended=true;status.textContent=customerText(error.message);controls();}}})();
 }
 async function updateDevice(board,file){
  const header=new Uint8Array(await file.slice(0,32).arrayBuffer());
  if(header.length!==32||new TextDecoder().decode(header.slice(0,16))!=='AMITY-UPDATE-V1\n')throw Error('Choose amity-update.bin, which includes the release information.');
  const view=new DataView(header.buffer),manifestSize=view.getUint32(16),signatureSize=view.getUint32(20),imageSize=view.getBigUint64(24),offset=32+manifestSize+signatureSize;
  if(manifestSize<1||manifestSize>8192||![0,64].includes(signatureSize)||imageSize<288n||imageSize>0x3d0000n||BigInt(file.size)!==BigInt(offset)+imageSize)throw Error('Invalid Amity update file.');
  const manifest=await file.slice(32,32+manifestSize).text(),release=JSON.parse(manifest),signature=Array.from(new Uint8Array(await file.slice(32+manifestSize,offset).arrayBuffer()),b=>b.toString(16).padStart(2,'0')).join(''),image=file.slice(offset),version=await firmwareVersion(file);
  if(release.firmware_version!==version)throw Error('The update image does not match its release information.');
  const d=dialog('Update '+boardName(board)),status=text(`Install firmware ${version}? Equipment connected to this Amity will pause during the update.`),progress=document.createElement('progress');progress.max=100;progress.value=0;status.setAttribute('role','status');d.append(status,progress);
  let stopped=false,jobId='';d.addEventListener('close',()=>{stopped=true;});
  const done=button('Cancel',()=>d.close()),start=button('Install firmware',async()=>{
   start.disabled=true;done.disabled=true;
   try{
    await mutation('updates/cohort',{manifest,signature,targets:[board.octopus_id]});
    const deadline=Date.now()+900000;
    while(!stopped){
     const job=await request('updates/cohort');jobId=job.job_id;
     if(job.phase==='complete'){status.textContent='Amity restarted and reported the expected firmware. Choose an activity when you are ready.';progress.value=100;break;}
     if(['failed','interrupted','rolled_back','recovery_required','aborted'].includes(job.phase))throw Error(job.error||'The update needs an explicit retry. Choose the same update file to resume.');
     if(Date.now()>deadline)throw Error('The device has not reported a healthy restart. Check its power and network before retrying.');
     const target=job.targets?.[Number(job.current_index)];if(!target||target.octopus_id!==board.octopus_id)throw Error('The update target changed. Review the current update before continuing.');
     if(job.phase==='boot_checking'){status.textContent='Waiting for Amity to restart and report its firmware…';await new Promise(resolve=>setTimeout(resolve,1500));continue;}
     let sent=Number(target.offset);if(!Number.isSafeInteger(sent)||sent<0||sent>image.size)throw Error('Invalid firmware transfer checkpoint.');
     while(sent<image.size&&!stopped){const bytes=new Uint8Array(await image.slice(sent,sent+2048).arrayBuffer());await mutation('updates/chunk',{cohort_job_id:jobId,target_octopus_id:board.octopus_id,offset:String(sent),data_hex:Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('')});sent+=bytes.length;progress.value=100*sent/image.size;status.textContent=`Sending firmware: ${Math.round(progress.value)}%`;}
     if(!stopped){status.textContent='Installing firmware…';await mutation('updates/install',{cohort_job_id:jobId,target_octopus_id:board.octopus_id});}
    }
   }catch(error){status.textContent=customerText(error.message);throw error;}
   finally{done.disabled=false;done.textContent='Close';await refresh();}
  });d.append(start,done);
 }
 function replace(board){
  const snapshot=structuredClone(configuration),current=structuredClone(system),previousPeer=current.membership?.peers?.find(peer=>peer.octopus_id===board.octopus_id);
  const d=dialog('Replace '+boardName(board)),select=document.createElement('select'),mapping=document.createElement('div'),preview=document.createElement('div');select.setAttribute('aria-label','Replacement Amity');
  const blank=document.createElement('option');blank.textContent='Choose an added Amity';blank.value='';select.append(blank);
  for(const peer of current.membership?.peers||[])if(peer.enrolled&&peer.ready&&peer.octopus_id!==board.octopus_id){const target=snapshot.octopuses.find(b=>b.octopus_id===peer.octopus_id),option=document.createElement('option');option.value=peer.octopus_id;option.textContent=boardName(target,current);select.append(option);}
  d.append(text('Add the replacement Amity first. Disconnect the old device from HDMI, then confirm its cable and equipment mappings.'),select,mapping,preview);
  const isolated=field('Old Amity is disconnected or electrically isolated');isolated.input.type='checkbox';d.append(isolated.label);
  const portOnly=n=>({consortium_id:n.consortium_id,octopus_id:n.octopus_id,tentacle_id:n.tentacle_id,side:n.side}),key=p=>JSON.stringify(portOnly(p)),ports=new Map();
  for(const component of snapshot.components||[])for(const n of component.qualified_names||[])if(n.octopus_id===board.octopus_id)ports.set(key(n),portOnly(n));
  let portFields=[],equipmentFields=[],approved=null;
  const addSelect=(title,items)=>{const label=document.createElement('label');label.textContent=title;const control=document.createElement('select');control.setAttribute('aria-label',title);const empty=document.createElement('option');empty.value='';empty.textContent='Choose a connection';control.append(empty);for(const [value,title] of items){const option=document.createElement('option');option.value=value;option.textContent=title;control.append(option);}label.append(control);mapping.append(label);return control;};
  const apply=button('Apply confirmed replacement',async()=>{if(!approved)throw Error('Preview the mappings first.');await completed('replacements/apply',approved);d.close();await refresh();if(await refreshActivityConfiguration())message('Replacement saved. Choose an activity when its equipment is ready.');});apply.disabled=true;
  select.onchange=()=>{mapping.replaceChildren();preview.replaceChildren();approved=null;apply.disabled=true;portFields=[];equipmentFields=[];if(!select.value)return;
   const peer=current.membership.peers.find(p=>p.octopus_id===select.value),connectors=[];
   for(let i=0;i<(peer?.supported_tentacles||1);i++)if((peer?.configured_tentacles_mask??1)&(1<<i))for(const side of ['front','back'])connectors.push([i+':'+side,`HDMI adapter ${i+1} · ${side==='back'?'From the source':'To the screen or receiver'}`]);
   for(const old of ports.values()){const control=addSelect(`Previous HDMI adapter ${old.tentacle_id+1} · ${old.side==='back'?'From the source':'To the screen or receiver'}`,connectors);portFields.push({old,control});}
   const observed=(current.observations||[]).filter(o=>o.resolved&&o.qualified_name?.octopus_id===select.value);
   for(const component of snapshot.components||[])for(const old of component.qualified_names||[])if(old.octopus_id===board.octopus_id){const control=addSelect('Replacement connection for '+(component.label||component.generated_label||old.osd_name),observed.map(o=>[JSON.stringify(o.qualified_name),`${o.title} · HDMI adapter ${o.qualified_name.tentacle_id+1} · ${o.qualified_name.side==='back'?'From the source':'To the screen or receiver'}`])),confirmed=field('Same physical equipment');confirmed.input.type='checkbox';mapping.append(confirmed.label);equipmentFields.push({component_id:component.component_id,old,control,confirmed:confirmed.input});}
  };
  d.append(button('Preview replacement',async()=>{
   if(!isolated.input.checked||!select.value||portFields.some(f=>!f.control.value)||equipmentFields.some(f=>!f.control.value||!f.confirmed.checked))throw Error('Confirm old-device isolation and every cable and equipment mapping.');
   const proposal={expected_config_revision:snapshot.config_revision,expected_presentation_revision:snapshot.presentation_revision,expected_membership_revision:current.membership?.membership_revision,expected_target_boot_id:previousPeer?.boot_id||null,previous_octopus_id:board.octopus_id,replacement_octopus_id:select.value,old_hardware_isolated:true,
    ports:portFields.map(f=>{const [id,side]=f.control.value.split(':');return {previous:f.old,replacement:{...f.old,octopus_id:select.value,tentacle_id:Number(id),side}};}),equipment:equipmentFields.map(f=>({component_id:f.component_id,previous:f.old,replacement:JSON.parse(f.control.value),same_equipment_confirmed:true}))};
   const result=await mutation('replacements/preview',proposal);approved=proposal;preview.replaceChildren(text('Review the confirmed mappings. Equipment, activity and home-selector identities stay stable.'));for(const task of result.blockers||[])preview.append(text(customerText(typeof task==='string'?task:JSON.stringify(task))));apply.disabled=false;
  }),apply,button('Cancel',()=>d.close()));
 }
 $('addAmity').addEventListener('click',run(add));
 return {get enabled(){return enabled;},route,close,refresh,open:refresh,isDeviceChoice:value=>choices.some(c=>attachment(c)===value)};
})();
