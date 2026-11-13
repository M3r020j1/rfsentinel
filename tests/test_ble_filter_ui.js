const fs=require('fs'),vm=require('vm'),assert=require('assert');
const s=fs.readFileSync(require('path').join(__dirname,'../main/ui/index.html'),'utf8');
const start=s.indexOf('let loadedWifiSsid = "";'),end=s.indexOf('\nasync function ',s.indexOf('async function saveConfig()',start)+1);
assert(start>=0&&end>start);
const elements={}; const el=id=>elements[id]??=( {value:'',checked:false} );
let current=1, saved=null;
const c={lang:'fr',Number,Math,parseInt,parseFloat,$:el,applyI18n:()=>{},updateSecretHints:()=>{},msg:()=>{},t:x=>x,
 get:async()=>({ble_address_filter:current}),post:async(p,b)=>{saved=b;return {ok:true}}};
vm.createContext(c);vm.runInContext('let secretFlags={};\n'+s.slice(start,end)+'\npresenceSettingsLoaded=true;',c);
(async()=>{for(const mode of [0,1,2]){current=mode;await c.loadConfig();assert.equal(el('cfBleFilter').value,String(mode));await c.saveConfig();assert.equal(saved.ble_address_filter,mode);}console.log('BLE UI: all filter modes load and save');})().catch(e=>{console.error(e);process.exit(1)});
