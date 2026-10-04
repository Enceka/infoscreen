// All actions are mocked; no USB device, network service or modem is touched.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require(process.env.E5_PLAYWRIGHT||'playwright');
const www=path.resolve(__dirname,'../root/usr/share/e5-infoscreen/www');
(async()=>{
 const browser=await chromium.launch({headless:true}),page=await browser.newPage({viewport:{width:320,height:480}});
 let resets=0,checked=0,busy=false,errors=[];
 page.on('pageerror',e=>errors.push(e.message));
 const items=()=>[
  {id:'check',type:'action',label:{zh:'一键检查连接'},reload:true},
  {id:'diagnosis',type:'info',label:{zh:'检查结论'},value:checked?'USB 尚未识别；热点尚未运行':'--'},
  {id:'reset_status',type:'info',label:{zh:'USB 重置结果'},value:busy?'正在修复':'已重新枚举',busy},
  {id:'reset',type:'action',label:{zh:'重置 USB 连接'},confirm:true,reload:true}
 ];
 await page.route('http://screen.test/**',async route=>{
  const p=new URL(route.request().url()).pathname;
  if(p==='/api/status')return route.fulfill({json:{time:0,tz_offset:0,modem:{present:false,sim:false,signal:{}},wan:{up:false},traffic:{rx_total:0,tx_total:0,rx_rate:0,tx_rate:0},wifi:{enabled:true,up:false,state:'failed',error:'NO_RADIO'},clients:[],battery:{capacity:80},usb:{link:'host',reset:{busy}},system:{temperatures:{}},screen:{idle:60,brightness:120,lang:'zh',touch:true,donate_seen:true},sms:{unread:[]},notifications:[]}});
  if(p==='/api/settings')return route.fulfill({json:{categories:[{id:'usb',label:{zh:'USB'}}]}});
  if(p==='/api/settings/usb'){
   if(route.request().method()==='POST'){
    const b=route.request().postDataJSON();if(b.id==='reset'){resets++;busy=true;}else checked++;
    return route.fulfill({json:{ok:true,item:items().find(i=>i.id===b.id)}});
   }
   return route.fulfill({json:{items:items()}});
  }
  if(p.startsWith('/api/'))return route.fulfill({json:{ok:true,plugins:[],available:false}});
  const file=path.join(www,p);
  if(!fs.existsSync(file))return route.fulfill({status:404,body:''});
  return route.fulfill({contentType:p.endsWith('.js')?'text/javascript':p.endsWith('.css')?'text/css':'text/html',body:fs.readFileSync(file)});
 });
 async function physical(key){await page.evaluate(key=>document.dispatchEvent(new KeyboardEvent('keydown',{key,bubbles:true,cancelable:true})),key);}
 await page.goto('http://screen.test/index.html');await page.waitForFunction(()=>document.getElementById('ov-wifi').textContent.includes('未启动'));
 await physical('5');assert.equal(await page.locator('#hs-qr').getAttribute('data-off'),'NO_RADIO');
 await physical('8');await page.waitForSelector('[data-st="cat:0"]');await page.click('[data-st="cat:0"]');
 await page.waitForSelector('[data-st="item:check"]');assert.equal(resets,0,'viewing diagnostics reset hardware');
 await page.click('[data-st="item:check"]');await page.waitForFunction(()=>document.getElementById('st-view').textContent.includes('热点尚未运行'));
 assert.equal(checked,1);assert.equal(resets,0);
 await page.click('[data-st="item:reset"]');assert.equal(resets,0,'first confirmation started recovery');
 await page.click('[data-st="item:reset"]');await page.waitForFunction(()=>document.getElementById('toast').textContent.includes('USB 修复已启动'));
 assert.equal(resets,1);busy=false;
 await page.waitForFunction(()=>document.getElementById('st-view').textContent.includes('已重新枚举'));
 assert.deepEqual(errors,[]);console.log('Connection recovery UI checks passed');await browser.close();
})().catch(e=>{console.error(e);process.exit(1);});
