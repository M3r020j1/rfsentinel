/* Exercise the real download handler with completed and interrupted responses. */
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const html = fs.readFileSync(require('path').join(__dirname, '../main/ui/index.html'), 'utf8');
const start = html.indexOf('async function exportJournal()');
const end = html.indexOf('\nlet purgePending', start);
const handler = html.slice(start, end);
async function run(fail) {
  const elements = { exFrom: {value:'2024-02-29'}, exTo: {value:'2024-02-29'}, exportBtn:{}, exportLbl:{} };
  const messages = []; let clicked = 0, created = 0, query = '';
  const ctx = { exporting:false, Date, Number, Math,
    $: id => elements[id], t: s => s, msg: (s, ok) => messages.push({s, ok}),
    fetch: async url => { query = url; return {ok:true, status:200,
      headers:{get:key => key === 'content-type' ? 'text/csv' : 'attachment; filename="test.csv"'},
      blob:async () => { if(fail) throw Error('interrupted'); return {}; }}; },
    URL:{createObjectURL:() => { created++; return 'blob:test'; }, revokeObjectURL:()=>{}},
    document:{body:{appendChild:()=>{}},createElement:()=>({click:()=>clicked++,remove:()=>{}})},
    setTimeout:()=>{} };
  vm.createContext(ctx); vm.runInContext(handler, ctx); await ctx.exportJournal();
  assert(query.includes('from=1709164800&to=1709251199'));
  assert.equal(clicked, fail ? 0 : 1); assert.equal(created, fail ? 0 : 1);
  assert.equal(messages.at(-1).ok,!fail); assert.equal(ctx.exporting,false);
}
Promise.resolve().then(()=>run(false)).then(()=>run(true)).then(()=>console.log('UTC dates and completed/interrupted downloads passed')).catch(e=>{console.error(e);process.exit(1);});
