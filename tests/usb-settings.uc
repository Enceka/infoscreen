'use strict';
let started=0, error=null, status=null, commands=[];
const ctx = {
 run: cmd => 0,
 read_trim: path => null,
 read_num: path => null,
 sh: cmd => '',
 sh_json: cmd => {push(commands,cmd);return status;},
 uci: () => ({get: () => null, foreach: () => null}),
 usb_status: () => ({state:'configured',ip:'192.168.9.2',reset:{busy:false,state:'done',stage:'complete',message:'done'}}),
 usb_reset_start: () => {started++;return error;},
 network_check_status:()=>({checks:['usb_not_enumerated','dhcp_not_running'],report:'/etc/e5-infoscreen/diagnostics/network.json'}),
 network_check:()=>null
};
let categories=loadfile('/usr/share/e5-infoscreen/settings.uc',{raw_mode:true})()(ctx);
let usb=filter(categories,c=>c.id=='usb')[0];assert(usb,'USB category missing');
let items=usb.items();assert(filter(items,i=>i.id=='reset'&&i.confirm&&i.reload)[0],'recovery action missing');
assert(filter(items,i=>i.id=='check')[0],'read-only check action missing');
assert(filter(items,i=>i.id=='diagnosis')[0].value.zh=='电脑尚未识别 USB；DHCP 服务未运行','diagnosis not shown');
assert(started==0,'reading settings reset hardware');
assert(usb.set('reset')==null&&started==1,'button did not queue recovery');
error='USB controller unavailable';assert(usb.set('reset')==error,'error was hidden');
// Unknown/read-only IDs are rejected before consulting hardware, even when
// its state is unavailable. A caller must never dispatch them as an action.
for (let available in [null,{ok:true}]) {
 status=available;
 for (let id in ['unknown','diagnosis','controller']) {
  commands=[];
  assert(usb.set(id)=='no such setting','unknown action accepted');
  assert(length(commands)==0,'unknown action queried hardware');
 }
}
status=null;
assert(usb.set('usb_role','device')=='USB status unavailable','valid action lost unavailable status');
status={ok:true};commands=[];
assert(usb.set('usb_role','invalid')=='Invalid USB setting','invalid role accepted');
assert(length(commands)==1,'invalid role dispatched a control command');
print('USB settings checks passed\n');
