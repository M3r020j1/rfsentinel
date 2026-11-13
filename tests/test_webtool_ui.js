/* Flasher navigateur : le script de la page est charge tel quel dans un
   contexte isole, puis les fonctions pures sont verifiees.
   Le DOM est absent : la page s'arrete a l'enregistrement de demarrage. */
const fs=require('fs'),path=require('path'),vm=require('vm'),assert=require('assert'),crypto=require('crypto');
const RACINE=path.join(__dirname,'..');
const page=fs.readFileSync(path.join(RACINE,'webtool/index.html'),'utf8');

/* Le script de la page est le dernier bloc <script>. */
const blocs=page.split('<script ').length - 1 + page.split('<script>').length - 1;
assert.equal(blocs,2,'la page doit charger le vendor puis son script');
const script=page.slice(page.lastIndexOf('<script>')+8,page.lastIndexOf('</scr'+'ipt>'));

const elements={};
const attributs={};
const document={readyState:'loading',addEventListener(){},documentElement:{getAttribute(n){return Object.prototype.hasOwnProperty.call(attributs,n)?attributs[n]:null;},setAttribute(n,v){attributs[n]=String(v);}},
  createElement(){return{style:{},classList:{add(){},remove(){},toggle(){}},firstElementChild:{style:{}}};},
  getElementById(id){return elements[id]||(elements[id]={style:{},classList:{add(){},remove(){},toggle(){}},firstElementChild:{style:{},textContent:''},hidden:true,checked:false,value:'',textContent:'',innerHTML:'',_a:{},setAttribute(n,v){this._a[n]=String(v);},getAttribute(n){return this._a[n]===undefined?null:this._a[n];}});}};
const contexte={document,navigator:{},location:{protocol:'file:',href:'file:///webtool/index.html'},
  window:{isSecureContext:true},console,Error,Math,JSON,String,Number,Array,Object,Boolean,RegExp,Promise,
  parseInt,parseFloat,isFinite,isNaN,setTimeout,clearTimeout,Date,Uint8Array,Uint32Array,ArrayBuffer,
  TextEncoder,TextDecoder,URL,fetch:undefined,
  localStorage:(function(){var d={};return{getItem(k){return Object.prototype.hasOwnProperty.call(d,k)?d[k]:null;},
    setItem(k,v){d[k]=String(v);}};})()};
contexte.globalThis=contexte;
vm.createContext(contexte);
vm.runInContext(script,contexte,{filename:'webtool/index.html'});

/* 1 — MD5 : esptool-js appele calculateMD5Hash sur 1,4 Mo, l'implementation
   maison doit coller a crypto de bout en bout. */
const C=contexte;
assert.equal(C.md5(new TextEncoder().encode('abc')),'900150983cd24fb0d6963f7d28e17f72');
assert.equal(C.md5(new Uint8Array(0)),'d41d8cd98f00b204e9800998ecf8427e');
for(const n of [0,1,55,56,57,63,64,65,1000,21648,3072,1477360]){
  const buf=new Uint8Array(n);for(let i=0;i<n;i++)buf[i]=(i*37+11)&255;
  assert.equal(C.md5(buf),crypto.createHash('md5').update(buf).digest('hex'),'md5 taille '+n);
}
for(const p of ['bootloader.bin','partition-table.bin','rfsentinel.bin']){
  const b=new Uint8Array(fs.readFileSync(path.join(RACINE,'webtool/firmware',p)));
  assert.equal(C.md5(b),crypto.createHash('md5').update(b).digest('hex'),'md5 '+p);
}
assert.equal(C.octets(21648),'21.1 Ko');
assert.equal(C.hexadecale(32768),'0x8000');
assert.equal(C.espace('<b>&"x"'),'&lt;b&gt;&amp;&quot;x&quot;');

/* 2 — plan impose : adresses et reglages memoire figes dans la page. */
assert.deepEqual(C.PLAN_IMPOSE.images.map(x=>x.address),[0,32768,65536]);
assert.deepEqual([C.PLAN_IMPOSE.flashMode,C.PLAN_IMPOSE.flashFreq,C.PLAN_IMPOSE.flashSize],['dio','80m','16MB']);

/* 3 — manifeste : le plan vient toujours de PLAN_IMPOSE, seules les URL
   viennent du manifeste, et une puce etrangere est refusee. */
const m1=C.lireManifeste(JSON.stringify({name:'RFSentinel',version:'1.0.0',chipFamily:'ESP32-S3',parts:[
  {fichier:'bootloader.bin',url:'bootloader.bin',taille:21648,sha256:'aa'},
  {path:'partition-table.bin',size:3072,sha256:'bb'},
  {path:'rfsentinel.bin',size:1477360,sha256:'cc'}]}));
