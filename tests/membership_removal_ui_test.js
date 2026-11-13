'use strict';
// Execute the actual removal UI with deterministic DOM, HTTP and timer seams.
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
 const main='1'.repeat(32),member='2'.repeat(32),other='3'.repeat(32),a='a'.repeat(32),b='b'.repeat(32);
 const document={createElement(tag){return new Element(tag,this);}};document.body=new Element('body',document);
 const add=document.createElement('button');add.id='addAmity';const management=document.createElement('div');management.id='managedAmityList';document.body.append(add,management);
 const posts=[],errors=[],reads=[],timers=[],messages=[];let refreshed=0;
 const configuration={controller_octopus_id:main,config_revision:'7',presentation_revision:'3',
  octopuses:[{octopus_id:main,supported_tentacles:1},{octopus_id:member,supported_tentacles:4}],
  components:[{component_id:a,qualified_names:[{octopus_id:member}]},{component_id:b,qualified_names:[{octopus_id:main}]}],
  activities:['display','source','audio','switch_device'].map((role,index)=>({activity_id:String(index+4).repeat(32),label:'Uses '+role,[role]:a}))};
 configuration.activities.push({activity_id:'9'.repeat(32),label:'Unrelated local activity',source:b});
 const system={octopus_id:main,hostname:'amity-main',named_devices:[{octopus_id:member,hostname:'amity-member'}],
  membership:{membership_revision:'11',peers:options.savedOnly?[]:[{octopus_id:member,enrolled:true,ready:true,boot_id:'c'.repeat(32)}]}};
 if(options.metadataOnly){configuration.octopuses=configuration.octopuses.slice(0,1);configuration.components=[];configuration.activities=configuration.activities.slice(-1);}
 const previous={consortium_id:'d'.repeat(32),octopus_id:member,tentacle_id:0,side:'front',osd_name:'Display'},soundbar={...previous,osd_name:'Soundbar'};
 const admission={scan_id:null,phase:'scanning',error:'',requires_setup_code:false,candidates:[],candidate_id:null};let generation=1;
 const observed=()=>({resolved:true,title:'Display',qualified_name:{...previous,octopus_id:other,side:options.moved?'back':'front'}});
 const join=()=>{
  configuration.octopuses.push({octopus_id:other,supported_tentacles:1});system.named_devices.push({octopus_id:other,hostname:'amity-replacement'});
  system.membership.peers.push({octopus_id:other,enrolled:true,ready:true,boot_id:'e'.repeat(32),supported_tentacles:1,configured_tentacles_mask:1});
  system.observations=options.late?[]:[observed()];
 };
 if(options.replacement||options.joinable)configuration.components[0].qualified_names=[previous];
 if(options.moved)configuration.components.push({component_id:'c'.repeat(32),qualified_names:[soundbar]});
 if(options.replacement)join();
 const request=async(path,data)=>{
  if(data===undefined)reads.push(path);else posts.push([path,structuredClone(data)]);
  if(path==='backup')return structuredClone(configuration);
  if(path==='system')return structuredClone(system);
  if(path==='membership/remove'){
   if(data.expected_config_revision!==configuration.config_revision||data.expected_presentation_revision!==configuration.presentation_revision||JSON.stringify(data.affected_activity_ids)!==JSON.stringify(configuration.activities.filter(row=>['display','source','audio','switch_device'].some(role=>row[role]===a)).map(row=>row.activity_id)))throw Object.assign(Error('Activities changed. Reopen Remove and review the activities.'),{status:409});
   assert.equal(data.expected_membership_revision,'11');assert.equal(data.expected_target_boot_id,options.savedOnly?null:'c'.repeat(32));
   assert.equal(data.confirm,true);assert.equal(data.octopus_id,member);
   if(options.held)return {operation_id:data.request_id,stage:'configuring'};
   configuration.octopuses=configuration.octopuses.filter(row=>row.octopus_id!==member);
   configuration.activities=configuration.activities.filter(row=>!data.affected_activity_ids.includes(row.activity_id));
   configuration.components=configuration.components.filter(row=>row.component_id!==a);system.membership.peers=[];
   system.named_devices=system.named_devices.filter(row=>row.octopus_id!==member);
   return {stage:'complete',reset:options.savedOnly?'not_needed':'scheduled'};
  }
  if(path==='replacements/apply'){
   if(options.applyError)throw Object.assign(Error('Equipment or configuration changed. Review the replacement mapping.'),{status:409});
   configuration.octopuses=configuration.octopuses.filter(row=>row.octopus_id!==member);
   for(const item of data.equipment){const component=configuration.components.find(row=>row.component_id===item.component_id);component.qualified_names=component.qualified_names.map(name=>JSON.stringify(name)===JSON.stringify(item.previous)?item.replacement:name);}
   system.membership.peers=system.membership.peers.filter(row=>row.octopus_id!==member);system.named_devices=system.named_devices.filter(row=>row.octopus_id!==member);
   return {stage:'applied'};
  }
  if(path==='membership/scan'){Object.assign(admission,{scan_id:data.request_id,phase:'found',candidates:[{token:'candidate',label:'Amity 3333'}]});return {operation_id:data.request_id,stage:'complete'};}
  if(path==='membership/admission')return structuredClone(admission);
  if(path==='membership/cancel'){admission.phase='cancelled';return {stage:'complete'};}
  if(path==='membership/add'){Object.assign(admission,{phase:'connecting',candidate_id:other});join();return {operation_id:'add-'+data.request_id,stage:'admitted'};}
  if(path==='status')return {discovery_generation:generation,discovering:false};
  if(path==='discover'){generation++;system.observations=[observed()];return {stage:'complete'};}
  if(path.startsWith('operations/'))return {operation_id:path.slice('operations/'.length),stage:'complete',message:'',reset:'scheduled'};
  throw Error('Unexpected request '+path);
 };
 const context={window:{},document,crypto:crypto.webcrypto,structuredClone,Response,readResponseJSON:response=>response.json(),token:'fixture-session',console,
  $:id=>walk(document.body).find(node=>node.id===id),request,customerText:value=>String(value??''),message:(...args)=>messages.push(args),
  refreshActivityConfiguration:async()=>{refreshed++;return true;},
  setTimeout:callback=>{timers.push(callback);return timers.length;},clearTimeout(){},
  run:action=>async event=>{try{await action(event);}catch(error){errors.push(error.message);}},
 };
 context.button=(title,action)=>{const button=document.createElement('button');button.textContent=title;button.addEventListener('click',context.run(action));return button;};
 vm.createContext(context);vm.runInContext(fs.readFileSync(require.resolve('../components/web_admin/assets/system.js'),'utf8'),context);
 const click=async node=>{assert.ok(node);if(!node.disabled&&!node.hidden)await node.dispatch('click');await flush();};
 const dialog=()=>walk(document.body).find(node=>node.tag==='dialog'&&node.open);
 const button=(title,root=dialog())=>walk(root).find(node=>node.tag==='button'&&node.textContent===title);
 const open=async()=>{await context.window.amityInterface.route('config').decode(new Response(JSON.stringify({version:1,device_choices:[],activities:[]})));await context.window.amityInterface.refresh();await click(button('Remove',management.children[1]));return dialog();};
 return {main,member,other,a,previous,soundbar,configuration,system,posts,reads,errors,messages,dialog,button,click,open,management,context,get refreshed(){return refreshed;},tick:async()=>{for(const callback of timers.splice(0))callback();await flush();}};
}
(async()=>{
 const cancelled=fixture();await cancelled.open();const wording=cancelled.dialog().textContent;
 assert.match(wording,/The following activities will be deleted if you confirm removal:/);
 for(const role of ['display','source','audio','switch_device'])assert.match(wording,new RegExp('Uses '+role));
 assert.doesNotMatch(wording,/Unrelated local activity|stay saved|retained/);
 assert.equal(walk(cancelled.dialog()).filter(node=>node.tag==='li').length,4);
 assert.ok(cancelled.reads.includes('backup')&&cancelled.reads.includes('system'));
 await cancelled.click(cancelled.button('Cancel'));assert.equal(cancelled.posts.length,0);
 const normal=fixture();await normal.open();await normal.click(normal.button('Remove Amity'));
 assert.equal(normal.posts.length,1);assert.deepEqual(normal.posts[0][1].affected_activity_ids,['4','5','6','7'].map(value=>value.repeat(32)));
 assert.equal(normal.posts[0][1].expected_config_revision,'7');assert.equal(normal.posts[0][1].expected_presentation_revision,'3');
 assert.match(normal.dialog().textContent,/Factory reset accepted/);assert.equal(normal.management.children.length,1);assert.equal(normal.refreshed,1);
 assert.deepEqual(normal.configuration.activities.map(row=>row.label),['Unrelated local activity']);
 const stale=fixture();await stale.open();stale.configuration.config_revision='8';stale.configuration.activities.push({activity_id:'f'.repeat(32),label:'New unreviewed activity',source:'a'.repeat(32)});await stale.context.window.amityInterface.refresh();await stale.click(stale.button('Remove Amity'));
 assert.match(stale.dialog().textContent,/Activities changed/);assert.equal(stale.button('Remove Amity').hidden,true);assert.equal(stale.posts.length,1);assert.equal(stale.configuration.activities.length,6);
 const relabelled=fixture();await relabelled.open();relabelled.configuration.presentation_revision='4';await relabelled.click(relabelled.button('Remove Amity'));assert.match(relabelled.dialog().textContent,/Activities changed/);assert.equal(relabelled.posts.length,1);
 const wrongSet=fixture();await wrongSet.open();wrongSet.configuration.activities[0].source='b'.repeat(32);wrongSet.configuration.activities[0].display=null;await wrongSet.click(wrongSet.button('Remove Amity'));assert.match(wrongSet.dialog().textContent,/Activities changed/);assert.equal(wrongSet.configuration.octopuses.length,2);
 const saved=fixture({savedOnly:true});await saved.open();assert.ok(saved.button('Remove Amity'));await saved.click(saved.button('Remove Amity'));assert.match(saved.dialog().textContent,/removed from this system/);assert.doesNotMatch(saved.dialog().textContent,/at least 10 seconds/);assert.equal(saved.management.children.length,1);
 const metadata=fixture({savedOnly:true,metadataOnly:true});await metadata.open();assert.match(metadata.dialog().textContent,/No saved activities reference/);await metadata.click(metadata.button('Remove Amity'));assert.deepEqual(metadata.posts[0][1].affected_activity_ids,[]);assert.equal(metadata.management.children.length,1);
 const actions=fixture();await actions.open();await actions.click(actions.button('Cancel'));const memberRow=actions.management.children[1];assert.ok(actions.button('Replace',memberRow));assert.equal(actions.button('Remove',actions.management.children[0]),undefined);assert.equal(actions.button('Replace',actions.management.children[0]),undefined);assert.doesNotMatch(actions.management.textContent,/Controlled by the original Amity|HDMI adapters/);
 const heading=root=>walk(root).find(node=>node.tag==='h3').textContent,alert=root=>walk(root).find(node=>node.attributes.role==='alert').textContent;
 const unplug=async f=>{const box=walk(f.dialog()).find(node=>node.tag==='input'&&node.type==='checkbox');box.checked=true;await box.dispatch('change');};
 const reviewing=async options=>{const f=fixture(options);await f.open();await f.click(f.button('Cancel'));await f.click(f.button('Replace',f.management.children[1]));await f.click(f.button('Replace'));return f;};
 // With no other Amity added, Replace opens Add directly and continues with the Amity it adds.
 await actions.click(actions.button('Replace',memberRow));assert.equal(heading(actions.dialog()),'Add Amity');await actions.click(actions.button('Cancel'));
 const joining=fixture({joinable:true});await joining.open();await joining.click(joining.button('Cancel'));await joining.click(joining.button('Replace',joining.management.children[1]));
 assert.equal(heading(joining.dialog()),'Add Amity');assert.match(joining.dialog().textContent,/^Add AmityUnplug amity-member from HDMI and connect the new Amity in its place, using the same HDMI cables\./);
 walk(joining.dialog()).find(node=>node.tag==='input'&&node.parent?._text==='Name').value='amity-replacement';await joining.click(joining.button('Add'));
 assert.deepEqual(joining.messages.at(-1),['Amity added.']);assert.equal(heading(joining.dialog()),'Replace amity-member');
 assert.match(joining.dialog().textContent,/Replacement: amity-replacement/);assert.match(joining.dialog().textContent,/DisplayAdapter 1 · DisplayFound/);
 // An added Amity is offered first; the button says what happens next, and Back returns to the choice.
 const chosen=fixture({replacement:true});await chosen.open();await chosen.click(chosen.button('Cancel'));await chosen.click(chosen.button('Replace',chosen.management.children[1]));
 const radios=walk(chosen.dialog()).filter(node=>node.type==='radio');
 assert.equal(heading(chosen.dialog()),'Replace amity-member');assert.deepEqual(walk(chosen.dialog()).filter(node=>node.tag==='option').map(node=>node.textContent),['amity-replacement']);
 assert.equal(radios[0].checked,true);assert.ok(chosen.button('Replace'));
 radios[0].checked=false;radios[1].checked=true;await radios[1].dispatch('change');assert.ok(chosen.button('Add'));assert.equal(chosen.button('Replace'),undefined);
 radios[1].checked=false;radios[0].checked=true;await radios[0].dispatch('change');await chosen.click(chosen.button('Replace'));
 assert.match(chosen.dialog().textContent,/Replacement: amity-replacement/);assert.equal(chosen.button('Replace').disabled,true);
 await chosen.click(chosen.button('Back'));assert.equal(walk(chosen.dialog()).filter(node=>node.type==='radio').length,2);await chosen.click(chosen.button('Replace'));
 await unplug(chosen);assert.equal(chosen.button('Replace').disabled,false);await chosen.click(chosen.button('Replace'));
 const [applied,sent]=chosen.posts.at(-1),port={consortium_id:'d'.repeat(32),octopus_id:chosen.member,tentacle_id:0,side:'front'};
 assert.equal(applied,'replacements/apply');assert.ok(!chosen.posts.some(([path])=>path==='replacements/preview'));
 assert.deepEqual([sent.expected_config_revision,sent.expected_presentation_revision,sent.expected_membership_revision,sent.expected_target_boot_id],['7','3','11','c'.repeat(32)]);
 assert.deepEqual([sent.previous_octopus_id,sent.replacement_octopus_id,sent.old_hardware_isolated],[chosen.member,chosen.other,true]);
 assert.deepEqual(sent.ports,[{previous:port,replacement:{...port,octopus_id:chosen.other}}]);
 assert.deepEqual(sent.equipment,[{component_id:chosen.a,previous:chosen.previous,replacement:{...chosen.previous,octopus_id:chosen.other},same_equipment_confirmed:true}]);
 assert.equal(chosen.dialog(),undefined);assert.equal(chosen.configuration.octopuses.length,2);assert.equal(chosen.refreshed,1);assert.deepEqual(chosen.messages.at(-1),['amity-replacement replaced amity-member.']);
 // Moved equipment says where it was; unseen equipment follows what shared its old connection, and contradicting that blocks Replace.
 const moved=await reviewing({replacement:true,moved:true});
 assert.match(moved.dialog().textContent,/DisplayAdapter 1 · Sourcewas Adapter 1 · DisplayMoved/);
 assert.match(moved.dialog().textContent,/Display is on a different connection than it was on amity-member\./);
 assert.match(moved.dialog().textContent,/Soundbar wasn’t found\. It moves with Display, which shared its connection\./);
 let soundbar=walk(moved.dialog()).find(node=>node.tag==='select');assert.equal(soundbar.value,'0:back');
 await unplug(moved);soundbar.value='0:front';await soundbar.dispatch('change');
 assert.match(moved.dialog().textContent,/Equipment that shared a connection on amity-member is on different connections now\./);assert.equal(moved.button('Replace').disabled,true);
 soundbar=walk(moved.dialog()).find(node=>node.tag==='select');soundbar.value='0:back';await soundbar.dispatch('change');assert.equal(moved.button('Replace').disabled,false);
 await moved.click(moved.button('Replace'));const movedSent=moved.posts.at(-1)[1];
 assert.deepEqual(movedSent.ports,[{previous:port,replacement:{...port,octopus_id:moved.other,side:'back'}}]);
 assert.deepEqual(movedSent.equipment[1],{component_id:'c'.repeat(32),previous:moved.soundbar,replacement:{...moved.soundbar,octopus_id:moved.other,side:'back'},same_equipment_confirmed:true});
 // Scan HDMI finds equipment the replacement had not reported yet.
 const late=await reviewing({replacement:true,late:true});
 assert.match(late.dialog().textContent,/Display wasn’t found\. It keeps the connection it had on amity-member\./);
 // The scan waits on a timer, so start it, then advance time.
 const scanning=late.button('Scan HDMI').dispatch('click');await flush();assert.equal(late.button('Scanning…').disabled,true);assert.ok(late.posts.some(([path])=>path==='discover'));
 await late.tick();await scanning;assert.match(late.dialog().textContent,/DisplayAdapter 1 · DisplayFound/);assert.ok(late.button('Scan HDMI'));
 // An open review follows equipment changes from the normal device-list refresh.
 const changing=await reviewing({replacement:true});
 changing.configuration.components.push({component_id:'f'.repeat(32),label:'Speaker',qualified_names:[{...changing.previous,side:'back',osd_name:'Speaker'}]});
 await changing.context.window.amityInterface.refresh();
 assert.match(changing.dialog().textContent,/Speaker wasn’t found\. It keeps the connection it had on amity-member\./);assert.equal(walk(changing.dialog()).find(node=>node.tag==='select').value,'0:back');
 // A rejected replacement reports its reason inside the dialog and changes nothing.
 const failing=await reviewing({replacement:true,applyError:true});await unplug(failing);await failing.click(failing.button('Replace'));
 assert.match(alert(failing.dialog()),/Equipment or configuration changed/);assert.equal(failing.button('Replace').disabled,false);
 assert.equal(failing.configuration.octopuses.length,3);assert.ok(!failing.messages.some(([text])=>/replaced/.test(text)));
 assert.deepEqual([...cancelled.errors,...normal.errors,...stale.errors,...relabelled.errors,...wrongSet.errors,...saved.errors,...metadata.errors,...actions.errors,...joining.errors,...chosen.errors,...moved.errors,...late.errors,...changing.errors,...failing.errors],[]);
 console.log('PASS removal UI: fresh exact activity review across all roles, inert cancel, stale review rejection, complete disappearance, saved-only purge, and Replace through an added or new Amity with derived connections');
})().catch(error=>{console.error(error);process.exitCode=1;});
