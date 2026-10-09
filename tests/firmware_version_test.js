/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

'use strict';
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const root=path.resolve(__dirname,'..'),source=fs.readFileSync(path.join(root,'components/web_admin/assets/app.js'),'utf8');
const context=vm.createContext({});
vm.runInContext(source.slice(source.indexOf('function compareFirmwareVersions('),source.indexOf('async function firmwareVersion(')),context);
let count=0;
for(const line of fs.readFileSync(path.join(__dirname,'firmware_versions.tsv'),'utf8').trimEnd().split('\n')){
 const [candidate,running,expected]=line.split('\t');
 assert.equal(context.compareFirmwareVersions(candidate,running),expected==='unknown'?null:Number(expected),line);count++;
}
assert.equal(context.compareFirmwareVersions('1.0.0\n','1.0.0'),null);
console.log(`PASS ${count+1} browser firmware version comparisons`);
