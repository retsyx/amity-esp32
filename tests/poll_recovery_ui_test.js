'use strict';
// Execute the shipping UI with deterministic DOM, HTTP and timer boundaries.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),crypto=require('node:crypto');
class Element {
 constructor(tag='div'){
  this.tag=tag;this.children=[];this.listeners={};this.dataset={};this.attributes={};this.hidden=false;this.disabled=false;this.open=false;this.textContent='';this.value='';this.className='';
  this.classList={contains:name=>this.className.split(/\s+/).includes(name),toggle:(name,on)=>{const names=new Set(this.className.split(/\s+/).filter(Boolean));if(on??!names.has(name))names.add(name);else names.delete(name);this.className=[...names].join(' ');}};
 }
 append(...items){this.children.push(...items);}
 replaceChildren(...items){this.children=[];this.append(...items);}
 addEventListener(name,callback){(this.listeners[name]??=[]).push(callback);}
 querySelector(){return null;}
 querySelectorAll(){return [];}
 setAttribute(name,value){this.attributes[name]=String(value);}
 removeAttribute(name){delete this.attributes[name];}
 hasAttribute(name){return Object.hasOwn(this.attributes,name);}
 close(){this.open=false;}
 focus(){}
}
const flush=async()=>{for(let n=0;n<10;n++)await new Promise(resolve=>setImmediate(resolve));};
function fixture({adapter=false}={}){
 const elements=new Map(),calls=[],intervals=[],storage=new Map(),faults=new Map(),timers=new Map(),requestOptions=[];
 let now=0,nextTimer=0;
 const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
 const document={getElementById:get,querySelectorAll:()=>[],querySelector:()=>null,createElement:tag=>new Element(tag),createElementNS:(_ns,tag)=>new Element(tag),addEventListener(){},activeElement:null};
 const response=(value,status=200)=>new Response(JSON.stringify(value),{status,headers:{'Content-Type':'application/json'}});
 const status={hostname:'amity',boot_id:'a'.repeat(32),state_revision:'1',current_activity:-1,devices:[],activity_error:'',wifi_connected:true};
 const values={setup:{required:false,hostname:'amity'},status,remotes:{devices:[]},matter:{fabrics:[]},homekit:{available:true},activity:{ok:true}};
 if(adapter){
  values['activity-editor']={version:1,expected_config_revision:'1',expected_presentation_revision:'1',device_choices:[],activities:[{activity_id:'1'.repeat(32),name:'Watch',display:'',source:'',audio:''}]};
  values['activity-editor/status']={...status,expected_boot_id:status.boot_id,expected_state_revision:'1',expected_config_revision:'1'};
 }
 const fetch=async(url,options)=>{
  const route=url.replace('/api/','');calls.push({route,method:options.method});requestOptions.push(options);
  if(faults.has(route)){const fault=faults.get(route);faults.delete(route);return fault(options);}
  assert.ok(Object.hasOwn(values,route),'Unexpected HTTP route '+route);return response(values[route]);
 };
 const location={origin:'https://amity.local',href:'https://amity.local/',replace(){},reload(){}};
 const window={location,addEventListener(){},amityInterface:{route:(path,data)=>({path,data}),close(){},async refresh(){}}};
 const context={window,document,location,console,Response,URL,Headers,TypeError,Error,SyntaxError,TextEncoder,TextDecoder,Uint8Array,DataView,AbortController,DOMException,structuredClone,fetch,confirm:()=>true,crypto:crypto.webcrypto,performance:{now:()=>now},
  sessionStorage:{getItem:key=>storage.get(key)??null,setItem:(key,value)=>storage.set(key,String(value)),removeItem:key=>storage.delete(key)},
  setInterval:(callback,delay)=>{intervals.push({callback,delay});return intervals.length;},clearInterval(){},
  setTimeout:(callback,delay)=>{const id=++nextTimer;timers.set(id,{callback,at:now+delay});return id;},clearTimeout:id=>timers.delete(id),requestAnimationFrame(){},cancelAnimationFrame(){}};
 vm.createContext(context);vm.runInContext(fs.readFileSync(require.resolve('../components/web_admin/assets/app.js'),'utf8'),context);
 if(adapter)vm.runInContext(fs.readFileSync(require.resolve('../components/web_admin/assets/system.js'),'utf8'),context);
 const evaluate=source=>vm.runInContext(source,context);
 const initialize=async()=>{await flush();assert.equal(get('message').textContent,'');get('homekit').hidden=true;get('amityDevices').hidden=true;evaluate("token='session';");if(adapter)await evaluate("(async()=>{config=await request('config');rememberSaved();state=await request('status');})()");calls.length=0;requestOptions.length=0;};
 const tick=async()=>{const timer=intervals.find(timer=>timer.delay===4000);assert.ok(timer);timer.callback();await flush();};
 const fail=(route,kind='transport')=>faults.set(route,()=>{
  if(kind==='transport')throw new TypeError('Failed to fetch');
  if(kind==='http')return response({error:'Request rejected.'},503);
  if(kind==='json')return {ok:true,status:200,json:async()=>{throw new SyntaxError('Invalid response JSON');}};
  if(kind==='json-type')return {ok:true,status:200,json:async()=>{throw new TypeError('Invalid decoded response');}};
  throw Error('Unknown fault');
 });
 const message=()=>({text:get('message').textContent,error:get('message').classList.contains('error')});
 const advance=async milliseconds=>{
  const target=now+milliseconds;await flush();
  for(;;){const next=[...timers].filter(([,timer])=>timer.at<=target).sort((a,b)=>a[1].at-b[1].at)[0];if(!next)break;const [id,timer]=next;timers.delete(id);now=timer.at;timer.callback();await flush();}
  now=target;await flush();
 };
 return {get,calls,window,status,values,faults,evaluate,initialize,tick,fail,message,response,advance,requestOptions,clock:()=>now,timers};
}
(async()=>{
 const recovered=fixture();await recovered.initialize();recovered.fail('status');await recovered.tick();
 assert.equal(recovered.message().error,true);assert.match(recovered.message().text,/Connection interrupted/);
 assert.deepEqual(recovered.calls,[{route:'status',method:'GET'}],'A failed poll must not retry immediately or replay an action');
 await recovered.tick();assert.deepEqual(recovered.message(),{text:'',error:false});
 assert.deepEqual(recovered.calls.slice(1),['status','remotes','matter'].map(route=>({route,method:'GET'})));

 // Reading status alone does not prove that the complete refresh recovered.
 const partial=fixture();await partial.initialize();partial.fail('status');await partial.tick();
 partial.fail('remotes');await partial.tick();assert.equal(partial.message().error,true);
 partial.fail('matter');await partial.tick();assert.equal(partial.message().error,true);
 partial.get('homekit').hidden=false;partial.fail('homekit');await partial.tick();assert.equal(partial.message().error,true);
 partial.get('amityDevices').hidden=false;partial.window.amityInterface.refresh=async()=>{throw Error('Device status invalid');};
 await partial.tick();assert.equal(partial.message().text,'Device status invalid');
 partial.window.amityInterface.refresh=async()=>{};await partial.tick();assert.equal(partial.message().text,'Device status invalid','A nontransport refresh error must stay visible');

 for(const kind of ['http','json','json-type']){
  const invalid=fixture();await invalid.initialize();invalid.fail('status',kind);await invalid.tick();
  const original=invalid.message();assert.equal(original.error,true);assert.doesNotMatch(original.text,/Connection interrupted/);
  await invalid.tick();assert.deepEqual(invalid.message(),original,'A successful poll must preserve HTTP and decode failures');
 }
 const decode=fixture();await decode.initialize();decode.window.amityInterface.route=(path,data)=>({path,data,decode:async()=>{throw new TypeError('Invalid activity status');}});
 await decode.tick();assert.equal(decode.message().text,'Invalid activity status');
 decode.window.amityInterface.route=(path,data)=>({path,data});await decode.tick();assert.equal(decode.message().text,'Invalid activity status');

 // Action errors retain their ownership, including the same fetch TypeError
 // which is recoverable only when it originated in background polling.
 for(const kind of ['http','transport']){
  const action=fixture();await action.initialize();action.fail('status');await action.tick();action.fail('activity',kind);
  await action.evaluate("run(async()=>{await request('activity',{index:0});})({})");
  const original=action.message();assert.equal(original.error,true);await action.tick();assert.deepEqual(action.message(),original);
  assert.equal(action.calls.filter(call=>call.method==='POST').length,1,'Polling recovery must never replay an action');
  action.fail('status');await action.tick();assert.deepEqual(action.message(),original,'A later network interruption must not replace an action error');
 }
 const activity=fixture();await activity.initialize();activity.fail('status');await activity.tick();activity.status.activity_error='Activity could not start';
 await activity.tick();assert.deepEqual(activity.message(),{text:'Activity could not start',error:true});
 activity.status.activity_error='';await activity.tick();assert.equal(activity.message().text,'Activity could not start');

 for(const replacement of ["message('Saved.');","reportError(Error('New action failed'));","message('Reconnecting to the new address…');hostnameReconnect={};","message('Waiting for firmware…');firmwareUpdating=true;","message('Signed out.');token='';"]){
  const newer=fixture();await newer.initialize();newer.fail('status');await newer.tick();
  let finish;newer.faults.set('status',()=>new Promise(resolve=>{finish=resolve;}));await newer.tick();assert.ok(finish);
  newer.evaluate(replacement);const original=newer.message();finish(newer.response(newer.status));await flush();assert.deepEqual(newer.message(),original,'Late successful polls must preserve newer messages');
 }
 const lateFailure=fixture();await lateFailure.initialize();let reject;
 lateFailure.faults.set('status',()=>new Promise((_resolve,fail)=>{reject=fail;}));await lateFailure.tick();
 lateFailure.evaluate("message('Saved.');");reject(new TypeError('Failed to fetch'));await flush();assert.deepEqual(lateFailure.message(),{text:'Saved.',error:false},'Late failed polls must preserve newer messages too');

 const skipped=fixture();await skipped.initialize();skipped.fail('status');await skipped.tick();
 skipped.evaluate('polling=true;');const original=skipped.message();await skipped.tick();assert.deepEqual(skipped.message(),original,'A skipped poll is not successful recovery');
 assert.equal(skipped.calls.length,1);

 // Execute the real activity handler through both shipping scripts. The POST
 // has already admitted a change when a later read of its receipt is lost.
 const operationId='b'.repeat(32),operationRoute='operations/'+operationId;
 const operationFixture=async()=>{
  const ui=fixture({adapter:true});await ui.initialize();
  ui.values['activity-editor/activity']={operation_id:operationId,stage:'admitted',status:'admitted',message:''};
  ui.values[operationRoute]={operation_id:operationId,stage:'complete',status:'complete',message:''};
  ui.values['activity-editor/status'].current_activity=0;
  return ui;
 };
 const select=ui=>ui.evaluate('run(()=>startActivity(config.activities[0]))({})');
 const receiptCalls=ui=>ui.calls.filter(call=>call.route.startsWith('operations/'));
 const onePost=ui=>assert.equal(ui.calls.filter(call=>call.method==='POST').length,1,'An admitted activity must never be posted again');
 const resumed=await operationFixture();resumed.fail(operationRoute);
 const pending=select(resumed);await resumed.advance(250);
 assert.equal(resumed.evaluate('activityActionPending'),true);onePost(resumed);
 assert.equal(receiptCalls(resumed).length,1);assert.equal(resumed.message().error,false);
 resumed.fail(operationRoute);await resumed.advance(250);await resumed.advance(250);await pending;
 assert.equal(receiptCalls(resumed).length,3);assert.ok(receiptCalls(resumed).every(call=>call.route===operationRoute&&call.method==='GET'));
 assert.deepEqual(resumed.message(),{text:'',error:false});assert.equal(resumed.evaluate('activityActionPending'),false);
 assert.equal(resumed.evaluate('state.current_activity'),0);onePost(resumed);
 assert.ok(resumed.requestOptions.filter((_,index)=>resumed.calls[index].route.startsWith('operations/')).every(options=>options.signal instanceof AbortSignal));
 assert.ok(resumed.requestOptions.filter((_,index)=>!resumed.calls[index].route.startsWith('operations/')).every(options=>!Object.hasOwn(options,'signal')),'Ordinary callers retain their original HTTP options');
 const bodyInterrupted=await operationFixture();
 const interruptedBody=()=>new Response(new ReadableStream({start(controller){
  controller.enqueue(new TextEncoder().encode('{"operation_id":'));
  controller.error(new TypeError('Network stream interrupted'));
 }}),{status:200,headers:{'Content-Type':'application/json'}});
 bodyInterrupted.faults.set(operationRoute,interruptedBody);
 const bodyInterruptedAction=select(bodyInterrupted);await bodyInterrupted.advance(250);
 assert.equal(bodyInterrupted.evaluate('activityActionPending'),true);assert.equal(bodyInterrupted.message().error,false);
 await bodyInterrupted.advance(250);await bodyInterruptedAction;
 assert.equal(receiptCalls(bodyInterrupted).length,2);onePost(bodyInterrupted);
 assert.deepEqual(bodyInterrupted.message(),{text:'',error:false});
 // The adapter reads transport bodies before projecting status/config. A
 // failed body read is recoverable; a transformation's TypeError is not.
 const statusStream=await operationFixture();statusStream.faults.set('activity-editor/status',interruptedBody);
 await statusStream.tick();assert.equal(statusStream.message().error,true);assert.match(statusStream.message().text,/Connection interrupted/);
 await statusStream.tick();assert.deepEqual(statusStream.message(),{text:'',error:false});
 const configStream=await operationFixture();configStream.faults.set('activity-editor',interruptedBody);
 assert.equal(await configStream.evaluate("request('config').catch(error=>transportErrors.has(error))"),true);
 const transformation=await operationFixture();const validStatus=transformation.values['activity-editor/status'];transformation.values['activity-editor/status']=null;
 await transformation.tick();const transformationError=transformation.message();
 assert.equal(transformationError.error,true);assert.doesNotMatch(transformationError.text,/Connection interrupted/);
 transformation.values['activity-editor/status']=validStatus;await transformation.tick();assert.deepEqual(transformation.message(),transformationError);

 // Many immediate failures consume the original 40-second budget; neither
 // retry nor an outstanding request can restart that budget or run forever.
 const exhausted=await operationFixture();
 const unavailable=()=>{exhausted.faults.set(operationRoute,unavailable);throw new TypeError('Failed to fetch');};
 exhausted.faults.set(operationRoute,unavailable);const exhaustedAction=select(exhausted);
 await exhausted.advance(40000);await exhaustedAction;onePost(exhausted);
 assert.equal(exhausted.clock(),40000);assert.equal(receiptCalls(exhausted).length,159);
 assert.match(exhausted.message().text,/still applying the change/);assert.equal(exhausted.message().error,true);
 const exhaustedCount=exhausted.calls.length;await exhausted.advance(60000);assert.equal(exhausted.calls.length,exhaustedCount);
 const blocked=await operationFixture();blocked.fail(operationRoute);let pendingSignal;
 const blockedAction=select(blocked);await blocked.advance(250);
 blocked.faults.set(operationRoute,options=>new Promise((_resolve,reject)=>{
  pendingSignal=options.signal;pendingSignal.addEventListener('abort',()=>reject(new DOMException('Timed out','AbortError')));
 }));
 await blocked.advance(39749);assert.ok(pendingSignal&&!pendingSignal.aborted);
 assert.equal(blocked.evaluate('activityActionPending'),true);
 await blocked.advance(1);await blockedAction;
 assert.equal(pendingSignal.aborted,true);assert.equal(blocked.clock(),40000);
 assert.equal(receiptCalls(blocked).length,2);assert.equal(blocked.timers.size,0);onePost(blocked);
 assert.match(blocked.message().text,/still applying the change/);
 const blockedBody=await operationFixture();let bodySignal,bodyRead=false;
 blockedBody.faults.set(operationRoute,options=>{
  bodySignal=options.signal;
  return new Response(new ReadableStream({start(controller){
   controller.enqueue(new TextEncoder().encode('{"operation_id":'));bodyRead=true;
   bodySignal.addEventListener('abort',()=>controller.error(new DOMException('Timed out','AbortError')));
  }}),{status:200});
 });
 const blockedBodyAction=select(blockedBody);await blockedBody.advance(39999);
 assert.ok(bodyRead&&bodySignal&&!bodySignal.aborted);assert.equal(blockedBody.evaluate('activityActionPending'),true);
 await blockedBody.advance(1);await blockedBodyAction;
 assert.equal(bodySignal.aborted,true);assert.equal(receiptCalls(blockedBody).length,1);onePost(blockedBody);
 assert.equal(blockedBody.timers.size,0);assert.match(blockedBody.message().text,/still applying the change/);

 // Only branded fetch errors recover. HTTP/auth, malformed JSON, foreign
 // aborts and explicit terminal failures keep their original meanings.
 for(const kind of ['http','json','json-type']){
  const invalid=await operationFixture();invalid.fail(operationRoute,kind);const action=select(invalid);
  await invalid.advance(250);await action;onePost(invalid);assert.equal(receiptCalls(invalid).length,1);
  const original=invalid.message();assert.equal(original.error,true);assert.doesNotMatch(original.text,/still applying|Connection interrupted/);
  await invalid.tick();assert.deepEqual(invalid.message(),original);
 }
 for(const kind of ['malformed','used','locked','decoder']){
  const invalid=await operationFixture();
  if(kind==='malformed')invalid.faults.set(operationRoute,()=>new Response('{"operation_id":',{status:200}));
  else if(kind==='used')invalid.faults.set(operationRoute,async()=>{const reply=invalid.response(invalid.values[operationRoute]);await reply.text();return reply;});
  else if(kind==='locked')invalid.faults.set(operationRoute,()=>{const reply=invalid.response(invalid.values[operationRoute]);reply.body.getReader();return reply;});
  else{
   const route=invalid.window.amityInterface.route;
   invalid.window.amityInterface.route=(path,data)=>path===operationRoute?{path,data,decode:async()=>{throw new TypeError('Invalid decoded receipt');}}:route(path,data);
  }
  const action=select(invalid);await invalid.advance(250);await action;
  assert.equal(receiptCalls(invalid).length,1);onePost(invalid);assert.equal(invalid.message().error,true);
  assert.doesNotMatch(invalid.message().text,/still applying|Connection interrupted/);
 }
 const cancelled=await operationFixture();cancelled.faults.set(operationRoute,()=>({ok:true,status:200,json:async()=>{throw new DOMException('Unrelated cancellation','AbortError');}}));
 const cancelledAction=select(cancelled);await cancelled.advance(250);await cancelledAction;
 assert.equal(cancelled.message().text,'Unrelated cancellation');assert.equal(receiptCalls(cancelled).length,1);onePost(cancelled);
 const signedOut=await operationFixture();signedOut.faults.set(operationRoute,()=>signedOut.response({error:'Login required'},401));
 const signedOutAction=select(signedOut);await signedOut.advance(250);await signedOutAction;
 assert.equal(signedOut.evaluate('token'),'');assert.equal(signedOut.get('login').hidden,false);
 assert.equal(receiptCalls(signedOut).length,1);onePost(signedOut);assert.equal(signedOut.timers.size,0);
 for(const stage of ['failed','rejected','cancelled']){
  const refused=await operationFixture();refused.values[operationRoute]={operation_id:operationId,stage,status:stage,message:'The receiver refused the change.'};
  const action=select(refused);await refused.advance(250);await action;
  assert.equal(refused.message().text,'The receiver refused the change.');assert.equal(receiptCalls(refused).length,1);onePost(refused);
 }
 for(const terminal of [null,{},[],{operation_id:operationId,stage:'unknown'},{operation_id:'c'.repeat(32),stage:'complete'},
  {operation_id:operationId,stage:'complete',status:'failed'}]){
  const invalid=await operationFixture();invalid.values[operationRoute]=terminal;const action=select(invalid);
  await invalid.advance(250);await action;onePost(invalid);assert.equal(receiptCalls(invalid).length,1);
  assert.match(invalid.message().text,/could not confirm the change/);assert.equal(invalid.message().error,true);
 }
 const lostPost=await operationFixture();lostPost.fail('activity-editor/activity');const lostPostAction=select(lostPost);
 await flush();await lostPostAction;onePost(lostPost);assert.equal(receiptCalls(lostPost).length,0);
 const uncertain=lostPost.message();assert.equal(uncertain.error,true);await lostPost.tick();assert.deepEqual(lostPost.message(),uncertain,'A lost mutation response remains an action error');

 // Diagnostics read other Amity devices' Wi-Fi signals only while visible.
 const signals=fixture();await signals.initialize();Object.assign(signals.window.amityInterface,{enabled:true,deviceNames:()=>[]});signals.get('advanced').hidden=true;
 Object.assign(signals.status,{version:'1.0',wifi_rssi:-50,wifi_channel:6});
 signals.values.system={membership:{peers:[{octopus_id:'1'.repeat(32),enrolled:true,hostname:'den',wifi_rssi:-72,wifi_channel:1},
  {octopus_id:'2'.repeat(32),enrolled:true,hostname:'kitchen'},{octopus_id:'3'.repeat(32),enrolled:false,hostname:'pending'}]}};
 await signals.tick();assert.ok(!signals.calls.some(call=>call.route==='system'),'Hidden diagnostics must not read other devices');
 const rows=()=>signals.get('diagnostics').children.map(child=>child.textContent);
 assert.deepEqual(rows().filter((_,index)=>index%2===0),['Firmware ','Wi-Fi ','Remote: ']);
 signals.get('advanced').hidden=false;await signals.tick();
 assert.deepEqual(rows().slice(0,8),['Firmware ','1.0','Wi-Fi ','signal -50 dBm · channel 6','den Wi-Fi ','signal -72 dBm · channel 1','kitchen Wi-Fi ','unavailable']);
 signals.values.system.membership.peers[0].wifi_rssi=-58;await signals.tick();assert.equal(rows()[5],'signal -58 dBm · channel 1');
 signals.fail('system');await signals.tick();assert.deepEqual(rows().filter((_,index)=>index%2===0),['Firmware ','Wi-Fi ','Remote: ']);
 assert.deepEqual(signals.message(),{text:'',error:false},'A diagnostics read failure is not a polling failure');
 console.log('PASS shipped polling recovery UI:full-cycle recovery; bounded same-receipt GET recovery with cancelled timeout and no POST replay; HTTP/auth/schema/terminal errors and message ownership');
})().catch(error=>{console.error(error);process.exitCode=1;});
