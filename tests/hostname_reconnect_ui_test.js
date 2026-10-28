'use strict';
// Run the shipped app.js and real hostname submit handler. Only DOM, HTTP,
// navigation and browser timers are deterministic seams; no device is contacted.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const DEVICE='1'.repeat(32),OLD_BOOT='a'.repeat(32),NEW_BOOT='b'.repeat(32);
class Element {
 constructor(tag='div'){this.tag=tag;this.children=[];this.listeners={};this.dataset={};this.attributes={};this.hidden=false;this.disabled=false;this.open=false;this.value='';this._text='';this.style={};const classes=new Set();this.classList={add:value=>classes.add(value),remove:value=>classes.delete(value),contains:value=>classes.has(value),toggle:(value,on)=>on===undefined?(classes.has(value)?classes.delete(value):classes.add(value)):on?classes.add(value):classes.delete(value)};}
 set textContent(value){this._text=String(value);this.children=[];}
 get textContent(){return this._text+this.children.map(child=>child.textContent).join('');}
 append(...items){this.children.push(...items);}
 replaceChildren(...items){this.children=[];this.append(...items);}
 addEventListener(type,listener){(this.listeners[type]??=[]).push(listener);}
 fire(type){const event={target:this,preventDefault(){this.prevented=true;}};return Promise.all((this.listeners[type]||[]).map(listener=>listener(event)));}
 querySelector(selector){if(selector==='button'){let button=this.children.find(child=>child.tag==='button');if(!button){button=new Element('button');this.children.push(button);}return button;}return null;}
 querySelectorAll(){return [];}
 setAttribute(name,value){this.attributes[name]=String(value);}
 removeAttribute(name){delete this.attributes[name];}
 hasAttribute(name){return Object.hasOwn(this.attributes,name);}
 close(){this.open=false;void this.fire('close');}
 focus(){}
 reset(){this.value='';}
}
const flush=async()=>{for(let n=0;n<10;n++)await new Promise(resolve=>setImmediate(resolve));};
function makeFixture({scenarios=[],saveFailure=false,holdOldStatus=false}={}){
 const elements=new Map(),windowListeners={},storage=new Map(),calls=[],navigation=[],heldBodies=[],heldStatus=[];
 const get=id=>{if(!elements.has(id)){const element=new Element(id==='changeHostname'?'form':'div');element.id=id;elements.set(id,element);}return elements.get(id);};
 const document={getElementById:get,querySelectorAll:()=>[],querySelector:()=>null,createElement:tag=>new Element(tag),createElementNS:(_ns,tag)=>new Element(tag),addEventListener(){},activeElement:null,title:''};
 let now=0,nextTimer=0,probeActive=0,maxProbeActive=0,aborts=0,adapterCloses=0;
 const timers=new Map();
 const timer=(callback,delay,repeat=false)=>{const id=++nextTimer;timers.set(id,{callback,at:now+delay,delay,repeat});return id;};
 const location={origin:'https://amity-dut.local',href:'https://amity-dut.local/',replace:url=>navigation.push(url),reload:()=>navigation.push('reload')};
 const ready=(extra={})=>({device_id:DEVICE,boot_id:NEW_BOOT,hostname:'living-room',ready:true,...extra});
 const response=(value,status=200)=>new Response(JSON.stringify(value),{status,headers:{'Content-Type':'application/json'}});
 const fetch=async(url,options={})=>{
  calls.push({url:String(url),options,at:now});
  if(url==='/api/setup')return response({required:false,hostname:'amity-dut',boot_id:OLD_BOOT,ready:true,version:'0.1.85'});
  if(url==='/api/hostname'){
   if(saveFailure)return response({error:'Save failed'},500);
   const body=JSON.parse(options.body);return response({hostname:body.hostname.toLowerCase(),rebooting:true,previous_boot_id:OLD_BOOT,device_id:DEVICE});
  }
  if(url==='/api/status'&&holdOldStatus)return new Promise(resolve=>heldStatus.push(resolve));
  if(String(url).startsWith('https://')&&String(url).endsWith('/api/ready')){
   probeActive++;maxProbeActive=Math.max(maxProbeActive,probeActive);let done=false;const finish=()=>{if(!done){done=true;probeActive--;}};
   options.signal?.addEventListener('abort',()=>{aborts++;finish();});
   const action=scenarios.shift()??{error:'browser'};
   if(action.error){finish();throw new TypeError('Failed to fetch');} // DNS/TLS/CORS are intentionally indistinguishable.
   if(action.timeout)return new Promise((_resolve,reject)=>options.signal.addEventListener('abort',()=>reject(new DOMException('The operation was aborted.','AbortError'))));
   if(action.opaque){finish();return {ok:true,type:'opaque',json:async()=>{throw Error('An opaque response must never be read');}};}
   if(action.httpError){finish();return response({},503);}
   if(action.invalidJson){finish();return {ok:true,type:'cors',json:async()=>{throw SyntaxError('Invalid JSON');}};}
   if(action.holdBody)return {ok:true,type:'cors',json:()=>new Promise(resolve=>heldBodies.push(value=>{finish();resolve(value);}))};
   return {ok:true,type:'cors',json:async()=>{finish();return action.health??ready();}};
  }
  throw Error('Unexpected old-origin request: '+url);
 };
 class ClockDate extends Date {static now(){return now;}}
 const window={location,amityInterface:{close:()=>adapterCloses++,route:(path,data)=>({path,data})},addEventListener:(type,listener)=>(windowListeners[type]??=[]).push(listener)};
 const context={window,document,location,console,Response,URL,Headers,AbortController,DOMException,TextDecoder,DataView,Uint8Array,Map,Set,Date:ClockDate,fetch,
  sessionStorage:{getItem:key=>storage.get(key)??null,setItem:(key,value)=>storage.set(key,String(value)),removeItem:key=>storage.delete(key)},
  setTimeout:(callback,delay)=>timer(callback,delay),clearTimeout:id=>timers.delete(id),setInterval:(callback,delay)=>timer(callback,delay,true),clearInterval:id=>timers.delete(id),
  requestAnimationFrame:callback=>timer(callback,16),cancelAnimationFrame:id=>timers.delete(id),confirm:()=>true};
 vm.createContext(context);vm.runInContext(fs.readFileSync(require.resolve('../components/web_admin/assets/app.js'),'utf8'),context);
 const evaluate=code=>vm.runInContext(code,context);
 const advance=async milliseconds=>{
  const target=now+milliseconds;let iterations=0;
  for(;;){const due=[...timers].filter(([,entry])=>entry.at<=target).sort((a,b)=>a[1].at-b[1].at||a[0]-b[0]);if(!due.length)break;assert.ok(++iterations<10000,'Retry timers must remain bounded per time step');const [id,entry]=due[0];now=entry.at;timers.delete(id);if(entry.repeat)timers.set(id,{...entry,at:now+entry.delay});entry.callback();await flush();}
  now=target;await flush();
 };
 const initialize=async()=>{await flush();assert.equal(get('message').textContent,'');storage.set('amityToken','old-session');evaluate("token='old-session';state={hostname:'amity-dut',boot_id:'"+OLD_BOOT+"'};dirty=false;");get('admin').hidden=false;get('login').hidden=true;get('hostname').value='Living-Room';get('hostname').dataset.edited='true';calls.length=0;adapterCloses=0;};
 const submit=async()=>{const pending=get('changeHostname').fire('submit');await flush();return pending;};
 const probes=()=>calls.filter(call=>call.url.endsWith('/api/ready'));
 const hide=async()=>{for(const listener of windowListeners.pagehide||[])listener({});await flush();};
 return {get,storage,calls,navigation,heldBodies,heldStatus,ready,scenarios,evaluate,advance,initialize,submit,probes,hide,location,
  get aborts(){return aborts;},get maxProbeActive(){return maxProbeActive;},get adapterCloses(){return adapterCloses;},timeouts:()=>[...timers.values()].filter(entry=>!entry.repeat)};
}
(async()=>{
 const normal=makeFixture({scenarios:[{error:'dns'},{timeout:true},{health:{device_id:DEVICE,boot_id:OLD_BOOT,hostname:'living-room',ready:true}},{health:{device_id:DEVICE,boot_id:NEW_BOOT,hostname:'other-amity',ready:true}},{health:{device_id:'9'.repeat(32),boot_id:NEW_BOOT,hostname:'living-room',ready:true}},{health:{device_id:DEVICE,boot_id:NEW_BOOT,hostname:'living-room',ready:false}},{health:{device_id:DEVICE,boot_id:NEW_BOOT,hostname:'living-room',ready:'true'}},{}]});
 await normal.initialize();await normal.submit();assert.equal(normal.calls.filter(call=>call.url==='/api/hostname').length,1);assert.equal(normal.storage.has('amityToken'),false);assert.equal(normal.evaluate('token'),'');assert.equal(normal.get('admin').hidden,true);assert.equal(normal.get('login').hidden,true);assert.equal(normal.get('hostnameLink').href,'https://living-room.local');assert.equal(normal.get('hostnameLink').hidden,false);assert.ok(normal.adapterCloses>0);assert.doesNotMatch(normal.get('message').textContent,/Hostname saved\. Restarting\. Use the link/);
 assert.equal(normal.probes().length,1);await normal.advance(2000);assert.equal(normal.probes().length,2);assert.equal(normal.maxProbeActive,1);await normal.advance(2000);assert.equal(normal.aborts,1);assert.equal(normal.probes().length,2);assert.deepEqual(normal.navigation,[]);
 for(let count=0;count<5;count++){await normal.advance(2000);assert.deepEqual(normal.navigation,[],'Old boot, wrong identity/hostname or not-ready response must not navigate');}
 await normal.advance(2000);assert.equal(normal.navigation.length,1);assert.equal(new URL(normal.navigation[0]).origin,'https://living-room.local');assert.equal(new URL(normal.navigation[0]).pathname,'/');assert.equal(normal.maxProbeActive,1);assert.equal(normal.timeouts().length,0);
 for(const {url,options} of normal.probes()){assert.equal(url,'https://living-room.local/api/ready');assert.equal(options.mode,'cors');assert.equal(options.credentials,'omit');assert.equal(options.cache,'no-store');assert.equal(options.redirect,'error');assert.equal(options.referrerPolicy,'no-referrer');assert.ok(options.signal);assert.equal(new Headers(options.headers).has('Authorization'),false);}
 assert.equal(normal.calls.filter(call=>call.url.startsWith('/api/')&&call.url!=='/api/hostname').length,0,'Old authenticated polling must stop after save');
 const failed=makeFixture({saveFailure:true});await failed.initialize();await failed.submit();assert.equal(failed.get('message').textContent,'Save failed');assert.equal(failed.get('hostname').value,'Living-Room');assert.equal(failed.get('hostname').dataset.edited,'true');assert.equal(failed.get('changeHostname').querySelector('button').disabled,false);assert.equal(failed.get('admin').hidden,false);assert.equal(failed.storage.get('amityToken'),'old-session');assert.equal(failed.probes().length,0);assert.deepEqual(failed.navigation,[]);
 const blocked=makeFixture({scenarios:[{opaque:true},{httpError:true},{invalidJson:true}]});await blocked.initialize();await blocked.submit();await blocked.advance(130000);assert.ok(blocked.probes().length>50,'Browser TLS/CORS failures must keep periodically retrying');assert.deepEqual(blocked.navigation,[]);assert.match(blocked.get('message').textContent,/power.*Wi-Fi|Wi-Fi.*power/);assert.match(blocked.get('message').textContent,/certificate/);assert.equal(blocked.get('hostnameLink').hidden,false);blocked.scenarios.push({});await blocked.advance(2000);assert.equal(blocked.navigation.length,1);
 const cancelled=makeFixture({scenarios:[{timeout:true}]});await cancelled.initialize();await cancelled.submit();assert.equal(cancelled.probes().length,1);await cancelled.hide();assert.equal(cancelled.aborts,1);assert.equal(cancelled.timeouts().length,0);await cancelled.advance(10000);assert.equal(cancelled.probes().length,1);assert.deepEqual(cancelled.navigation,[]);
 const superseded=makeFixture({scenarios:[{holdBody:true},{}]});await superseded.initialize();await superseded.submit();assert.equal(superseded.heldBodies.length,1);superseded.evaluate("startHostnameReconnect({hostname:'den',previousBootId:'"+OLD_BOOT+"',deviceId:'"+DEVICE+"'})");await flush();assert.equal(superseded.probes().length,2);assert.deepEqual(superseded.navigation,[]);superseded.heldBodies[0](superseded.ready());await flush();assert.deepEqual(superseded.navigation,[]);superseded.scenarios.push({health:superseded.ready({hostname:'den'})});await superseded.advance(2000);assert.equal(superseded.navigation.length,1);assert.equal(new URL(superseded.navigation[0]).origin,'https://den.local');
 for(const status of [200,401]){const stale=makeFixture({holdOldStatus:true});await stale.initialize();const old=stale.evaluate('refresh()');await flush();assert.equal(stale.heldStatus.length,1);await stale.submit();stale.heldStatus[0](new Response(JSON.stringify(status===200?{hostname:'amity-dut',boot_id:OLD_BOOT,current_activity:-1,devices:[]}:{}),{status,headers:{'Content-Type':'application/json'}}));try{await old;}catch(error){assert.equal(status,401);assert.equal(error.silent,true);}await flush();assert.equal(stale.get('deviceName').textContent,'living-room');assert.match(stale.get('message').textContent,/Reconnecting/);assert.equal(stale.get('login').hidden,true);await stale.hide();}
 console.log('PASS shipped hostname reconnect UI: real submit/session cleanup, DNS and timeout retry, exact new-boot identity/readiness, safe cross-origin fetch, continuous TLS/CORS fallback, cancellation/supersession and stale old-session replies');
})().catch(error=>{console.error(error);process.exitCode=1;});
