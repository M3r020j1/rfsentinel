const fs=require('fs'),vm=require('vm'),assert=require('assert');
const s=fs.readFileSync(require('path').join(__dirname,'../main/ui/index.html'),'utf8');
const a=s.indexOf('function historyTimestamp('),b=s.indexOf('/* Le canevas doit',a);assert(a>=0&&b>a);
const elements={histPrompt:{style:{}},histInfo:{},histBody:{}};let calls=[],renders=[],mode='ok';
const c={lang:'fr',Date,Number,Infinity,encodeURIComponent,$:id=>elements[id],t:x=>x,drawMap:()=>{},wigleErrText:x=>'error:'+x,
 renderHist:(rows,info)=>renders.push({rows,info}),get:async path=>{calls.push(path);if(path.startsWith('/api/history')){if(mode==='localerr')throw Error('local');if(mode==='many')return {observations:Array.from({length:10},(_,i)=>({ts:`2026-02-${String(i+1).padStart(2,'0')} 00:00:00`,mac:'AA'}))};return {observations:[{ts:'2026-01-02 00:00:00',mac:'AA',type:'BLE'},{ts:'2026-01-01 00:00:00',mac:'AA',type:'WIFI'}],truncated:true}}
 if(mode==='remoteerr')throw Error('network');if(mode==='many')return {summary_only:true,results:[{time:'2024-01-01T00:00:00Z',lasttime:'2025-01-01T00:00:00Z',mac:'AA'},{time:'2023-01-01T00:00:00Z',lasttime:'2024-01-01T00:00:00Z',mac:'AA'}]};return {results:[{time:'2026-01-03T00:00:00Z',mac:'AA',lat:1,lon:2}],truncated:false}}};
vm.createContext(c);vm.runInContext('let histSeq=0,histItems=[];\n'+s.slice(a,b),c);
(async()=>{await c.showHistory('AA','BLE');assert(calls[0].startsWith('/api/history')&&calls[1].startsWith('/api/wigle'));let last=renders.at(-1);assert.deepEqual(Array.from(last.rows,x=>x.source),['wigle','local','local']);assert(last.info.includes('10 plus récents'));assert.equal(last.rows[0].ts,'2026-01-03T00:00:00Z');
 mode='remoteerr';calls=[];renders=[];await c.showHistory('AA','BLE');last=renders.at(-1);assert.equal(last.rows.length,2);assert(last.info.includes('network'));
 mode='localerr';renders=[];await c.showHistory('AA','BLE');last=renders.at(-1);assert.equal(last.rows.length,1);assert(last.info.includes('histLocalUnavailable'));
 mode='many';renders=[];await c.showHistory('AA','WIFI');last=renders.at(-1);assert.equal(last.rows.length,10);assert.deepEqual(Array.from(last.rows,x=>x.source),[...Array(8).fill('local'),'wigle','wigle']);assert.equal(last.rows[8].ts,'2025-01-01T00:00:00Z');assert(last.info.includes('priorité à WiGLE'));
 assert.equal(c.historyTimestamp('2026-01-01 00:00:00'),c.historyTimestamp('2026-01-01T01:00:00+01:00'));console.log('Combined history: ordering, source, partial failures and sequential requests passed')})().catch(e=>{console.error(e);process.exit(1)});
