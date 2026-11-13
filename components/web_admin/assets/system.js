'use strict';
// Preserve the original interface. This adapter carries scoped identities and
// revisions privately; only friendly attachment locations enter the controls.
window.amityInterface=(()=>{
 let enabled=false,choices=[],componentOwners=[],componentDialogUpdate=null,replaceUpdate=null,componentPending=false,configuration=null,system=null,openDialog=null,originalSelections=new Map();
 const roles=['display','source','audio','switch'];
 const identifier=()=>crypto.randomUUID().replaceAll('-','');
 const baseAttachment=c=>[c.title,c.location].filter(Boolean).join(' · ');
 const attachment=(c,available=choices)=>{
  const base=baseAttachment(c),side=c.qualified_name?.side;
  return side&&available.some(other=>other.value!==c.value&&baseAttachment(other)===base&&other.qualified_name?.side&&other.qualified_name.side!==side)?base+' · '+(side==='back'?'Source':'Display'):base;
 };
 function choiceFor(role,value){return choices.find(c=>(c.roles||[]).includes(role)&&c.value===value);}
 function visible(role,value){if(!value)return '';const c=choiceFor(role,value);return c?attachment(c):value;}
 function selections(document){return new Map((document.activities||[]).filter(a=>a.activity_id).map(a=>[a.activity_id,Object.fromEntries(roles.map(role=>{const value=role==='switch'?a.switch?.device:a[role];return [role,{value,visible:visible(role,value)}];}))]));}
 function updateChoices(document){
  if(Array.isArray(document.device_choices)){
   const previous=choices;choices=document.device_choices;
   if(typeof relabelActivityEquipment==='function'&&previous.some(old=>choices.some(next=>next.value===old.value&&attachment(next)!==attachment(old,previous)))){
    relabelActivityEquipment((role,value,id)=>{
     const saved=originalSelections.get(id)?.[role];let selector;
     if(saved&&value===saved.visible)selector=saved.value;
     else {const matches=previous.filter(c=>c.roles.includes(role)&&(attachment(c,previous)===value||c.value===value));if(matches.length===1)selector=matches[0].value;}
     return selector&&choiceFor(role,selector)?visible(role,selector):value;
    });
    for(const current of originalSelections.values())for(const role of roles)if(choiceFor(role,current[role].value))current[role].visible=visible(role,current[role].value);
   }
  }
  if(Array.isArray(document.component_owners))componentOwners=document.component_owners;
  componentDialogUpdate?.();
 }
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
  updateChoices(document);
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
  const result=await readResponseJSON(original);
  if(result.warning)message(result.warning,true);
  if(!result.operation_id||!['admitted','preparing','executing','configuring','activating','stopping'].includes(result.stage))return response(result,original);
  const deadline=performance.now()+40000;
  for(let n=0;n<160;n++){
   await new Promise(resolve=>setTimeout(resolve,Math.min(250,Math.max(0,deadline-performance.now()))));
   const remaining=deadline-performance.now();if(remaining<=0)break;
   const controller=new AbortController();let expired=false,next;
   const timer=setTimeout(()=>{expired=true;controller.abort();},remaining);
   try{
    next=await request('operations/'+result.operation_id,undefined,false,controller.signal);
    if(performance.now()>=deadline)break;
   }catch(error){
    if(expired&&controller.signal.aborted&&error.name==='AbortError')break;
    // Admission already succeeded. Resume this read without replaying the
    // mutation, and retain the original deadline across network changes.
    if(!transportErrors.has(error))throw error;
    if(performance.now()>=deadline)break;
    continue;
   }finally{clearTimeout(timer);}
   if(!next||typeof next!=='object'||next.operation_id!==result.operation_id||('status' in next&&next.status!==next.stage))throw Error('Amity could not confirm the change. Check its status before trying again.');
   if(['failed','rejected','cancelled'].includes(next.stage))throw Error(customerText(next.message||'The operation could not complete.'));
   if(['complete','applied'].includes(next.stage)){if(next.warning)message(next.warning,true);return response(next,original);}
   if(!['admitted','preparing','executing','configuring','activating','stopping'].includes(next.stage))throw Error('Amity could not confirm the change. Check its status before trying again.');
  }
  throw Error('Amity is still applying the change. Check its status before trying again.');
 }
 async function saved(original,submitted){const completed=await wait(original),result=await completed.clone().json();if(result.stage==='applied'||result.saved===true)originalSelections=selections(submitted);return completed;}
 function route(path,data){
  if(path==='config'&&data===undefined)return {path:'activity-editor',decode:async original=>{
   const document=await readResponseJSON(original);
   if(document.version!==1||!Array.isArray(document.device_choices))throw Error('Amity returned an invalid activity document. Reload before continuing.');
   enabled=true;return response(project(document),original);
  }};
  if(!enabled)return {path,data};
  if(path==='config'){const submitted=submit(typeof data==='string'?JSON.parse(data):data);return {path:'activity-editor',data:submitted,decode:original=>saved(original,submitted)};}
  if(path==='status')return {path:'activity-editor/status',decode:async original=>{
   const value=await readResponseJSON(original);
   updateChoices(value);
   value.state_revision=value.expected_state_revision;value.boot_id=value.expected_boot_id;
   value.devices=(value.devices||[]).map(d=>{const c=choices.find(c=>c.value===d.name),title=d.title||c?.title||d.name,location=d.location||c?.location||'';return {...d,title,location,name:c?attachment(c):[title,location].filter(Boolean).join(' · ')};});
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
 function close(){enabled=false;configuration=null;system=null;choices=[];componentOwners=[];originalSelections.clear();openDialog?.close();openDialog?.remove();openDialog=null;componentDialogUpdate=null;replaceUpdate=null;componentPending=false;}
 function dialog(title){
  openDialog?.close();openDialog?.remove();const d=document.createElement('dialog');openDialog=d;const heading=document.createElement('h3');heading.textContent=title;d.append(heading);document.body.append(d);d.showModal();d.addEventListener('close',()=>{if(openDialog===d)openDialog=null;d.remove();});return d;
 }
 function text(value){const p=document.createElement('p');p.textContent=value;return p;}
 function field(title,value=''){const l=document.createElement('label');l.textContent=title;const input=document.createElement('input');input.value=value;l.append(input);return {label:l,input};}
 async function addComponent(role,osdName,selected,origin){
  if(!enabled||!roles.includes(role)||componentPending)return;
  const matching=choices.filter(c=>(c.roles||[]).includes(role)&&(attachment(c)===osdName||c.value===osdName));
  if(matching.length===1)osdName=matching[0].qualified_name?.osd_name||matching[0].title;
  await currentSavedConfiguration();
  if(!origin?.isConnected||!origin.closest('dialog')?.open)return;
  const d=dialog('Add component');d.id='componentEditor';d.setAttribute('aria-labelledby','componentEditorTitle');d.querySelector('h3').id='componentEditorTitle';
  cancelOnBackdrop(d,()=>{if(!componentPending)d.close();});
  const form=document.createElement('form'),fields=document.createElement('div'),error=document.createElement('p'),actions=document.createElement('div');fields.className='fields';error.id='componentEditorError';error.setAttribute('role','alert');actions.className='actions';
  const selectField=(title,id)=>{const label=document.createElement('label'),input=document.createElement('select');label.textContent=title;input.id=id;label.append(input);fields.append(label);return {label,input};};
  const owner=selectField('Amity','componentOwner'),adapter=selectField('Adapter','componentAdapter'),side=selectField('Side','componentSide'),name=field('OSD name',osdName);
  name.input.id='componentOsdName';name.input.required=true;name.input.maxLength=14;name.input.autocomplete='off';fields.append(name.label);
  for(const [value,title] of [['back','Source'],['front','Display']]){const option=document.createElement('option');option.value=value;option.textContent=title;side.input.append(option);}side.input.value=role==='source'?'back':'front';
  const fill=(select,values,value)=>{if(values.length!==select.options.length||values.some(([id,title],n)=>select.options[n].value!==id||select.options[n].textContent!==title)){select.replaceChildren();for(const [id,title] of values){const option=document.createElement('option');option.value=id;option.textContent=title;select.append(option);}}if(values.some(([id])=>id===value))select.value=value;};
  const update=()=>{
   if(componentPending||!d.open)return;
   const before=owner.input.value;fill(owner.input,componentOwners.map(o=>[o.octopus_id,o.name]),before);owner.label.hidden=componentOwners.length===1;
   const current=componentOwners.find(o=>o.octopus_id===owner.input.value),capacity=current?.supported_tentacles||0;
   fill(adapter.input,Array.from({length:capacity},(_,n)=>[String(n),String(n+1)]),adapter.input.value);adapter.label.hidden=capacity===1;
   add.disabled=!current||!capacity;
  };
  const cancel=button('Cancel',()=>d.close());cancel.type='button';cancel.className='quiet';
  const add=document.createElement('button');add.type='submit';add.textContent='Add';add.className='primary';actions.append(cancel,add);
  form.append(fields,error,actions);d.append(form);componentDialogUpdate=update;
  owner.input.addEventListener('change',update);
  name.input.addEventListener('input',()=>{error.textContent='';name.input.setCustomValidity('');});
  d.addEventListener('cancel',e=>{if(componentPending)e.preventDefault();});
  d.addEventListener('close',()=>{if(componentDialogUpdate===update)componentDialogUpdate=null;});
  form.addEventListener('submit',async e=>{
   e.preventDefault();if(componentPending)return;error.textContent='';
   if(!name.input.value.trim()||new TextEncoder().encode(name.input.value).length>14||/[\x00-\x1f\x7f]/.test(name.input.value)){error.textContent='Enter an OSD name of 1–14 bytes with no control characters.';return;}
   const chosen={octopus_id:owner.input.value,tentacle_id:Number(adapter.input.value),osd_name:name.input.value,side:side.input.value,role};
   componentPending=true;updateActivityButtons();form.querySelectorAll('input,select,button').forEach(c=>c.disabled=true);
   try{
    const latest=await currentSavedConfiguration();
    const result=await completed('activity-editor/components',{expected_config_revision:latest.expected_config_revision,expected_presentation_revision:latest.expected_presentation_revision,...chosen});
    const document=await request('config');
    config.expected_config_revision=document.expected_config_revision;config.expected_presentation_revision=document.expected_presentation_revision;
    const choice=choices.find(c=>c.value==='component:'+result.component_id)||choices.find(c=>(c.roles||[]).includes(role)&&c.qualified_name&&c.qualified_name.octopus_id===chosen.octopus_id&&c.qualified_name.tentacle_id===chosen.tentacle_id&&c.qualified_name.side===chosen.side&&c.qualified_name.osd_name===chosen.osd_name);
    if(!choice)throw Error('The component was saved. Reload the equipment list before selecting it.');
    selected(attachment(choice));renderDeviceNames();d.close();message('Component saved.');
   }catch(e){if(!e.silent&&d.open)error.textContent=e.message;}
   finally{componentPending=false;updateActivityButtons();if(d.open){form.querySelectorAll('input,select,button').forEach(c=>c.disabled=false);update();}}
  });
  update();name.input.focus();
 }
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
 // The status sits directly below the name; the actions are beside both.
 function deviceHeader(row,title,status){
  const header=document.createElement('div'),about=document.createElement('div'),heading=document.createElement('h3'),state=text(status),actions=document.createElement('div');
  header.className='amity-device-header';about.className='amity-device-title';actions.className='amity-device-actions';heading.textContent=title;
  state.className=status==='Not responding'?'amity-device-status not-responding':'amity-device-status';
  about.append(heading,state);header.append(about,actions);row.append(header);return actions;
 }
 async function refresh(){
  if(!enabled||!token)return;
  [configuration,system]=await Promise.all([request('backup'),request('system')]);
  const update=(system.membership?.peers||[]).some(peer=>peer.management_ready)?await request('updates/cohort').catch(()=>null):null;
  const root=$('managedAmityList');root.replaceChildren();
  const original=configuration.controller_octopus_id||system.octopus_id;
  const localRow=document.createElement('article'),localActions=deviceHeader(localRow,system.hostname||'Name unavailable','This device');
  localActions.append(button('Settings',()=>deviceSettings({octopus_id:system.octopus_id})));
  root.append(localRow);
  const boards=[...(configuration.octopuses||[])];
  for(const peer of system.membership?.peers||[])if((peer.enrolled||peer.provisional)&&!boards.some(board=>board.octopus_id===peer.octopus_id))boards.push({octopus_id:peer.octopus_id,supported_tentacles:peer.supported_tentacles||1});
  for(const known of system.named_devices||[])if(!boards.some(board=>board.octopus_id===known.octopus_id))boards.push({octopus_id:known.octopus_id});
  for(const board of boards){
   if(board.octopus_id===original)continue;
   const peer=system.membership?.peers?.find(p=>p.octopus_id===board.octopus_id),row=document.createElement('article');
   const target=update?.targets?.find(target=>target.octopus_id===board.octopus_id)||update?.automatic_failures?.find(target=>target.octopus_id===board.octopus_id);
   const actions=deviceHeader(row,boardName(board),peer?.ready?'Connected':peer?.management_ready?(target?firmwareTargetText(target,update.image_bytes):'Updating firmware automatically'):peer?.enrolled?'Not responding':'Not added');
   actions.append(Object.assign(button('Remove',()=>remove(board)),{className:'primary danger'}),button('Replace',()=>replace(board)),button('Settings',()=>deviceSettings(board)));
   root.append(row);
  }
  replaceUpdate?.();
 }
 function physicalAddress(value){
  value=value.trim().toUpperCase();if(!value)return '1.0.0.0';
  if(!/^[0-9A-F](\.[0-9A-F]){3}$/.test(value)||value==='F.F.F.F')return null;
  let zero=false;for(const digit of value.split('.')){if(digit==='0')zero=true;else if(zero)return null;}return value;
 }
 async function deviceSettings(board){
  const name=boardName(board),d=dialog(name+' settings'),status=text('Reading settings…'),addressesTitle=document.createElement('div'),rows=document.createElement('div'),hostname=field('Hostname',name==='Name unavailable'?'':name);
  d.classList.add('adapter-settings');status.setAttribute('role','status');status.setAttribute('aria-live','polite');
  addressesTitle.id='adapterAddressesTitle';addressesTitle.className='adapter-addresses-title';addressesTitle.textContent='Adapter physical addresses';
  hostname.input.maxLength=32;hostname.input.required=true;hostname.input.pattern='[A-Za-z0-9]([A-Za-z0-9-]{0,30}[A-Za-z0-9])?';hostname.input.autocomplete='off';hostname.input.spellcheck=false;hostname.input.dataset.hostname='';hostname.input.disabled=true;
  hostname.input.addEventListener('input',()=>hostname.input.setCustomValidity(''));
  d.append(hostname.label,addressesTitle,rows,text('Leave adapter addresses blank to use 1.0.0.0.'),status);
  let snapshot=null,inputs=[],busy=false,loading=false,needsReview=false;
  const path='devices/'+board.octopus_id+'/settings';
  const save=button('Save',async()=>{
   if(busy||!snapshot)return;
   const nextHostname=hostname.input.value.trim().toLowerCase();
   hostname.input.setCustomValidity(/^[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$/.test(nextHostname)?'':'Use 1–32 letters, digits or hyphens, without a leading or trailing hyphen.');
   if(!hostname.input.reportValidity())return;
   const renamed=nextHostname!==snapshot.hostname;
   const changes=[];
   for(const {input,row,editable} of inputs){
    if(!editable)continue;
    const address=physicalAddress(input.value);
    input.setCustomValidity(address===null?'Use a dotted hexadecimal address such as 2.0.0.0; zeros must follow all nonzero digits.':'');
    if(!input.reportValidity())return;
    if(address!==row.default_front_physical_address)changes.push({tentacle_id:row.tentacle_id,default_front_physical_address:address});
   }
   if(!changes.length&&!renamed){status.textContent='No settings changed.';return;}
   const reviewed=snapshot;let adaptersSaved=false,completed=false;
   busy=true;save.disabled=true;reload.disabled=true;hostname.input.disabled=true;inputs.forEach(({input})=>input.disabled=true);
   async function apply(data){
    let result=await request(path,{request_id:identifier(),expected_boot_id:reviewed.boot_id,...data});
    for(let n=0;result.stage==='admitted'&&n<80;n++){
     await new Promise(resolve=>setTimeout(resolve,250));if(!d.isConnected)return false;
     try{result=await request('operations/'+result.operation_id);}catch(error){error.settingsUncertain=true;throw error;}
    }
    if(!d.isConnected)return false;
    if(result.stage!=='applied'){
     const uncertain=result.stage==='unknown'||result.stage==='admitted';
     const error=Error(uncertain?'The save outcome is unknown. Refresh and check the saved values before trying again.':customerText(result.message||'Settings were not saved. Refresh and check the settings before trying again.'));
     error.settingsResult=true;error.settingsUncertain=uncertain;throw error;
    }
    return true;
   }
   try{
    if(changes.length){
     status.textContent='Saving adapter addresses…';
     if(!await apply({expected_settings_revision:reviewed.settings_revision,changes}))return;
     adaptersSaved=true;
    }
    if(renamed){
     status.textContent='Saving hostname…';
     if(!await apply({expected_hostname_revision:reviewed.hostname_revision,hostname:nextHostname,changes:[]}))return;
    }
    completed=true;
    if(renamed&&board.octopus_id===system?.octopus_id){startHostnameReconnect({hostname:nextHostname,bootId:reviewed.boot_id,deviceId:board.octopus_id});return;}
    if(await load(true))status.textContent='Settings saved.';
    if(renamed){await refresh().catch(reportError);await refreshActivityConfiguration().catch(reportError);}
   }catch(error){
    const uncertain=error.settingsUncertain||!error.settingsResult&&(!error.status||error.status>=500);
    needsReview=completed||adaptersSaved||uncertain;
    if(completed)status.textContent='Settings saved. Refresh to read the latest values.';
    else if(adaptersSaved)status.textContent=uncertain?'Adapter addresses saved. The hostname save outcome is unknown. Refresh and check the saved values before trying again.':'Adapter addresses saved. Hostname was not saved. '+customerText(error.message)+' Refresh before trying again.';
    else if(uncertain)status.textContent='The save outcome is unknown. Reconnect, refresh and check the saved values before trying again.'+(renamed&&changes.length?' Hostname was not changed.':'');
    else status.textContent=customerText(error.message)+(renamed&&changes.length?' Hostname was not changed.':'');
    throw Error(status.textContent);
   }
   finally{busy=false;reload.disabled=false;hostname.input.disabled=!snapshot||hostname.input.dataset.editable!=='true';save.disabled=needsReview||hostname.input.disabled;inputs.forEach(({input,editable})=>input.disabled=!editable);}
  });
  const reload=button('Refresh',()=>load()),cancel=button('Cancel',()=>d.close()),actions=document.createElement('div');save.disabled=true;save.className='primary';cancel.className='quiet';
  actions.className='actions';actions.append(reload,cancel,save);d.append(actions);
  d.addEventListener('click',event=>{
   if(event.target!==d)return;
   const bounds=d.getBoundingClientRect();
   if(event.clientX<bounds.left||event.clientX>=bounds.right||event.clientY<bounds.top||event.clientY>=bounds.bottom)cancel.click();
  });
  async function load(afterSave=false){
   if(loading||(busy&&!afterSave))return;loading=true;reload.disabled=true;save.disabled=true;
   try{
    let result;
    for(let n=0;n<24;n++){
     result=await request(path);if(!d.isConnected)return;
     if(result.status!=='reading')break;
     status.textContent='Reading settings…';await new Promise(resolve=>setTimeout(resolve,250));
    }
    snapshot=result.settings;needsReview=false;inputs=[];rows.replaceChildren();
    const fresh=result.status==='ready',named=!!snapshot?.hostname;
    hostname.input.value=snapshot?.hostname||name;hostname.input.disabled=!fresh||!named;hostname.input.dataset.editable=String(fresh&&named);hostname.input.setCustomValidity('');
    if(named)d.querySelector('h3').textContent=snapshot.hostname+' settings';
    status.textContent=fresh?'':result.status==='unsupported'?'Update this Amity to edit its settings.':result.status==='offline'?'This Amity is offline. Saved information is read only.':'Current settings are unavailable. Refresh after reconnecting the Amity.';
    if(snapshot?.adapters?.length){
     const table=document.createElement('table'),head=document.createElement('thead'),body=document.createElement('tbody'),headRow=document.createElement('tr');
     table.setAttribute('aria-labelledby','adapterAddressesTitle');
     for(const title of ['Adapter','Default','Detected Address','Selected Address']){const cell=document.createElement('th');cell.scope='col';cell.textContent=title;headRow.append(cell);}
     head.append(headRow);table.append(head,body);rows.append(table);
     for(const row of snapshot.adapters){
      const tr=document.createElement('tr'),adapter=document.createElement('th'),setting=document.createElement('td'),detected=document.createElement('td'),selected=document.createElement('td'),input=document.createElement('input'),editable=fresh&&named&&row.configured;
      tr.dataset.adapterId=String(row.tentacle_id);adapter.scope='row';adapter.textContent=String(row.tentacle_id+1);
      if(!row.configured){const note=document.createElement('small');note.textContent='Not configured';adapter.append(note);}
      input.value=row.default_front_physical_address;input.setAttribute('aria-label','Default address for adapter '+(row.tentacle_id+1));
      input.maxLength=7;input.placeholder='1.0.0.0';input.autocomplete='off';input.spellcheck=false;input.disabled=!editable;input.addEventListener('input',()=>input.setCustomValidity(''));
      detected.textContent=row.observed_front_physical_address||'—';selected.textContent=row.effective_front_physical_address||'—';
      setting.append(input);tr.append(adapter,setting,detected,selected);body.append(tr);inputs.push({input,row,editable});
     }
    }
    if(!inputs.length)rows.append(text('No adapter settings are available.'));
    save.disabled=hostname.input.disabled;
    return fresh;
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
  });remove.disabled=true;remove.className='primary danger';close.className='primary';
  const actions=document.createElement('div');actions.className='actions';actions.append(remove,close);d.className='remove-amity';d.append(actions);
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
 // Replace opens Add with a note on where to connect the new Amity, and
 // continues with the added Amity once it is confirmed.
 function add(options={}){
  const d=dialog('Add Amity'),scanId=identifier(),name=field('Name'),code=field('Matter setup code'),status=text('Searching for Amity devices…');
  const selection=document.createElement('label'),select=document.createElement('select'),found=text('');selection.textContent='Amity device';selection.append(select);selection.hidden=true;
  name.input.required=true;name.input.maxLength=32;name.input.pattern='[A-Za-z0-9]((?:[A-Za-z0-9]|-){0,30}[A-Za-z0-9])?';name.input.title='Use 1–32 letters, digits or hyphens, beginning and ending with a letter or digit.';name.input.autocomplete='off';name.input.spellcheck=false;
  code.input.type='password';code.input.maxLength=256;code.input.autocomplete='off';code.input.spellcheck=false;code.input.placeholder='Printed Matter setup code';code.input.title='Enter the printed Matter setup code or the text from its Matter QR code.';
  name.label.hidden=code.label.hidden=true;found.hidden=true;
  if(options.note)d.append(text(options.note));
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
      finished=true;ended=true;code.input.value='';await refresh();if(live()){d.close();message('Amity added.');if(options.added&&state.candidate_id)options.added(state.candidate_id);}return;
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
  const closeButton=button('Cancel',()=>d.close()),actions=document.createElement('div');start.className='primary';closeButton.className='quiet';
  actions.className='actions';actions.append(closeButton,start);d.className='add-amity';d.append(actions);controls();
  void (async()=>{try{await request('membership/scan',{request_id:scanId});if(live())await poll();else await cancelSearch();}catch(error){if(live()){ended=true;status.textContent=customerText(error.message);controls();}}})();
 }
 const sideName=side=>side==='back'?'Source':'Display';
 const portName=port=>`Adapter ${port.tentacle_id+1} · ${sideName(port.side)}`;
 const samePort=(a,b)=>a.tentacle_id===b.tentacle_id&&a.side===b.side;
 const portOnly=n=>({consortium_id:n.consortium_id,octopus_id:n.octopus_id,tentacle_id:n.tentacle_id,side:n.side});
 const listNames=names=>names.length<2?names.join(''):names.slice(0,-1).join(', ')+' and '+names.at(-1);
 const node=(tag,className,value)=>{const e=document.createElement(tag);if(className)e.className=className;if(value!==undefined)e.textContent=value;return e;};
 // Replace asks what takes over the failed Amity's equipment: an Amity that's
 // already added, or a new one. With nothing else added it opens Add directly.
 function replace(board){
  const candidates=(system?.membership?.peers||[]).filter(peer=>peer.enrolled&&peer.ready&&peer.octopus_id!==board.octopus_id);
  if(!candidates.length)return addReplacement(board);
  const old=boardName(board),d=dialog('Replace '+old),select=node('select'),existing=node('input'),fresh=node('input'),label=node('label'),about=node('span');d.className='replace-amity';
  select.setAttribute('aria-label','Added Amity');
  for(const peer of candidates){const option=node('option',null,boardName(peer));option.value=peer.octopus_id;select.append(option);}
  select.value=candidates[0].octopus_id;
  for(const radio of [existing,fresh]){radio.type='radio';radio.name='replace-with';radio.addEventListener('change',update);}
  existing.checked=true;existing.setAttribute('aria-label','An Amity that’s already added');fresh.setAttribute('aria-label','A new Amity');
  const inline=node('div','choice-inline'),first=node('div'),second=node('div'),choices=node('div','replace-choices');
  inline.append(existing,select);first.append(inline);
  about.append(node('strong',null,'A new Amity'),node('small',null,`Unplug ${old} from HDMI and connect the new Amity in its place, using the same HDMI cables. Then add it.`));
  label.append(fresh,about);second.append(label);choices.append(first,second);
  for(const type of ['focus','change'])select.addEventListener(type,()=>{existing.checked=true;fresh.checked=false;update();});
  const go=button('Replace',()=>existing.checked?review(board,select.value):addReplacement(board)),cancel=button('Cancel',()=>d.close()),actions=node('div','actions');
  go.className='primary';cancel.className='quiet';actions.append(cancel,go);
  function update(){first.className='replace-choice'+(existing.checked?' selected':'');second.className='replace-choice'+(fresh.checked?' selected':'');go.textContent=existing.checked?'Replace':'Add';}
  d.append(text(`Replace ${old} with:`),choices,actions);update();
 }
 function addReplacement(board){
  const old=boardName(board);
  add({note:`Unplug ${old} from HDMI and connect the new Amity in its place, using the same HDMI cables.`,added:id=>review(board,id)});
 }
 // Check where the replacement sees the failed Amity's equipment, then replace
 // it. Equipment is matched by name; the HDMI connection mappings the firmware
 // cross-checks are derived from the equipment rather than asked for.
 function review(board,replacementId){
  const old=boardName(board),d=dialog('Replace '+old),named=node('p','replacement'),strong=node('strong'),title=node('div','equipment-title'),list=node('div','equipment-list'),notes=node('div'),summary=node('div','summary'),status=node('p'),check=node('label','check'),unplugged=node('input');
  d.className='replace-amity';status.setAttribute('role','alert');list.setAttribute('role','table');list.setAttribute('aria-label','Equipment');
  named.append(node('span',null,'Replacement: '),strong);
  unplugged.type='checkbox';unplugged.addEventListener('change',()=>controls());check.append(unplugged,node('span',null,`${old} is unplugged from HDMI`));
  const overrides=new Map();let shown=null,busy=false,scanning=false,selects=[];
  const scan=button('Scan HDMI',async()=>{
   if(scanning)return;scanning=true;scan.textContent='Scanning…';controls();
   try{
    const before=await request('status');await request('discover',{});
    for(let n=0;n<120&&d.isConnected;n++){await new Promise(resolve=>setTimeout(resolve,500));const now=await request('status');if(now.discovery_generation!==before.discovery_generation&&!now.discovering)break;}
    await refresh();
   }catch(error){if(d.isConnected)status.textContent=customerText(error.message);}
   finally{scanning=false;scan.textContent='Scan HDMI';controls();}
  });
  title.append(node('h4',null,'Equipment'),scan);
  const back=button('Back',()=>replace(board)),cancel=button('Cancel',()=>d.close()),go=button('Replace',apply),actions=node('div','actions');
  back.className='back';cancel.className='quiet';go.className='primary';actions.append(back,cancel,go);
  d.append(named,title,list,notes,check,summary,status,actions);
  function plan(){
   const snapshot=configuration,current=system;if(!snapshot||!current)return null;
   const peer=(current.membership?.peers||[]).find(p=>p.octopus_id===replacementId),name=boardName({octopus_id:replacementId},current);
   const connectors=[];for(let t=0;t<(peer?.supported_tentacles||1);t++)if((peer?.configured_tentacles_mask??1)&(1<<t))for(const side of ['front','back'])connectors.push({tentacle_id:t,side});
   const available=port=>connectors.some(c=>samePort(c,port));
   const seen=(current.observations||[]).filter(o=>o.resolved&&o.qualified_name?.octopus_id===replacementId).map(o=>o.qualified_name);
   const rows=[];
   for(const component of snapshot.components||[])for(const previous of component.qualified_names||[])if(previous.octopus_id===board.octopus_id){
    const matches=seen.filter(n=>n.osd_name===previous.osd_name);
    rows.push({key:component.component_id+'|'+JSON.stringify(previous),component_id:component.component_id,name:component.label||component.generated_label||previous.osd_name,previous,found:matches.find(n=>samePort(n,previous))||matches[0]||null});
   }
   // Unseen equipment moves with seen equipment from its old connection, or keeps that connection.
   for(const row of rows){
    if(row.found){row.port=row.found;row.status=samePort(row.found,row.previous)?'found':'moved';continue;}
    row.status='missing';row.mate=rows.find(other=>other.found&&samePort(other.previous,row.previous))||null;
    row.reason=row.mate?'mate':'same';row.port=row.mate?.found||row.previous;
    if(!available(row.port)){row.reason='choose';row.port=connectors.find(c=>c.side===row.previous.side)||connectors[0];}
    const chosen=overrides.get(row.key);if(chosen&&available(chosen)){row.reason='chosen';row.port=chosen;}
   }
   const ports=[],targets=new Set(),groups=new Map();
   for(const row of rows){const key=JSON.stringify(portOnly(row.previous));if(!groups.has(key))groups.set(key,[]);groups.get(key).push(row);}
   for(const group of groups.values()){
    const target=group[0].port,key=target.tentacle_id+':'+target.side;
    if(group.some(row=>!samePort(row.port,target))||targets.has(key))continue;
    targets.add(key);ports.push({previous:portOnly(group[0].previous),replacement:{...portOnly(group[0].previous),octopus_id:replacementId,tentacle_id:target.tentacle_id,side:target.side}});
   }
   const equipment=new Set(rows.map(row=>row.component_id));
   const activities=(snapshot.activities||[]).filter(activity=>['display','source','audio','switch_device'].some(role=>equipment.has(activity[role]))).map(activity=>activity.label||activity.generated_label||'Unnamed activity');
   const ready=!!peer?.enrolled&&!!peer?.ready;
   const proposal={expected_config_revision:snapshot.config_revision,expected_presentation_revision:snapshot.presentation_revision,expected_membership_revision:current.membership?.membership_revision,
    expected_target_boot_id:(current.membership?.peers||[]).find(p=>p.octopus_id===board.octopus_id)?.boot_id||null,previous_octopus_id:board.octopus_id,replacement_octopus_id:replacementId,old_hardware_isolated:true,ports,
    equipment:rows.map(row=>({component_id:row.component_id,previous:row.previous,replacement:row.found||{...row.previous,octopus_id:replacementId,tentacle_id:row.port.tentacle_id,side:row.port.side},same_equipment_confirmed:true}))};
   const signature=JSON.stringify([ready,name,connectors,activities,rows.map(row=>[row.key,row.name,row.status,row.port.tentacle_id,row.port.side,row.reason]),ports.length]);
   return {ready,name,connectors,rows,ports,activities,proposal,signature};
  }
  function show(next){
   if(!next||next.signature===shown?.signature){shown=next||shown;controls();return;}
   shown=next;strong.textContent=next.name;list.replaceChildren();notes.replaceChildren();summary.replaceChildren();selects=[];
   for(const row of next.rows){
    const line=node('div','equipment-row'),where=node('span','equipment-connection'),state=node('span','equipment-status');
    line.setAttribute('role','row');where.setAttribute('role','cell');state.setAttribute('role','cell');
    const name=node('span','equipment-name',row.name);name.setAttribute('role','rowheader');
    if(row.found){where.append(node('span',null,portName(row.found)));if(row.status==='moved')where.append(node('small',null,'was '+portName(row.previous)));}
    else{
     const select=node('select');select.setAttribute('aria-label',row.name+' connection');
     for(const port of next.connectors){const option=node('option',null,portName(port));option.value=port.tentacle_id+':'+port.side;select.append(option);}
     select.value=row.port.tentacle_id+':'+row.port.side;
     select.addEventListener('change',()=>{const [t,side]=select.value.split(':');overrides.set(row.key,{tentacle_id:Number(t),side});show(plan());});
     where.append(select);selects.push(select);
    }
    state.append(node('span',row.status,{found:'Found',moved:'Moved',missing:'Not found'}[row.status]));
    line.append(name,where,state);list.append(line);
   }
   const moved=next.rows.filter(row=>row.status==='moved').map(row=>row.name);
   if(!next.ready)notes.append(node('p','warning',`${next.name} is not connected. Check that it is powered on and wait for it to reconnect.`));
   if(!next.rows.length)notes.append(node('p','warning',`${old} has no saved equipment to move. Use Remove instead.`));
   if(moved.length)notes.append(node('p','warning',`${listNames(moved)} ${moved.length>1?'are on different connections than they were':'is on a different connection than it was'} on ${old}. If that’s a mistake, fix the HDMI cables and press Scan HDMI.`));
   if(next.rows.length&&!next.ports.length)notes.append(node('p','warning',`Equipment that shared a connection on ${old} is on different connections now. Check the HDMI cables, then press Scan HDMI.`));
   for(const row of next.rows.filter(row=>row.status==='missing'&&row.reason!=='chosen'))notes.append(node('p','note',row.reason==='mate'?`${row.name} wasn’t found. It moves with ${row.mate.name}, which shared its connection.`:row.reason==='same'?`${row.name} wasn’t found. It keeps the connection it had on ${old}. Change it if it’s connected somewhere else.`:`${row.name} wasn’t found. Choose its connection on ${next.name}.`));
   if(next.activities.length){const items=node('ul');for(const activity of next.activities)items.append(node('li',null,activity));summary.append(node('p',null,`These activities will use ${next.name}:`),items);}
   summary.append(node('p',null,`${old} will be removed from this system.`));
   controls();
  }
  function controls(){
   const ready=!!shown?.ready&&!!shown.rows.length&&!!shown.ports.length;
   go.disabled=busy||!ready||!unplugged.checked;scan.disabled=busy||scanning;back.disabled=busy;unplugged.disabled=busy;
   for(const select of selects)select.disabled=busy;
  }
  async function apply(){
   const next=plan();
   if(!next||next.signature!==shown?.signature){show(next);status.textContent='The equipment changed while this dialog was open. Check it, then press Replace again.';return;}
   busy=true;status.textContent='';go.textContent='Replacing…';controls();
   try{
    await completed('replacements/apply',next.proposal);
    d.close();await refresh();
    if(await refreshActivityConfiguration())message(`${next.name} replaced ${old}.`);
   }catch(error){
    if(d.isConnected)status.textContent=customerText(error.message);
    await refresh().catch(()=>{});
   }finally{busy=false;go.textContent='Replace';if(d.isConnected)controls();}
  }
  const update=()=>{if(d.isConnected)show(plan());};
  replaceUpdate=update;d.addEventListener('close',()=>{if(replaceUpdate===update)replaceUpdate=null;});
  update();
 }
 $('addAmity').addEventListener('click',run(()=>add()));
 return {get enabled(){return enabled;},get componentPending(){return componentPending;},route,close,refresh,open:refresh,addComponent,deviceNames:role=>choices.filter(c=>(c.roles||[]).includes(role)).map(c=>attachment(c)),isDeviceChoice:value=>choices.some(c=>attachment(c)===value)};
})();
