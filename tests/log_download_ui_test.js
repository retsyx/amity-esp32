'use strict';
// Run the shipped app.js download helper: the saved name is the server's
// Content-Disposition filename, falling back to the caller's name.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const source=fs.readFileSync(path.join(__dirname,'../components/web_admin/assets/app.js'),'utf8');
const start=source.indexOf('async function download(');
assert.ok(start>=0,'download helper not found');
const saved=[];
const context=vm.createContext({
 Headers,
 URL:{createObjectURL:()=>'blob:logs',revokeObjectURL(){}},
 document:{createElement:()=>({click(){saved.push(this.download);}})},
 setTimeout:()=>0,
 request:async(_path,_data,raw)=>{assert.equal(raw,true);return context.response;},
});
vm.runInContext(source.slice(start,source.indexOf('\n',start)),context);
const check=async(disposition,expected)=>{
 const headers=new Headers();if(disposition)headers.set('Content-Disposition',disposition);
 context.response={headers,blob:async()=>'log text'};
 await vm.runInContext("download('logs','amity.log')",context);
 assert.equal(saved.at(-1),expected);
};
(async()=>{
 await check('attachment; filename=amity-livingroom.log','amity-livingroom.log');
 await check(null,'amity.log');
 console.log('PASS UI log download uses the Amity hostname filename');
})().catch(error=>{console.error(error);process.exit(1);});
