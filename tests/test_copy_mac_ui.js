const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync(require('path').join(__dirname,'../main/ui/index.html'),'utf8');
const a=html.indexOf('function copyMacButton('),b=html.indexOf('\nlet observationRequest',a);assert(a>=0&&b>a);
let copied='',messages=[],stopped=0,removed=0,ok=true;
function element(tag){return {tag,style:{},setAttribute(){},select(){copied=this.value},remove(){removed++}}}
const context={document:{createElement:element,body:{appendChild(){}},execCommand(cmd){assert.equal(cmd,'copy');return ok}},window:{isSecureContext:false},navigator:{},t:k=>k,msg:(m,v)=>messages.push([m,v])};
vm.createContext(context);vm.runInContext(html.slice(a,b),context);
(async()=>{let button=context.copyMacButton('AA:BB:CC:DD:EE:FF');await button.onclick({stopPropagation(){stopped++}});assert.equal(copied,'AA:BB:CC:DD:EE:FF');assert.equal(removed,1);assert.deepEqual(messages.pop(),['copiedMac',true]);ok=false;await button.onclick({stopPropagation(){stopped++}});assert.deepEqual(messages.pop(),['copyMacFailed',false]);context.window.isSecureContext=true;context.navigator.clipboard={writeText:async x=>{copied=x}};button=context.copyMacButton('11:22:33:44:55:66');await button.onclick({stopPropagation(){stopped++}});assert.equal(copied,'11:22:33:44:55:66');assert.equal(stopped,3);assert.equal((html.match(/td2\.appendChild\(copyMacButton\(o\.mac\)\)/g)||[]).length,2);console.log('MAC copy: HTTP fallback, secure clipboard, errors and row propagation passed')})().catch(e=>{console.error(e);process.exit(1)});
