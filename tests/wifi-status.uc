'use strict';
let radio={pending:true,up:false,interfaces:[]}, saved={}, enabled=true;
const ctx={
 cursor:()=>({get:(config,section,key)=>key=='disabled'?(enabled?'0':'1'):null}),
 ubus_call:()=>({radio0:radio}),
 state_get:n=>saved[n], state_put:(n,v)=>{saved[n]=v;}, stat:()=>({mtime:1})
};
let status=loadfile('/test/wifi-under-test.uc',{raw_mode:true})()(ctx);
assert(status().state=='starting','pending startup not shown');
saved['wifi-start']={at:time()-31,revision:1};
let r=status();assert(r.state=='failed'&&r.error=='Wireless startup timed out','pending flag remained indefinitely');
saved={};radio.errors=[{code:'NO_RADIO'}];
r=status();assert(r.state=='failed'&&r.error=='NO_RADIO','native startup error hidden');
radio={up:true,interfaces:[{}]};assert(status().state=='on','working hotspot not shown');
enabled=false;assert(status().state=='off','disabled hotspot reported a failure');
print('Wireless startup state checks passed\n');
