// Read-only connection checks. Save a bounded report without Wi-Fi credentials.
'use strict';
import { readfile, writefile, mkdir, stat, popen } from 'fs';

function command(cmd) {
	// Group pipelines so failures from logread/dmesg stay inside the report,
	// rather than being printed before its JSON and breaking RPC parsing.
	let p=popen(`(${cmd}) 2>&1`), out=p ? p.read('all') : '';
	return { output:trim(out ?? ''), rc:p ? p.close() : -1 };
}
function redact(value) {
	if (type(value)=='array') return map(value, redact);
	if (type(value)!='object') return value;
	let out={};
	for (let key,item in value)
		if (!match(lc(key), /^(key|password|psk|sae_password|private_key|passphrase)$/)) out[key]=redact(item);
	return out;
}
function logs(cmd) {
	return join('\n', map(split(command(cmd).output,'\n'), line =>
		match(lc(line), /(password|passphrase|psk=|key=)/) ? '[authentication detail omitted]' : line));
}
function trimmed(path) { return trim(readfile(path) ?? ''); }

let checks=[], details={};
let u=command('timeout 5 ubus call network.wireless status');
let wireless=null;
try { wireless=redact(json(u.output)); } catch(e) {}
details.wireless=wireless;
details.wireless_query_exit=u.rc;
details.usb_controller=command('ls /sys/class/udc').output;
let controllers=split(details.usb_controller,'\n');
let udc=length(controllers) ? controllers[0] : '';
details.usb_state=udc ? trimmed(`/sys/class/udc/${udc}/state`) : '';
details.gadget_controller=trimmed('/sys/kernel/config/usb_gadget/linux/UDC');
if (!udc || !stat(`/sys/class/udc/${udc}`)) push(checks,'no_controller');
else if (details.usb_state!='configured') push(checks,'usb_not_enumerated');
if (!details.gadget_controller) push(checks,'gadget_unbound');
if (!stat('/sys/class/net/usb0')) push(checks,'usb_interface_missing');
if (!stat('/sys/class/net/br-lan')) push(checks,'lan_missing');
else if (!stat('/sys/class/net/br-lan/brif/usb0')) push(checks,'usb_not_bridged');
details.lan_ipv4=command('ip -4 -o addr show dev br-lan').output;
if (!match(details.lan_ipv4, / inet /)) push(checks,'lan_no_ipv4');
details.dhcp=command('/etc/init.d/dnsmasq status');
if (details.dhcp.rc!=0) push(checks,'dhcp_not_running');
let radio=wireless?.radio0;
let disabled=command('uci -q get wireless.radio0.disabled').output=='1' || command('uci -q get wireless.default_radio0.disabled').output=='1';
if (!disabled && (!radio?.up || !length(radio?.interfaces ?? []))) push(checks,'wifi_not_running');
details.native_errors=radio?.errors ?? [];
details.modules=filter(split(readfile('/proc/modules') ?? '', '\n'), line=>match(line,/^(wcn_bsp|sprd_wlan_combo|cfg80211|libcomposite|usb_f_ncm) /));
details.firmware={wifi:stat('/lib/firmware/wcnmodem.bin')!=null, regulatory:stat('/lib/firmware/regulatory.db')!=null};
if (!details.firmware.wifi) push(checks,'wifi_firmware_missing');
if (!details.firmware.regulatory) push(checks,'regulatory_missing');
details.versions={image:trimmed('/etc/e5/image-version'), kernel:trimmed('/proc/sys/kernel/osrelease'), screen:trimmed('/usr/share/e5-infoscreen/VERSION')};
details.hostapd_log=logs('logread -e hostapd | tail -30');
details.netifd_log=logs('logread -e netifd | tail -30');
details.dhcp_log=logs('logread -e dnsmasq | tail -20');
details.kernel_log=logs("dmesg | grep -iE 'usb|gadget|musb|ncm|wcn|sprd_wlan|cfg80211|firmware' | tail -50");
let result={ok:true,checked_at:time(),checks,details};
let directory='/etc/e5-infoscreen/diagnostics';
if (!stat('/etc/e5-infoscreen')) mkdir('/etc/e5-infoscreen');
if (!stat(directory)) mkdir(directory);
result.report=`${directory}/network.json`;
if (!writefile(result.report,sprintf('%J\n',result))) {
	result.ok=false; result.error='Cannot save the network diagnostic report';
}
print(sprintf('%J\n',result));