const plan1=C.planDepuisManifeste(m1,'https://x/raw/main/webtool/firmware/manifest.json');
assert.deepEqual(plan1.map(p=>p.address),[0,32768,65536]);
assert.deepEqual(plan1.map(p=>p.url),['https://x/raw/main/webtool/firmware/bootloader.bin',
  'https://x/raw/main/webtool/firmware/partition-table.bin','https://x/raw/main/webtool/firmware/rfsentinel.bin']);
assert.equal(plan1[2].sha256,'cc');
assert.equal(plan1[2].taille,1477360);
const m2=C.lireManifeste({chipFamily:'esp32s3',parts:[{name:'bootloader',path:'b.bin',url:'https://y/b.bin'},
  {name:'partition-table',path:'p.bin',offset:999},{name:'app',path:'r.bin',flashSize:'keep'}]});
const plan2=C.planDepuisManifeste(m2,'https://z/a/manifest.json');
assert.equal(plan2[0].url,'https://y/b.bin');
assert.equal(plan2[1].url,'https://z/a/p.bin');
assert.equal(plan2[2].url,'https://z/a/r.bin');
assert.deepEqual(plan2.map(p=>p.address),[0,32768,65536],'les offsets du manifeste sont ignores');
assert.throws(()=>C.lireManifeste({chipFamily:'ESP32-C3',parts:[{}]}),/ESP32-S3/);
assert.throws(()=>C.lireManifeste('pas du json'),/JSON/);
assert.throws(()=>C.lireManifeste({chipFamily:'ESP32-S3'}),/images/);
assert.deepEqual(C.empreinteAttendue(plan1),[]);
assert.deepEqual(C.empreinteAttendue([{fichier:'a.bin',sha256:''}]),['a.bin']);

/* 4 — controle de coherence : seule la barriere contre une carte 8 Mo. */
assert.equal(C.controlerPuce('ESP32-S3 (QFN56)','16MB').bloque,false);
assert.equal(C.controlerPuce('ESP32-S3','16MB').avertissements.length,0);
assert.equal(C.controlerPuce('ESP32-S3','8MB').bloque,true);
assert.match(C.controlerPuce('ESP32-C3','16MB').motifs[0],/ESP32-S3/);
assert.equal(C.controlerPuce('ESP32-C3','8MB').motifs.length,2);
assert.equal(C.controlerPuce('ESP32-S3','').avertissements.length,1);
assert.equal(C.controlerPuce('ESP32-S3','').bloque,false);
assert.equal(C.controlerPuce('ESP32-S3','32MB').avertissements.length,1);
assert.equal(C.controlerPuce('ESP32-S3','16MB').tailleOctets,16777216);

/* 5 — progression : esptool-js rend des octets compresses, la barre doit
   rester fidele aux tailles reelles des images. */
const imgs=[{taille:21648},{taille:3072},{taille:1477360}];
let pr=C.progression(imgs,0,1000,1000);
assert.equal(Math.round(pr.part*100),100);
assert.ok(Math.abs(pr.total-(21648/1502080))<1e-12);
pr=C.progression(imgs,2,500,1000);
assert.ok(Math.abs(pr.total-((21648+3072+0.5*1477360)/1502080))<1e-12);
assert.equal(C.progression(imgs,1,0,10).part,0);
assert.deepEqual(C.progression([],0,0,0),{part:0,image:0,total:0});

/* 6 — adresse proposee par defaut selon le contexte d'ouverture. */
assert.equal(C.urlParDefaut(),'https://raw.githubusercontent.com/'+C.DEPOT+'/'+C.BRANCHE+'/webtool/firmware/manifest.json');
contexte.location={protocol:'https:',href:'https://p.github.io/rfsentinel/webtool/index.html'};
assert.equal(C.urlParDefaut(),'https://p.github.io/rfsentinel/webtool/firmware/manifest.json');
contexte.location={protocol:'http:',href:'http://localhost:8000/webtool/index.html'};
assert.equal(C.urlParDefaut(),'http://localhost:8000/webtool/firmware/manifest.json');

/* 7 — le manifeste emballe est lisible par la page et ses empreintes sont
   celles des fichiers cotes. */
