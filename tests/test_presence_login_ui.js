const fs = require('fs'), vm = require('vm'), assert = require('assert');
const s = fs.readFileSync(require('path').join(__dirname, '../main/ui/index.html'), 'utf8');
const login = s.slice(s.indexOf('async function doLogin()'), s.indexOf('\nasync function doLogout()'));
const save = s.slice(s.indexOf('async function saveConfig()'), s.indexOf('\nasync function ', s.indexOf('async function saveConfig()') + 1));
const elements = {loginUser:{value:'admin'},loginPass:{value:'secret'}};
let calls = [], messages = [];
const ctx = {lang:'fr', $:id=>elements[id], post:async(path)=>{calls.push(path); return {ok:true}},
  setLoggedIn:()=>{}, refresh:async()=>{}, loadConfig:async()=>{}, loadObservations:async()=>{},
  loadBlacklist:async()=>{}, loadPresence:async(load)=>{calls.push('presence:'+load)},
  msg:(message,ok)=>messages.push({message,ok}), t:x=>x};
vm.createContext(ctx);
vm.runInContext('let presenceSettingsLoaded = false; let loadedWifiSsid = "";\n'+login+'\n'+save,ctx);
(async()=>{
 await ctx.doLogin(); assert(calls.includes('presence:true'));
 calls=[]; await ctx.saveConfig(); assert.deepEqual(calls,[]); assert.equal(messages.at(-1).ok,false);
 console.log('Presence login: settings loaded after login; save blocked if loading failed');
})().catch(e=>{console.error(e);process.exit(1)});
