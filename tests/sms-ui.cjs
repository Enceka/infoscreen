// Synthetic messages only; requests never leave screen.test.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require(process.env.E5_PLAYWRIGHT||'playwright');
const www=path.resolve(__dirname,'../root/usr/share/e5-infoscreen/www');
(async()=>{
 const browser=await chromium.launch({headless:true}),page=await browser.newPage({viewport:{width:320,height:480}});
 let unread=[],messages=[],errors=[],reads=0;
 page.on('pageerror',e=>errors.push(e.message));
 await page.route('http://screen.test/**',async route=>{
  const p=new URL(route.request().url()).pathname;
  if(p==='/api/status')return route.fulfill({json:{time:0,tz_offset:0,modem:{present:false,sim:false,signal:{}},wan:{up:false},traffic:{rx_total:0,tx_total:0,rx_rate:0,tx_rate:0},wifi:{enabled:false},clients:[],battery:{capacity:80},usb:{},system:{temperatures:{}},screen:{idle:60,brightness:120,lang:'zh',touch:true,donate_seen:true},sms:{unread,screen:true},notifications:[]}});
  if(p==='/api/sms')return route.fulfill({json:{messages}});
  if(p==='/api/sms-read'){reads++;unread=[];return route.fulfill({json:{ok:true}});}
  if(p.startsWith('/api/'))return route.fulfill({json:{ok:true,plugins:[],available:false}});
  const file=path.join(www,p);if(!fs.existsSync(file))return route.fulfill({status:404,body:''});
  return route.fulfill({contentType:p.endsWith('.js')?'text/javascript':p.endsWith('.css')?'text/css':'text/html',body:fs.readFileSync(file)});
 });
 await page.goto('http://screen.test/index.html');await page.waitForTimeout(800);
 // A larger ID can have an older network timestamp: select by time.
 messages=[{id:1000000000,sim:'SIM2',card:1,number:'10010',time:'2026-10-05T20:00:00+08:00',text:'头部<script>safe</script>',state:'receiving',unread:true},
 {id:1000000001,sim:'SIM1',card:0,number:'10086',time:'2026-10-05T19:00:00+08:00',text:'older',state:'received',unread:true}];
 unread=messages.map(m=>m.id);
 await page.waitForFunction(()=>document.getElementById('sv-time').textContent.includes('SIM2'));
 assert((await page.locator('#toast').textContent()).includes('SIM2'));
 assert.equal(await page.locator('#sv-text').textContent(),'头部<script>safe</script>');
 assert.equal(await page.locator('#sv-text script').count(),0);
 messages[0]={...messages[0],text:'头部<script>safe</script>尾部',state:'received',unread:false};
 await page.waitForFunction(()=>document.getElementById('sv-text').textContent.endsWith('尾部'));
 await page.click('#sv-back');
 assert((await page.locator('[data-sms="1000000000"]').textContent()).includes('SIM2'));
 assert((await page.locator('[data-sms="1000000001"]').textContent()).includes('SIM1'));
 assert.equal(reads,1);assert.deepEqual(errors,[]);
 console.log('Dual-SIM screen labels, chronological alerts, safe rendering and multipart refresh checks passed');
 await browser.close();
})().catch(e=>{console.error(e);process.exit(1);});