const emballe=JSON.parse(fs.readFileSync(path.join(RACINE,'webtool/firmware/manifest.json'),'utf8'));
const m3=C.lireManifeste(emballe);
const plan3=C.planDepuisManifeste(m3,'https://raw.githubusercontent.com/o/r/main/webtool/firmware/manifest.json');
assert.deepEqual(plan3.map(p=>p.address),[0,32768,65536]);
assert.equal(plan3.length,3);
assert.deepEqual(C.empreinteAttendue(plan3),[],'le manifeste doit porter les trois empreintes');
const sommes=fs.readFileSync(path.join(RACINE,'webtool/firmware/SHA256SUMS'),'utf8').trim().split(String.fromCharCode(10));
assert.equal(sommes.length,3);
for(const ligne of sommes){
  const [digest,nom]=ligne.split(/\s+/);
  const b=fs.readFileSync(path.join(RACINE,'webtool/firmware',nom));
  assert.equal(digest,crypto.createHash('sha256').update(b).digest('hex'),'SHA256SUMS '+nom);
  const dansPlan=plan3.find(p=>p.fichier===nom);
  assert.equal(dansPlan.sha256,digest,'manifeste '+nom);
  assert.equal(dansPlan.taille,b.length,'taille '+nom);
}

/* 8 — la page ne promet rien d'inexistant : tous les ids utilises existent. */
const ids=[...new Set([...script.matchAll(/el\("([^"]+)"\)/g)].map(x=>x[1]))];
const presents=new Set([...page.matchAll(/id="([^"]+)"/g)].map(x=>x[1]));
const manquants=ids.filter(i=>!presents.has(i));
assert.deepEqual(manquants,[],'ids references sans balise : '+manquants.join(', '));
assert.ok(page.includes('vendor/esptool-js/bundle.global.js'),'le moteur doit etre charge en local');
assert.ok(!/cdn\.jsdelivr|unpkg|googleapis/.test(page),'aucun CDN dans la page');

/* 9 — les deux sources de images sont independantes : changer de source ne
   peut plus effacer le contenu prepare sur l'autre. */
const srcDepot=C.document.getElementById('src-depot');
const srcLocal=C.document.getElementById('src-local');
srcDepot.checked=true; srcLocal.checked=false;
assert.equal(C.imagesPretes(),false,'aucune image au depart');
const mSource=C.lireManifeste({name:'RFSentinel',version:'1.0.0',chipFamily:'ESP32-S3',parts:[
  {name:'bootloader',path:'bootloader.bin',offset:0,size:21648,sha256:'aa'},
  {name:'partition-table',path:'partition-table.bin',offset:32768,size:3072,sha256:'bb'},
  {name:'app',path:'rfsentinel.bin',offset:65536,size:1477360,sha256:'cc'}]});
C.etat.sources.depot.manifeste=mSource;
C.etat.sources.depot.plan=C.planDepuisManifeste(mSource,'https://x/webtool/firmware/manifest.json');
assert.equal(C.imagesPretes(),true,'le manifeste charge rend les images pretes');
srcDepot.checked=false; srcLocal.checked=true;
assert.equal(C.imagesPretes(),false,'le mode fichiers vierge reste bloque');
srcDepot.checked=true; srcLocal.checked=false;
assert.equal(C.imagesPretes(),true,'le manifeste survit a un aller-retour vers le mode fichiers');
assert.equal(C.etat.sources.depot.plan.length,3,'le plan du depot n a pas ete efface');
assert.equal(C.etat.sources.local.plan.length,0,'le mode fichiers n a rien herite du depot');

/* 10 — bilingue : les deux dictionnaires couvrent les memes cles, et tout ce
   que la page ecrit pendant une session est reconstruit a la demande. */
const clesFR=Object.keys(C.I18N.fr).sort(),clesEN=Object.keys(C.I18N.en).sort();
assert.deepEqual(clesFR,clesEN,'les deux dictionnaires ont les memes cles');
assert.ok(clesFR.length>=101,'les dictionnaires doivent etre complets');
const sansTexte=C.IDS_TEXTE.filter(i=>C.I18N.fr[i]===undefined||C.I18N.en[i]===undefined);
assert.deepEqual(sansTexte,[],'ids sans traduction : '+sansTexte.join(', '));
const sansTitre=C.IDS_TITRE.map(t=>'titre-'+t).filter(t=>C.I18N.fr[t]===undefined||C.I18N.en[t]===undefined);
assert.deepEqual(sansTitre,[],'titres sans traduction : '+sansTitre.join(', '));
C.appliquerLangue('en');
assert.equal(C.langue(),'en','la langue bascule');
assert.match(C.T('err-puce'),/ESP32-S3/,'le nom de puce reste en anglais');
assert.match(C.T('err-manifeste-json'),/JSON/);
assert.match(C.T('err-manifeste-images'),/images/);
assert.equal(C.T('lib-image-adresse','rfsentinel.bin','0x10000'),'rfsentinel.bin at 0x10000');
C.appliquerLangue('fr');
assert.equal(C.langue(),'fr');
assert.match(C.T('err-puce'),/ESP32-S3/);
assert.match(C.T('err-manifeste-json'),/JSON/);
assert.match(C.T('err-manifeste-images'),/images/);
assert.equal(C.T('lib-image-adresse','rfsentinel.bin','0x10000'),'rfsentinel.bin à 0x10000');
assert.equal(C.octets(21648),'21.1 Ko','les tailles ne dependent pas de la langue');

/* Les messages de controle suivent la langue choisie, y compris apres coup. */
C.appliquerLangue('en');
assert.match(C.controlerPuce('ESP32-C3','16MB').motifs[0],/built for ESP32-S3/);
assert.match(C.controlerPuce('ESP32-S3','8MB').motifs[0],/will not boot/);
assert.equal(C.controlerPuce('ESP32-C3','8MB').motifs.length,2,'deux refus cumules');
assert.throws(()=>C.lireManifeste({chipFamily:'ESP32-C3',parts:[{}]}),/ESP32-S3/);
assert.throws(()=>C.lireManifeste('nope'),/JSON/);
assert.throws(()=>C.lireManifeste({chipFamily:'ESP32-S3'}),/images/);
C.appliquerLangue('fr');
assert.match(C.controlerPuce('ESP32-C3','16MB').motifs[0],/compilé pour ESP32-S3/);

/* Le refus, les infos puce et le resultat partent de l'etat : changer de
   langue les retraduit sans relire la carte. */
C.etat.controleBrut={puce:'ESP32-S3 (QFN56) ESP32-S3',taille:'8MB'};
C.etat.controle=C.controlerPuce(C.etat.controleBrut.puce,C.etat.controleBrut.taille);
C.etat.gateBrut=null;
C.appliquerLangue('fr');
assert.match(C.document.getElementById('gate').innerHTML,/Écriture refusée/);
C.appliquerLangue('en');
assert.match(C.document.getElementById('gate').innerHTML,/Writing refused/);
assert.match(C.document.getElementById('gate').innerHTML,/will not boot/);
C.etat.infosPuce={puce:'ESP32-S3 (QFN56)',taille:'',mac:'',options:'X'};
C.appliquerLangue('fr');
assert.match(C.document.getElementById('infos-puce').innerHTML,/non lue/);
C.appliquerLangue('en');
assert.match(C.document.getElementById('infos-puce').innerHTML,/not read/);
C.etat.resultat={cle:'res-succes',classe:'reussie',args:['1.0.0'],suite:'res-succes-conserve'};
C.appliquerLangue('fr');
assert.match(C.document.getElementById('resultat').innerHTML,/journal est conservé/);
C.appliquerLangue('en');
assert.match(C.document.getElementById('resultat').innerHTML,/log is kept/);
C.etat.controleBrut=null;
C.etat.gateBrut={cle:'gate-connexion',args:['port busy'],classe:'erreur'};
C.appliquerLangue('fr');
assert.match(C.document.getElementById('gate').innerHTML,/Connexion impossible/);
C.appliquerLangue('en');
assert.match(C.document.getElementById('gate').innerHTML,/Cannot connect/);
assert.match(C.document.getElementById('gate').innerHTML,/port busy/);
C.basculerLangue('fr');
assert.equal(C.langue(),'fr','le choix est memorise');
C.appliquerLangue('en');

/* 11 — mode hors ligne : les trois fichiers choisis sur disque produisent le
   meme plan impose, sans empreinte SHA-256 publiee. */
const fauxFichier=(nom,taille)=>({name:nom,arrayBuffer:async()=>new Uint8Array(taille).buffer});
const fichiers={'f-boot':[fauxFichier('bootloader.bin',21648)],'f-part':[fauxFichier('partition-table.bin',3072)],'f-app':[fauxFichier('rfsentinel.bin',3072)]};
(async function(){
  for(const id of Object.keys(fichiers)){C.document.getElementById(id).files=fichiers[id];}
  await C.lireFichiersChoisis();
  assert.equal(C.imagesPretes(),true,'le mode fichiers est pret');
  const local=C.etat.sources.local;
  assert.deepEqual(local.images.map(x=>x.address),[0,32768,65536],'adresses imposees en mode fichiers');
  assert.deepEqual(local.images.map(x=>x.taille),[21648,3072,3072],'tailles lues depuis le disque');
  assert.deepEqual(local.plan.map(x=>x.sha256),['','',''],'aucune empreinte publiee en mode fichiers');
  assert.equal(local.manifeste.local,true,'le resume doit prevenir de l absence d empreinte');
  assert.equal(C.etat.sources.depot.plan.length,3,'le manifeste du depot reste intact');
  console.log('Flasher navigateur: MD5, plan impose, manifeste, controle 8/16 Mo, progression, manifeste emballe, sources independantes, bilingue FR/EN, mode hors ligne et ids: OK');
})().catch(function(e){console.error('mode hors ligne: '+(e&&e.message));process.exit(1);});
