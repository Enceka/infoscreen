// The info screen's settings ("高级"): categories of declarative items the page
// draws by type, and what reading and changing each one does.  Loaded by
// api.uc (loadfile) for every settings request and handed `ctx` -- the same
// helpers a plugin backend gets (docs/API.md, "Backend context").
//
// An item, as the page receives it:
//   { id, type, label, value, ... }
//   type toggle   value true|false
//        choice   value, options: [ { value, label } ]
//        number   value, min, max, step, unit
//        multi    value: [ ... ], options: [ { value, label } ] (none chosen = no lock)
//        action   no value; confirm: true -> the page asks for a second press
//        info     value is text, read-only
//        image    src: a picture the page shows full screen when the row is pressed
//   confirm      the change asks for a second press (it can cut the connection)
//   note         a line under the item (why, or what it does)
//   reload       after a change the page reads the category again (it shows in other items)
// label and note are { zh, en }.  A change is POST /api/settings/<category>
// { id, value } or { id } for an action; the reply is the item again, read back.

'use strict';

return function(ctx) {

const L = (zh, en) => ({ zh, en });

/* ---------- network ---------- */

// the bands Android's RIL itself allows on this modem (unisoc-cpd FINDINGS
// 1.4), and the NR bands in the CP's tables that Chinese networks use
const LTE_CHOICES = [ 1, 3, 5, 7, 8, 20, 28, 34, 38, 39, 40, 41 ];
const LTE_ANDROID = LTE_CHOICES;
const NR_CHOICES = [ 1, 3, 5, 8, 28, 41, 77, 78, 79 ];

// AT+SPTESTMODE=<card 1>,<card 2>,<primary SIM>: the work mode of each card
// (the values UFI-TOOLS uses on this CP generation, uficode's
// UniSocCellularUtils.kt NetworkMode); the read form answers the same three
// first.  5G without 4G is SA: "5G only" is SA only.
const NET_MODES = [
	[ '134', L('5G/4G/3G 自动', '5G/4G/3G auto') ],
	[ '131', L('5G/4G', '5G/4G') ],
	[ '128', L('仅 5G (SA)', '5G only (SA)') ],
	[ '3', L('仅 4G', '4G only') ]
];

function testmode() {
	let w = ctx.payload_ints(ctx.at('AT+SPTESTMODE?'));
	return (length(w) >= 3) ? { card1: w[0], card2: w[1], primary: w[2] } : null;
}

// AT+SPLBAND=1,<49-64>,<33-48>,<17-32>,<1-16>,<65-80>  (unisoc-cpd lte_band_lock_command)
function lte_lock_cmd(bands) {
	// (ucode object keys are strings)
	let g = { '1': 0, '17': 0, '33': 0, '49': 0, '65': 0 };
	for (let b in bands) {
		for (let base in [ 1, 17, 33, 49, 65 ])
			if (b >= base && b < base + 16)
				g[`${base}`] |= 1 << (b - base);
	}
	return `AT+SPLBAND=1,${g['49']},${g['33']},${g['17']},${g['1']},${g['65']}`;
}

// AT+SPLBAND=2,<value1>,0,<value3>,<super>  (nr_band_lock_command)
function nr_lock_cmd(bands) {
	let v1 = 0, v3 = 0, sup = 0;
	for (let b in bands) {
		let i = index(ctx.NR_V1, b);
		if (i >= 0) { v1 |= 1 << i; continue; }
		i = index(ctx.NR_V3, b);
		if (i >= 0) { v3 |= 1 << i; continue; }
		i = index(ctx.NR_SUPER, b);
		if (i >= 0) sup |= 1 << i;
	}
	return `AT+SPLBAND=2,${v1},0,${v3},${sup}`;
}

function same_set(a, b) {
	a = sort([ ...a ]); b = sort([ ...b ]);
	if (length(a) != length(b))
		return false;
	for (let i = 0; i < length(a); i++)
		if (a[i] != b[i])
			return false;
	return true;
}

function apn_options() {
	// e5-linux's e5-apn-auto: the APN from the SIM's operator
	let seen = {}, out = [ { value: 'auto', label: L('自动（按 SIM 卡）', 'Automatic (from the SIM)') } ];
	let add = (apn) => {
		if (apn && apn != 'ims' && !seen[apn]) {
			seen[apn] = true;
			push(out, { value: apn, label: apn });
		}
	};
	add(ctx.uci().get('network', 'wan', 'apn'));
	// the contexts the modem holds (ims is the voice one)
	let pl = ctx.sh_json('mmcli -J -m any --timeout=5 --3gpp-profile-manager-list 2>/dev/null');
	for (let line in (pl?.modem?.['3gpp']?.['profile-manager']?.list ?? pl?.['modem.3gpp.profile-manager.list'] ?? [])) {
		let m = match(line, /apn: ([^,]+)/);
		if (m) add(trim(m[1]));
	}
	// and the ones listed for the switch: uci add_list e5-infoscreen.main.apn=<apn>
	for (let a in (ctx.uci().get('e5-infoscreen', 'main', 'apn') ?? []))
		add(a);
	return out;
}

// the cells to lock to: the serving one and its neighbours, of its technology
function cell_options(cells) {
	let out = [ { value: 'none', label: L('不锁定', 'Not locked') } ];
	for (let c in cells) {
		if (c.pci == null || c.arfcn == null || (c.type != '5gnr' && c.type != 'lte'))
			continue;
		let rat = (c.type == '5gnr') ? 'nr' : 'lte';
		let what = c.serving ? L('当前小区', 'serving') : L('邻区', 'neighbour');
		push(out, {
			value: `${rat}:${c.arfcn}:${c.pci}`,
			label: L(`${what.zh} ${rat == 'nr' ? 'NR' : 'LTE'} PCI ${c.pci} · ${c.arfcn}${c.rsrp != null ? ' · ' + c.rsrp + ' dBm' : ''}`,
				`${what.en} ${rat == 'nr' ? 'NR' : 'LTE'} PCI ${c.pci} · ${c.arfcn}${c.rsrp != null ? ' · ' + c.rsrp + ' dBm' : ''}`)
		});
	}
	return out;
}

function cell_lock_value() {
	let nr = ctx.cell_locks(ctx.at('AT+SPFORCEFRQ=16,3'));
	if (length(nr)) return `nr:${nr[0].arfcn}:${nr[0].pci}`;
	let lte = ctx.cell_locks(ctx.at('AT+SPFORCEFRQ=12,3'));
	if (length(lte)) return `lte:${lte[0].arfcn}:${lte[0].pci}`;
	return 'none';
}

const network = {
	id: 'network', label: L('网络', 'Network'),
	items: function() {
		let modem = ctx.modem_present();
		let cells = modem ? ctx.cells() : [];
		let cur_cell = modem ? cell_lock_value() : 'none';
		let tm = modem ? testmode() : null;
		let opts = cell_options(cells);
		// a lock on a cell that is not in view any more is still shown
		if (cur_cell != 'none' && !length(filter(opts, (o) => o.value == cur_cell))) {
			let p = split(cur_cell, ':');
			push(opts, { value: cur_cell, label: L(`已锁定 ${uc(p[0])} PCI ${p[2]} · ${p[1]}`, `locked ${uc(p[0])} PCI ${p[2]} · ${p[1]}`) });
		}
		return [
			{ id: 'sim_card', type: 'choice', label: L('SIM 卡', 'SIM card'), confirm: true,
			  value: ctx.uci().get('e5-sim', 'main', 'card') ?? `${ctx.sim_card()}`,
			  options: [ { value: '0', label: L('卡 1', 'SIM 1') }, { value: '1', label: L('卡 2', 'SIM 2') } ],
			  note: L('上网用的卡；切换时会重新连接网络', 'The card that carries data; the connection restarts on a change') },
			{ id: 'apn', type: 'choice', label: L('APN', 'APN'),
			  value: (ctx.uci().get('network', 'wan', 'apn_auto') == '1') ? 'auto' : ctx.uci().get('network', 'wan', 'apn'),
			  options: apn_options(), confirm: true,
			  note: L('切换后会重新连接网络', 'The connection restarts on a change') },
			{ id: 'net_mode', type: 'choice', label: L('网络模式', 'Network mode'), confirm: true,
			  value: (modem && tm) ? `${tm.card1}` : null,
			  options: map(NET_MODES, (m) => ({ value: m[0], label: m[1] })),
			  note: L('切换时会重新注册网络', 'The modem registers again on a change') },
			{ id: 'nr_mode', type: 'choice', label: L('5G 组网', '5G access'), confirm: true,
			  value: modem ? ((ctx.payload_ints(ctx.at('AT+SP5GRAN?'))[0] == 1) ? 'sa' : 'nsa') : null,
			  options: [ { value: 'sa', label: L('SA + NSA', 'SA + NSA') },
			             { value: 'nsa', label: L('仅 NSA', 'NSA only') } ] },
			{ id: 'lte_bands', type: 'multi', label: L('LTE 频段锁定', 'LTE band lock'), confirm: true,
			  value: modem ? ctx.lte_bands(ctx.at('AT+SPLBAND=0')) : [],
			  options: map(LTE_CHOICES, (b) => ({ value: b, label: `B${b}` })),
			  note: L('都不选 = 不锁定', 'None chosen = no lock') },
			{ id: 'nr_bands', type: 'multi', label: L('NR 频段锁定', 'NR band lock'), confirm: true,
			  value: modem ? ctx.nr_bands(ctx.at('AT+SPLBAND=3')) : [],
			  options: map(NR_CHOICES, (b) => ({ value: b, label: `n${b}` })),
			  note: L('都不选 = 不锁定', 'None chosen = no lock') },
			{ id: 'bands_default', type: 'action', label: L('恢复默认频段', 'Default bands'), confirm: true,
			  note: L('LTE 恢复 Android 的频段组合，NR 不锁定', "LTE back to Android's set, NR unlocked") },
			{ id: 'cell_lock', type: 'choice', label: L('锁定小区', 'Cell lock'), value: cur_cell,
			  options: opts, confirm: true,
			  note: L('锁到某个小区后只连这个小区（实验）', 'Only that cell once locked (experimental)') },
			{ id: 'reconnect', type: 'action', label: L('重新连接网络', 'Reconnect'), confirm: true }
		];
	},
	set: function(id, value) {
		if (id == 'sim_card') {
			if (value != '0' && value != '1') return 'unknown card';
			// e5-linux's e5-sim: the modem's port to that card, then wan again
			// (tens of seconds; the page sees the new card come up).  The
			// choice reads back at once: e5-sim keeps it in the same option.
			let c = ctx.uci();
			c.set('e5-sim', 'main', 'card', value);
			c.commit('e5-sim');
			ctx.forget('modem');
			ctx.run(`(/usr/sbin/e5-sim ${value}) >/dev/null 2>&1 &`);
			return null;
		}
		if (id == 'apn') {
			let ok = false;
			for (let o in apn_options())
				if (o.value == value) ok = true;
			if (!ok) return 'unknown APN';
			let c = ctx.uci();
			if (value == 'auto') {
				c.set('network', 'wan', 'apn_auto', '1');
				c.commit('network');
				// (sets the APN and reconnects when it differs)
				ctx.run('(/usr/libexec/e5-apn-auto) >/dev/null 2>&1 &');
				return null;
			}
			c.set('network', 'wan', 'apn', value);
			c.set('network', 'wan', 'apn_auto', '0');
			c.commit('network');
			ctx.run('(ifup wan) >/dev/null 2>&1 &');
			return null;
		}
		if (id == 'net_mode') {
			let ok = false;
			for (let m in NET_MODES) if (m[0] == `${value}`) ok = true;
			let tm = testmode();
			if (!ok || !tm) return ok ? 'cannot read the current mode' : 'unknown mode';
			// card 2's mode and the primary SIM stay as the modem has them
			ctx.at(`AT+SPTESTMODE=${int(value)},${tm.card2},${tm.primary}`);
			ctx.forget('modem');
			// the CP applies it a moment after the OK: read back for up to 3 s
			for (let i = 0; i < 6; i++) {
				let now = testmode();
				if (now && now.card1 == int(value)) return null;
				sleep(500);
			}
			return 'the modem did not take it';
		}
		if (id == 'nr_mode') {
			if (value != 'sa' && value != 'nsa') return 'unknown';
			let v = (value == 'sa') ? 1 : 0;
			ctx.at(`AT+SP5GRAN=${v}`);
			return (ctx.payload_ints(ctx.at('AT+SP5GRAN?'))[0] == v) ? null : 'the modem did not take it';
		}
		if (id == 'lte_bands' || id == 'nr_bands' || id == 'bands_default') {
			let lte = (id == 'bands_default') ? LTE_ANDROID : (id == 'lte_bands') ? value : null;
			let nr = (id == 'bands_default') ? [] : (id == 'nr_bands') ? value : null;
			if (lte != null) {
				lte = filter(map(lte, (b) => +b), (b) => index(LTE_CHOICES, b) >= 0);
				ctx.at(lte_lock_cmd(lte));
				// (a lock that did not take must not look like one)
				if (!same_set(ctx.lte_bands(ctx.at('AT+SPLBAND=0')), lte))
					return 'LTE: the modem read back other bands';
			}
			if (nr != null) {
				nr = filter(map(nr, (b) => +b), (b) => index(NR_CHOICES, b) >= 0);
				ctx.at(nr_lock_cmd(nr));
				if (!same_set(ctx.nr_bands(ctx.at('AT+SPLBAND=3')), nr))
					return 'NR: the modem read back other bands';
			}
			return null;
		}
		if (id == 'cell_lock') {
			if (value == 'none') {
				ctx.at('AT+SPFORCEFRQ=16,4');
				ctx.at('AT+SPFORCEFRQ=12,4');
				return (cell_lock_value() == 'none') ? null : 'still locked';
			}
			let p = split(value ?? '', ':');
			if (length(p) != 3 || (p[0] != 'nr' && p[0] != 'lte'))
				return 'bad cell';
			let arfcn = int(p[1]), pci = int(p[2]);
			ctx.at(`AT+SPFORCEFRQ=${p[0] == 'nr' ? 16 : 12},6,${arfcn},${pci}`);
			return (cell_lock_value() == `${p[0]}:${arfcn}:${pci}`) ? null : 'the modem did not take the lock';
		}
		if (id == 'reconnect') {
			ctx.forget('modem');
			ctx.run('(ifup wan) >/dev/null 2>&1 &');
			return null;
		}
		return 'no such setting';
	}
};

/* ---------- charging, notifications, screen: uci-backed ---------- */

// a setting that is a uci option: `apply` runs after a change
function uci_item(def) {
	let u = split(def.uci, '.');
	let raw = ctx.uci().get(u[0], u[1], u[2]) ?? def.default;
	let v = raw;
	if (def.type == 'toggle') v = (raw == '1');
	else if (def.type == 'number') v = +raw;
	// what the page sees: not the uci name, the apply command or the converters
	let out = { value: (def.to_page ? def.to_page(v) : v) };
	for (let k in [ 'id', 'type', 'label', 'note', 'confirm', 'options', 'min', 'max', 'step', 'unit' ])
		if (def[k] != null) out[k] = def[k];
	return out;
}

function uci_set(def, value) {
	let u = split(def.uci, '.');
	if (def.from_page) value = def.from_page(value);
	let v;
	if (def.type == 'toggle') v = value ? '1' : '0';
	else if (def.type == 'number') {
		v = +value;
		if (v != v || v < def.raw_min || v > def.raw_max) return 'out of range';
		v = `${int(v)}`;
	}
	else if (def.type == 'choice') {
		let ok = false;
		for (let o in def.options) if (`${o.value}` == `${value}`) ok = true;
		if (!ok) return 'not a choice';
		v = `${value}`;
	}
	let c = ctx.uci();
	c.set(u[0], u[1], u[2], v);
	c.commit(u[0]);
	if (def.apply) ctx.run(def.apply);
	return null;
}

function uci_category(id, label, defs, extra_items, extra_set) {
	return {
		id, label,
		items: function() {
			let out = map(defs, uci_item);
			if (extra_items) out = [ ...extra_items(), ...out ];
			return out;
		},
		set: function(item, value) {
			for (let d in defs)
				if (d.id == item) return uci_set(d, value);
			return extra_set ? extra_set(item, value) : 'no such setting';
		}
	};
}

const charge = uci_category('charge', L('充电', 'Charging'), [
	{ id: 'enabled', type: 'toggle', uci: 'e5-charge.main.enabled', default: '0',
	  label: L('充电上限', 'Charge limit'), apply: '/etc/init.d/e5-charge reload',
	  note: L('到上限停止充电，降到下限再充', 'Stop at the upper limit, charge again at the lower') },
	{ id: 'stop', type: 'number', uci: 'e5-charge.main.stop', default: '80', min: 50, max: 100, step: 5, unit: '%',
	  raw_min: 50, raw_max: 100, label: L('停止充电于', 'Stop at'), apply: '/etc/init.d/e5-charge reload' },
	{ id: 'start', type: 'number', uci: 'e5-charge.main.start', default: '70', min: 20, max: 95, step: 5, unit: '%',
	  raw_min: 20, raw_max: 95, label: L('恢复充电于', 'Charge again at'), apply: '/etc/init.d/e5-charge reload' }
], function() {
	let st = ctx.sh('/usr/libexec/e5-charge status 2>/dev/null') ?? '';
	let m = match(st, /capacity=([0-9]+) status=([^ ]+)/);
	let once = match(st, /full_once=1/);
	let paused = match(st, /stopped=1/);
	// (as the status bar has it, app.js chargeState: "Not charging" while current
	// flows in is charging, and only 100 % is full)
	let ma = +(trim(ctx.sh('cat /sys/class/power_supply/battery/current_now 2>/dev/null') ?? '') || '0') / 1000;
	let word = !m ? null : paused ? L('已暂停充电', 'paused')
		: (m[2] == 'Full' || +m[1] >= 100) ? L('已充满', 'full')
		: (m[2] == 'Charging' || (m[2] == 'Not' && ma > 20)) ? L('充电中', 'charging')
		: (m[2] == 'Discharging') ? L('使用电池', 'on battery')
		: (m[2] == 'Not') ? L('未充电', 'not charging') : L(m[2], m[2]);
	return [
		{ id: 'state', type: 'info', label: L('当前', 'Now'),
		  value: m ? L(`${m[1]}% · ${word.zh}`, `${m[1]}% · ${word.en}`) : '--' },
		{ id: 'full_once', type: 'action', label: once ? L('正在临时充满…', 'Charging to full…') : L('临时充满一次', 'Charge to full once'),
		  note: L('充到 100% 后回到上限', 'Back to the limit at 100 %') }
	];
}, function(item, value) {
	if (item != 'full_once') return 'no such setting';
	let c = ctx.uci();
	c.set('e5-charge', 'main', 'full_once', '1');
	c.commit('e5-charge');
	ctx.run('/etc/init.d/e5-charge reload');
	return null;
});

const notify = uci_category('notify', L('通知', 'Notifications'), [
	{ id: 'vibrate', type: 'toggle', uci: 'e5-notify.sms.vibrate', default: '1', label: L('短信震动', 'Vibrate on SMS') },
	{ id: 'screen', type: 'toggle', uci: 'e5-notify.sms.screen', default: '1', label: L('短信亮屏', 'Light up on SMS') },
	{ id: 'sound', type: 'toggle', uci: 'e5-notify.sms.sound', default: '0', label: L('短信提示音', 'Sound on SMS'),
	  note: L('按“声音”里的音量播放', 'At the volume set under Sound') }
], function() {
	return [ { id: 'test', type: 'action', label: L('试一下震动', 'Test the vibration') } ];
}, function(item, value) {
	if (item != 'test') return 'no such setting';
	ctx.run('(e5-sms-notify test) >/dev/null 2>&1 &');
	return null;
});

// the speaker (e5-linux's e5-audio and e5-volume): shown when it is there
const sound = uci_category('sound', L('声音', 'Sound'), [
	{ id: 'volume', type: 'number', uci: 'e5-audio.main.volume', default: '10', min: 0, max: 15, step: 1,
	  raw_min: 0, raw_max: 15, label: L('音量', 'Volume'), apply: '/usr/libexec/e5-volume apply',
	  note: L('音量键也可以调节；0 为静音', 'The volume keys change it too; 0 is mute') }
], function() {
	return [ { id: 'test', type: 'action', label: L('播放测试音', 'Play a test sound') } ];
}, function(item, value) {
	if (item != 'test') return 'no such setting';
	ctx.run('/usr/libexec/e5-volume play beep >/dev/null 2>&1');
	return null;
});
const has_sound = ctx.run('[ -x /usr/libexec/e5-volume ]') == 0;

const screen = uci_category('screen', L('屏幕', 'Screen'), [
	// shown in percent, stored as the backlight level 1-255
	{ id: 'brightness', type: 'number', uci: 'e5-infoscreen.main.brightness', default: '120',
	  min: 5, max: 100, step: 5, unit: '%', raw_min: 1, raw_max: 255, label: L('亮度', 'Brightness'),
	  to_page: (v) => int((v * 100 + 127) / 255), from_page: (p) => int((+p * 255 + 50) / 100) },
	{ id: 'idle', type: 'choice', uci: 'e5-infoscreen.main.idle', default: '60', label: L('自动息屏', 'Screen off after'),
	  options: [ { value: '15', label: L('15 秒', '15 s') }, { value: '30', label: L('30 秒', '30 s') },
	             { value: '60', label: L('1 分钟', '1 min') }, { value: '120', label: L('2 分钟', '2 min') },
	             { value: '300', label: L('5 分钟', '5 min') }, { value: '0', label: L('从不', 'Never') } ] },
	{ id: 'touch', type: 'toggle', uci: 'e5-infoscreen.main.touch', default: '1', label: L('触摸', 'Touch'),
	  note: L('关闭后只用按键操作', 'Off: the keys only') },
	{ id: 'lang', type: 'choice', uci: 'e5-infoscreen.main.lang', default: 'zh', label: L('语言', 'Language'),
	  options: [ { value: 'zh', label: L('中文', '中文') }, { value: 'en', label: L('English', 'English') } ] }
]);

/* ---------- system ---------- */

// zonename -> the POSIX TZ string OpenWrt keeps in system.timezone
const ZONES = [
	[ 'Asia/Shanghai', 'CST-8', L('中国 (北京时间)', 'China (Beijing)') ],
	[ 'Asia/Hong_Kong', 'HKT-8', L('中国香港', 'China (Hong Kong)') ],
	[ 'Asia/Taipei', 'CST-8', L('中国台湾', 'China (Taiwan)') ],
	[ 'Asia/Singapore', '<+08>-8', L('新加坡', 'Singapore') ],
	[ 'Asia/Tokyo', 'JST-9', L('东京', 'Tokyo') ],
	[ 'Asia/Seoul', 'KST-9', L('首尔', 'Seoul') ],
	[ 'Asia/Kolkata', 'IST-5:30', L('印度', 'India') ],
	[ 'Asia/Dubai', '<+04>-4', L('迪拜', 'Dubai') ],
	[ 'Europe/Moscow', 'MSK-3', L('莫斯科', 'Moscow') ],
	[ 'Europe/Berlin', 'CET-1CEST,M3.5.0,M10.5.0/3', L('中欧 (柏林)', 'Central Europe (Berlin)') ],
	[ 'Europe/London', 'GMT0BST,M3.5.0/1,M10.5.0', L('伦敦', 'London') ],
	[ 'America/New_York', 'EST5EDT,M3.2.0,M11.1.0', L('美国东部', 'US Eastern') ],
	[ 'America/Chicago', 'CST6CDT,M3.2.0,M11.1.0', L('美国中部', 'US Central') ],
	[ 'America/Los_Angeles', 'PST8PDT,M3.2.0,M11.1.0', L('美国西部', 'US Pacific') ],
	[ 'UTC', 'UTC0', L('UTC', 'UTC') ]
];

function system_section() {
	let name = null;
	ctx.uci().foreach('system', 'system', (s) => { name ??= s['.name']; });
	return name;
}

// when the image was built, in the device's time zone: /etc/e5/build-time
// (e5-linux's build-rootfs.sh writes it), else when its version file was
// written -- the same moment, for an image from before the file
function build_time() {
	let t = +(ctx.read_trim('/etc/e5/build-time') ?? '');
	if (!t) t = require('fs').stat('/etc/e5/image-version')?.mtime;
	if (!t) return null;
	let lt = localtime(t);
	return sprintf('%04d-%02d-%02d %02d:%02d', lt.year, lt.mon, lt.mday, lt.hour, lt.min);
}

// what the next reboot boots (e5-linux's e5-next-boot: the slots in misc as they
// stand -- a trial boot of Linux goes back to Android unless Linux is the default)
function next_boot() {
	let t = trim(ctx.sh('e5-next-boot next 2>/dev/null') ?? '');
	if (t == 'linux') return { value: t, label: L('Linux', 'Linux') };
	if (t == 'android') return { value: t, label: L('Android', 'Android') };
	return null;
}

// the default e5-next-boot keeps (/etc/e5linux/default-boot: linux re-arms slot b
// on every boot that comes up).  A trial image (e5.openwrt= on the command line,
// the mainline kernel's) is never the default: boot/init writes android there
function default_boot() {
	return ctx.read_trim('/etc/e5linux/default-boot') == 'linux' ? 'linux' : 'android';
}
function trial_image() {
	return match(ctx.read_trim('/proc/cmdline') ?? '', /(^| )e5\.openwrt=/) != null;
}

// the screen's own online update (/usr/libexec/e5-infoscreen/update): what the last check found,
// and whether an update or a rollback is running
const UPDATES = loadfile('/usr/share/e5-infoscreen/updates.uc', { raw_mode: true })()(ctx);
function update_state() {
	let s = UPDATES.state();
	let note;
	if (s.busy) note = L('正在更新，完成后屏幕会重新载入', 'Updating; the screen reloads when it is done');
	else if (s.checking) note = L('正在检查更新', 'Checking for updates');
	else if (s.available) note = L(`有新版本 ${s.latest.version}`, `${s.latest.version} is out`);
	else if (s.error) note = L(`检查失败：${s.error}`, `The check failed: ${s.error}`);
	else if (s.checked_at) note = L('已是最新版本', 'Up to date');
	else note = L('从 GitHub 获取最新的信息屏版本', 'Asks GitHub for the latest info screen');
	return { ...s, cur: s.current, note, newer: s.available && !s.busy && !s.checking };
}

const system_cat = {
	id: 'system', label: L('系统', 'System'),
	items: function() {
		let sec = system_section();
		let zone = sec ? ctx.uci().get('system', sec, 'zonename') : null;
		let next = next_boot();
		let u = update_state();
		return filter([
			{ id: 'timezone', type: 'choice', label: L('时区', 'Time zone'), value: zone ?? 'UTC',
			  options: map(ZONES, (z) => ({ value: z[0], label: z[2] })),
			  note: L('屏幕会重新载入', 'The screen reloads') },
			{ id: 'clock_seconds', type: 'toggle', label: L('时间显示秒', 'Clock with seconds'),
			  value: ctx.uci().get('e5-infoscreen', 'main', 'clock_seconds') == '1' },
			{ id: 'version', type: 'info', label: L('镜像版本', 'Image'), value: ctx.read_trim('/etc/e5/image-version') ?? '--' },
			{ id: 'built', type: 'info', label: L('构建时间', 'Built'), value: build_time() ?? '--' },
			{ id: 'screen_version', type: 'info', label: L('信息屏版本', 'Info screen'), value: u.cur },
			{ id: 'update_check', type: 'action', label: L('检查更新', 'Check for an update'), note: u.note },
			u.newer && u.latest ? { id: 'update_apply', type: 'action', confirm: true,
			  label: L(`更新到 ${u.latest.version}`, `Update to ${u.latest.version}`),
			  note: u.latest.notes ?? L('只更新信息屏，设置和已装应用保留', 'The screen only; settings and apps stay') } : null,
			u.backup && !u.busy ? { id: 'update_rollback', type: 'action', confirm: true,
			  label: L('回退信息屏', 'Roll the screen back'),
			  note: L('回到上一次更新之前的版本', 'Back to the version before the last update') } : null,
			{ id: 'traffic_clear', type: 'action', label: L('清空流量记录', 'Clear traffic records'), confirm: true,
			  note: L('今日、本月和每日的统计都从零开始', 'Today, this month and the days start from zero') },
			{ id: 'next_boot', type: 'info', label: L('下次重启进入', 'Next reboot boots'),
			  value: next ? next.label : '--' },
			trial_image()
			? { id: 'default_boot', type: 'info', label: L('默认启动', 'Default boot'), value: L('Android', 'Android'),
			    note: L('试用镜像不能设为默认启动', 'A trial image is never the default boot') }
			: { id: 'default_boot', type: 'choice', label: L('默认启动', 'Default boot'), value: default_boot(),
			    options: [ { value: 'linux', label: L('Linux', 'Linux') }, { value: 'android', label: L('Android', 'Android') } ],
			    confirm: true, reload: true,
			    note: L('Linux：每次开机后都回到 Linux；Android：下次重启起进入 Android',
			            'Linux: every reboot comes back to Linux; Android: reboots go to Android') },
			{ id: 'reboot', type: 'action', label: L('重启', 'Reboot'), confirm: true,
			  note: next ? L(`重启后进入 ${next.label.zh}`, `Boots ${next.label.en}`) : null },
			{ id: 'poweroff', type: 'action', label: L('关机', 'Power off'), confirm: true,
			  note: L('插着 USB 时可能会进入充电模式', 'With USB plugged in it may start in charging mode') },
			{ id: 'android_once', type: 'action', label: L('下次启动 Android', 'Boot Android once'), confirm: true,
			  note: L('重启进 Android 一次', 'One boot of Android') }
		], (i) => i != null);
	},
	set: function(id, value) {
		if (id == 'timezone') {
			let z = null;
			for (let e in ZONES) if (e[0] == value) z = e;
			let sec = system_section();
			if (!z || !sec) return 'unknown time zone';
			let c = ctx.uci();
			c.set('system', sec, 'zonename', z[0]);
			c.set('system', sec, 'timezone', z[1]);
			c.commit('system');
			// /etc/TZ for the system; the screen's WebKit reads TZ at its start
			ctx.run('/etc/init.d/system reload >/dev/null 2>&1');
			ctx.run('(sleep 2; /etc/init.d/e5-infoscreen restart) >/dev/null 2>&1 &');
			return null;
		}
		if (id == 'clock_seconds') {
			let c = ctx.uci();
			c.set('e5-infoscreen', 'main', 'clock_seconds', value ? '1' : '0');
			c.commit('e5-infoscreen');
			return null;
		}
		if (id == 'traffic_clear') {
			// a new, empty database: vnstat adds the configured interfaces again
			ctx.run('/etc/init.d/vnstat stop >/dev/null 2>&1; rm -f /etc/vnstat/vnstat.db; /etc/init.d/vnstat start >/dev/null 2>&1');
			return null;
		}
		if (id == 'default_boot') {
			if (value != 'linux' && value != 'android') return 'linux or android';
			if (trial_image()) return 'a trial image is never the default boot';
			return ctx.run(`e5-next-boot ${value} >/dev/null 2>&1`) == 0 ? null : 'e5-next-boot failed';
		}
		if (id == 'update_check') {
			return UPDATES.start('check');
		}
		if (id == 'update_apply' || id == 'update_rollback') {
			return UPDATES.start(id == 'update_apply' ? 'apply' : 'rollback');
		}
		if (id == 'reboot') { ctx.run('(sleep 2; reboot) >/dev/null 2>&1 &'); return null; }
		if (id == 'poweroff') { ctx.run('(sleep 2; poweroff) >/dev/null 2>&1 &'); return null; }
		// (booting Debian is e5-os on the command line: e5-os debian --once)
		if (id == 'android_once') { ctx.run('(e5-next-boot android && sleep 2 && reboot) >/dev/null 2>&1 &'); return null; }
		return 'no such setting';
	}
};

const usb_cat = {
	id: 'usb', label: L('USB', 'USB'),
	items: function() {
		let u = ctx.usb_status(), r = u.reset;
		let diagnosis = ctx.network_check_status();
		let labels = {
			no_controller:L('USB 控制器缺失','USB controller missing'),
			usb_not_enumerated:L('电脑尚未识别 USB','USB not enumerated by computer'),
			gadget_unbound:L('USB 设备尚未绑定','USB gadget not bound'),
			usb_interface_missing:L('USB 网卡缺失','USB network interface missing'),
			lan_missing:L('LAN 网桥缺失','LAN bridge missing'),
			usb_not_bridged:L('USB 未加入 LAN','USB not attached to LAN'),
			lan_no_ipv4:L('LAN 没有 IPv4 地址','LAN has no IPv4 address'),
			dhcp_not_running:L('DHCP 服务未运行','DHCP server not running'),
			wifi_not_running:L('热点尚未运行','Hotspot not running'),
			wifi_firmware_missing:L('Wi-Fi 固件缺失','Wi-Fi firmware missing'),
			regulatory_missing:L('无线管制数据库缺失','Regulatory database missing')
		};
		let checks = map(diagnosis?.checks ?? [], code => labels[code] ?? L(code,code));
		let hardware_errors = filter(split(diagnosis?.details?.kernel_log ?? '', '\n'), line=>match(lc(line), /(failed|error|not found|timeout|wrong nth|power on wcn)/));
		return [
			{ id: 'check', type: 'action', label:L('一键检查连接','Check connections'), reload:true,
			  note:L('检查 USB、热点、网桥和 DHCP 并保存报告，不改变网络配置','Check USB, Wi-Fi, LAN and DHCP; save a report without changing network settings') },
			{ id: 'diagnosis', type:'info', label:L('检查结论','Check result'),
			  value:diagnosis ? length(checks) ? L(join('；',map(checks,l=>l.zh)),join('; ',map(checks,l=>l.en))) : L('未发现异常','No issue detected') : '--',
			  note:diagnosis?.report ?? null },
			{ id: 'wireless_error', type:'info', label:L('热点原始报错','Wireless error'),
			  value:join('; ',map(type(diagnosis?.details?.native_errors)=='array' ? diagnosis.details.native_errors : [], e=>type(e)=='object' ? e.code ?? e.message ?? sprintf('%J',e) : `${e}`)) || '--',
			  note:L('无法联网时可拍下本页的检查结论和报错','If networking is unavailable, photograph the check result and error on this page') },
			{ id:'hardware_error', type:'info', label:L('硬件原始报错','Hardware error'), value:length(hardware_errors) ? hardware_errors[length(hardware_errors)-1] : '--' },
			{ id: 'controller', type: 'info', label: L('控制器状态', 'Controller state'), value: u.state ?? '--' },
			{ id: 'host_address', type: 'info', label: L('电脑地址', 'Computer address'), value: u.ip ?? L('尚未获取地址', 'No address yet') },
			{ id: 'reset_status', type: 'info', label: L('USB 重置结果', 'USB reset status'),
			  busy: r.busy,
			  value: r.busy ? L('正在修复…', 'Recovering…') : r.state == 'done' ? L('已重新枚举', 'Re-enumerated') : r.state == 'failed' ? L('修复失败', 'Recovery failed') : '--',
			  note: r.message ? `${r.stage ?? ''}: ${r.message}` : null },
			{ id: 'reset', type: 'action', label: L('重置 USB 连接', 'Reset USB connection'), confirm: true, reload: true,
			  note: L('电脑的 USB 网络会短暂断开；重新识别设备并恢复 LAN/DHCP，保留网络设置', 'USB networking disconnects briefly; re-enumerate and restore LAN/DHCP while keeping network settings') }
		];
	},
	set: function(id) { return id == 'reset' ? ctx.usb_reset_start() : id == 'check' ? ctx.network_check() : 'no such setting'; }
};

/* ---------- about ---------- */

const about = {
	id: 'about', label: L('关于', 'About'),
	items: function() {
		return [
			{ id: 'name', type: 'info', label: L('信息屏', 'Info screen'), value: 'e5-infoscreen' },
			{ id: 'maintainer', type: 'info', label: L('维护者', 'Maintainer'), value: 'Enceka <enceka@yeah.net>' },
			{ id: 'copyright', type: 'info', label: L('版权', 'Copyright'), value: '© 2026 Enceka' },
			{ id: 'license', type: 'info', label: L('许可', 'License'), value: 'MIT' },
			{ id: 'donate', type: 'image', label: L('赞赏', 'Donate'), src: '/donate.jpg',
			  caption: L('觉得好用，可以微信扫码赞赏作者', 'If it is of use to you: the author\'s WeChat tip code') },
			{ id: 'disclaimer', type: 'info', label: L('免责声明', 'Disclaimer'),
			  value: L('按“原样”提供，不作任何担保', 'Provided "as is", without warranty'),
			  note: L('非官方软件，与荣悦及设备、芯片厂商无关。修改网络模式、频段、AT 指令和充电设置可能导致断网、设备异常或失去保修，风险由使用者自行承担。',
			          'Unofficial software, not affiliated with Rongyue or the device and chip makers.  Changing the network mode, bands, AT commands or charging can cut the connection, upset the device or void its warranty; at your own risk.') }
		];
	},
	set: function(id, value) {
		return 'read-only';
	}
};

// (the devices category is its own view: /api/devices)
// (the AT page is its own view as well: /api/at, /api/at/presets)
return [ network, { id: 'at', label: L('AT 指令', 'AT commands'), view: 'at' },
         { id: 'devices', label: L('设备管理', 'Devices'), view: 'devices' },
         { id: 'appmgr', label: L('应用管理', 'Apps'), view: 'apps' },
         ...(ctx.run('command -v bluetoothctl >/dev/null 2>&1') == 0
             ? [ { id: 'bluetooth', label: L('蓝牙', 'Bluetooth'), view: 'bluetooth' } ] : []),
         charge, notify, ...(has_sound ? [ sound ] : []), screen, usb_cat, system_cat, about ];

};
