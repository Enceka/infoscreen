'use strict';
let started=0, error=null;
const ctx = {
 run: cmd => 0,
 read_trim: path => null,
 read_num: path => null,
 sh: cmd => '',
 sh_json: cmd => null,
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
assert(index(filter(items,i=>i.id=='diagnosis')[0].value.zh,'DHCP')>=0,'diagnosis not shown');
assert(started==0,'reading settings reset hardware');
assert(usb.set('reset')==null&&started==1,'button did not queue recovery');
error='USB controller unavailable';assert(usb.set('reset')==error,'error was hidden');
assert(usb.set('unknown')=='no such setting','unknown action accepted');
print('USB settings checks passed\n');
