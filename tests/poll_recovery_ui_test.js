'use strict';
// Execute the shipping UI with deterministic DOM, HTTP and timer boundaries.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
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
function fixture(){
 const elements=new Map(),calls=[],intervals=[],storage=new Map(),faults=new Map();
 const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
 const document={getElementById:get,querySelectorAll:()=>[],querySelector:()=>null,createElement:tag=>new Element(tag),createElementNS:(_ns,tag)=>new Element(tag),addEventListener(){},activeElement:null};
 const response=(value,status=200)=>new Response(JSON.stringify(value),{status,headers:{'Content-Type':'application/json'}});
 const status={hostname:'amity',boot_id:'a'.repeat(32),state_revision:'1',current_activity:-1,devices:[],activity_error:'',wifi_connected:true};
 const values={setup:{required:false,hostname:'amity'},status,remotes:{devices:[]},matter:{fabrics:[]},homekit:{available:true},activity:{ok:true}};
 const fetch=async(url,options)=>{
  const route=url.replace('/api/','');calls.push({route,method:options.method});
  if(faults.has(route)){const fault=faults.get(route);faults.delete(route);return fault();}
  assert.ok(Object.hasOwn(values,route),'Unexpected HTTP route '+route);return response(values[route]);
 };
 const location={origin:'https://amity.local',href:'https://amity.local/',replace(){},reload(){}};
 const window={location,addEventListener(){},amityInterface:{route:(path,data)=>({path,data}),close(){},async refresh(){}}};
 const context={window,document,location,console,Response,URL,Headers,TypeError,Error,SyntaxError,TextEncoder,TextDecoder,Uint8Array,DataView,AbortController,DOMException,structuredClone,fetch,confirm:()=>true,
  sessionStorage:{getItem:key=>storage.get(key)??null,setItem:(key,value)=>storage.set(key,String(value)),removeItem:key=>storage.delete(key)},
  setInterval:(callback,delay)=>{intervals.push({callback,delay});return intervals.length;},clearInterval(){},setTimeout(){},clearTimeout(){},requestAnimationFrame(){},cancelAnimationFrame(){}};
 vm.createContext(context);vm.runInContext(fs.readFileSync(require.resolve('../components/web_admin/assets/app.js'),'utf8'),context);
 const evaluate=source=>vm.runInContext(source,context);
 const initialize=async()=>{await flush();assert.equal(get('message').textContent,'');get('homekit').hidden=true;get('amityDevices').hidden=true;evaluate("token='session';");calls.length=0;};
 const tick=async()=>{const timer=intervals.find(timer=>timer.delay===4000);assert.ok(timer);timer.callback();await flush();};
 const fail=(route,kind='transport')=>faults.set(route,()=>{
  if(kind==='transport')throw new TypeError('Failed to fetch');
  if(kind==='http')return response({error:'Request rejected.'},503);
  if(kind==='json')return {ok:true,status:200,json:async()=>{throw new SyntaxError('Invalid response JSON');}};
  if(kind==='json-type')return {ok:true,status:200,json:async()=>{throw new TypeError('Invalid decoded response');}};
  throw Error('Unknown fault');
 });
 const message=()=>({text:get('message').textContent,error:get('message').classList.contains('error')});
 return {get,calls,window,status,faults,evaluate,initialize,tick,fail,message,response};
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
 console.log('PASS shipped polling recovery UI: full-cycle transport recovery, HTTP/schema/action errors, message ownership, interrupted flows, and no action replay');
})().catch(error=>{console.error(error);process.exitCode=1;});
