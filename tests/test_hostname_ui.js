const fs=require('fs'),vm=require('vm'),assert=require('assert');
const s=fs.readFileSync(require('path').join(__dirname,'../main/ui/index.html'),'utf8');
const a=s.indexOf('let loadedWifiSsid = "";'),b=s.indexOf('\nasync function ',s.indexOf('async function saveConfig()',a)+1);
const elements={}, el=id=>elements[id]??=({value:'',checked:false});
let sent=null, info='';
const c={lang:'fr',Number,Math,parseInt,parseFloat,$:el,applyI18n:()=>{},updateSecretHints:()=>{},msg:m=>{info=m},t:x=>x,
 get:async()=>({hostname:'rfsentinel-2'}),post:async(p,b)=>{sent=b;return {ok:true}}};
vm.createContext(c);vm.runInContext('let secretFlags={};\n'+s.slice(a,b)+'\npresenceSettingsLoaded=true;',c);
(async()=>{
 await c.loadConfig();assert.equal(el('cfHostname').value,'rfsentinel-2');
 el('cfHostname').value='-invalid';await c.saveConfig();assert.equal(sent,null);assert(info.includes('invalide'));
 el('cfHostname').value='rfsentinel-3';await c.saveConfig();assert.equal(sent.hostname,'rfsentinel-3');assert(info.includes('Redémarrez'));
 console.log('Hostname UI: loading, invalid input and restart notice passed');
})().catch(e=>{console.error(e);process.exit(1)});
