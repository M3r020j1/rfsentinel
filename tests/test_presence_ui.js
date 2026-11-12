const fs=require('fs'),vm=require('vm'),assert=require('assert');
const s=fs.readFileSync(require('path').join(__dirname,'../main/ui/index.html'),'utf8');
const a=s.indexOf('async function loadPresence('),b=s.indexOf('\nasync function refresh()',a);assert(a>=0 && b>a);
function el(){return {value:'',textContent:'',children:[],style:{},replaceChildren(){this.children=[]},append(...v){this.children.push(...v)},appendChild(v){this.children.push(v)},setAttribute(){}}}
const ids=['cfPresenceTopic','cfPresenceAbsence','presenceList','presenceMac','presenceLabel'];const es=Object.fromEntries(ids.map(id=>[id,el()]));
let items=[],posts=[],seconds=120;const c={lang:'fr',document:{createElement:()=>el()},$:id=>es[id],t:x=>x,msg:()=>{},get:async()=>({topic:'/rfsentinel/presence',absence_s:seconds,items}),post:async(p,b)=>{posts.push(b);if(b.action==='add')items.push({mac:b.mac,label:b.label});if(b.action==='remove')items=items.filter(i=>i.mac!==b.mac)}};
vm.createContext(c);vm.runInContext('let presenceSettingsLoaded=false;\n'+s.slice(a,b),c);
(async()=>{await c.loadPresence(true);assert.equal(es.cfPresenceTopic.value,'/rfsentinel/presence');assert.equal(es.cfPresenceAbsence.value,2); for(const n of [30, 120, 600, 601, 65535]) { seconds=n; await c.loadPresence(true); assert.equal(Math.round(Number(es.cfPresenceAbsence.value)*60), n); }
 es.presenceMac.value='aa:bb:cc:dd:ee:ff';es.presenceLabel.value='phone';await c.addPresence();assert.equal(posts[0].mac,'AA:BB:CC:DD:EE:FF');assert.equal(es.presenceList.children.length,1);
 es.cfPresenceTopic.value='unsaved';await c.loadPresence();assert.equal(es.cfPresenceTopic.value,'unsaved');await es.presenceList.children[0].children[1].onclick();assert.equal(items.length,0);
 es.presenceMac.value='invalid';await c.addPresence();assert.equal(posts.length,2);console.log('presence UI: settings, add, remove and invalid MAC passed');})().catch(e=>{console.error(e);process.exit(1)});
