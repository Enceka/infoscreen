const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require(process.env.E5_PLAYWRIGHT||'playwright');
const www=path.resolve(__dirname,'../root/usr/share/e5-infoscreen/www');
(async()=>{
 const browser=await chromium.launch({headless:true});const page=await browser.newPage({viewport:{width:320,height:480}});
 let notices=[],actions=[],errors=[];page.on('pageerror',e=>errors.push(e.message));
 const plugins=['fixture','other'].map(id=>({id,api_version:2,name:{zh:id,en:id},entry:'index.html'}));
 await page.route('http://screen.test/**',async route=>{
  const url=new URL(route.request().url()),p=url.pathname;
  if(p.startsWith('/api/')){
   if(route.request().method()==='POST')actions.push(p);
   if(p==='/api/status')return route.fulfill({json:{time:0,tz_offset:0,modem:{present:false,sim:false,signal:{}},wan:{up:false},traffic:{rx_total:0,tx_total:0,rx_rate:0,tx_rate:0},wifi:{enabled:false},clients:[],battery:{capacity:80},usb:{},system:{temperatures:{}},screen:{idle:60,brightness:120,lang:'zh',touch:true,donate_seen:true,lock_enabled:false},sms:{unread:[]},notifications:notices}});
   if(p==='/api/plugins')return route.fulfill({json:{plugins}});
   if(p==='/api/update')return route.fulfill({json:{current:'1.5.0',available:false}});
   return route.fulfill({json:{ok:true,categories:[]}});
  }
  if(p.startsWith('/plugins/'))return route.fulfill({contentType:'text/html',body:'<button id="danger">action</button><script src="/sdk/e5.js"></script><script>e5.onKey(k=>{if(k.kind==="ok"){e5.api("/action",{body:{}});return true;}return false;});</script>'});
  const file=path.join(www,p);if(!fs.existsSync(file))return route.fulfill({status:404,body:''});
  return route.fulfill({contentType:p.endsWith('.js')?'text/javascript':p.endsWith('.css')?'text/css':'text/html',body:fs.readFileSync(file)});
 });
 await page.goto('http://screen.test/index.html');await page.waitForTimeout(1000);
 notices=[{plugin:'fixture',id:'call',title:{zh:'来电',en:'Incoming'},body:'<script>unsafe</script>',wake:true}];
 await page.waitForSelector('#notification-card:not([hidden])');
 assert.equal(await page.locator('#notification-body').textContent(),'<script>unsafe</script>');
 assert(!actions.some(p=>p.startsWith('/api/plugins/')),'notification triggered an action');
 await page.click('#notification-open');await page.waitForSelector('#app-frame:not([hidden])');
 assert((await page.locator('#app-frame').getAttribute('src')).includes('/fixture/'));
 notices=[{plugin:'other',id:'message',title:'Message',body:'notice',wake:false}];
 await page.waitForSelector('#notification-card:not([hidden])');await page.waitForTimeout(300);
 const frame=page.frames().find(f=>f.url().includes('/plugins/fixture/'));
 await frame.evaluate(()=>document.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',keyCode:13,bubbles:true})));
 await page.waitForTimeout(500);
 assert(!actions.some(p=>p.endsWith('/action')),'notification overlay leaked confirm into plugin');
 assert.deepEqual(errors,[]);console.log('Generic notification UI checks passed');await browser.close();
})().catch(e=>{console.error(e);process.exit(1);});
