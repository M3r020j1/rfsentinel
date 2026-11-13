const fs=require('fs'),vm=require('vm'),assert=require('assert');
const s=fs.readFileSync(require('path').join(__dirname,'../main/ui/index.html'),'utf8');
const a=s.indexOf('let observationRequest = 0;'),b=s.indexOf('/* --- historique',a);
assert(a>=0&&b>a);
const elements={obsType:{value:''},obsNote:{textContent:''},pageInfo:{textContent:'Chargement…'},prevBtn:{},nextBtn:{}};
let message='';
const c={state:{page:1,sort:'',dir:'asc'},lang:'fr',$:id=>elements[id],encodeURIComponent,
 obsDateTime:()=>0,get:async()=>{throw Error('reseau: store')},msg:(m)=>{message=m},t:x=>x};
vm.createContext(c);vm.runInContext(s.slice(a,b),c);
(async()=>{await c.loadObservations();assert(message.includes('Journal indisponible'));
 assert(elements.obsNote.textContent.includes('Journal indisponible'));
 assert.equal(elements.pageInfo.textContent,'—');assert(elements.prevBtn.disabled&&elements.nextBtn.disabled);
 console.log('Observation error: storage failure is identified and loading state cleared');
})().catch(e=>{console.error(e);process.exit(1)});
