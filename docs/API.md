# e5-infoscreen API, version 1

> 中文：[`API.zh-CN.md`](API.zh-CN.md)

The info screen is a web page (`www/`) and a local HTTP API (`api.uc`), served by
their own uhttpd on **`http://127.0.0.1:8088`**.  Plugins extend both: a page of
their own in the Apps list, optionally a backend under `/api/plugins/<id>/`, and
optionally settings in the 高级 (settings) menu.  This document is the contract
for version 1 of all three.

## 1. Conventions

* **Local only.**  The server listens on 127.0.0.1; nothing outside the device
  reaches it, which is why there is no login.  Do not expose it (no port
  forward, no proxy from LuCI).
* **Trust.**  A plugin backend runs inside the API server, as root, with the
  helpers of section 5.3.  Installing a plugin means trusting it with the
  device.  Only install plugins you have read.
* **JSON** in and out (`Content-Type: application/json`).  A POST body is one
  JSON object, at most 4 KiB.  An error is a non-2xx status with
  `{ "error": "<text>" }`.
* **Text for people** is `{ "zh": "...", "en": "..." }` wherever it is shown on
  the screen (labels, notes, names); the page picks the screen's language and
  falls back to `zh`.  Plain strings are shown as they are.
* **Identifiers** (IMEI, ICCID, IMSI, own number) come from `/api/identity`
  alone.  A plugin must not show or send them without the user asking.
* **Versions.**  `api_version` is 1.  Within version 1, fields and endpoints are
  only added, never renamed or removed; a plugin ignores fields it does not
  know.  A plugin whose manifest asks for a higher `api_version` than the
  screen's is not loaded.

## 2. Core API

`GET /api/<name>` unless marked POST.  Values the device does not have are
`null`.

| Endpoint | Returns / does |
|---|---|
| `GET /status` | everything the pages show, polled every 2 s (below) |
| `GET /update` | `{ current, latest, available, busy, checking, checked_at, deferred, deferred_until, error, action, result, backup }`: info screen update state |
| `GET /update?check=1` | Same state; starts a background check when due (6 hours, retry after 10 minutes on failure) |
| `POST /update` | `{ action: "check"\|"apply"\|"rollback"\|"defer", version }` -> update state plus `{ ok, error }`; `apply` and `defer` validate the offered version |
| `GET /traffic` | `{ wan, lan }`, each `{ available, since, today, month, total, days[] }` with `{ rx, tx }` in bytes (vnstat; `wan` the modem's interface, `lan` the LAN bridge); `days` the last 7, `{ date: "MM-DD", rx, tx }`; `since` when counting began.  The `wan` fields are also at the top level. |
| `GET /sms` | `{ messages: [ { id, number, text, time, state, type, unread } ] }`, newest first; `time` ISO 8601 |
| `POST /sms-read` | all messages seen (clears the unread list) |
| `POST /sms-delete` | `{ id }` -- deletes the message from the SIM/modem |
| `POST /sms-send` | `{ number, text, card }` -- sends a message (e5-linux's `/usr/libexec/e5-sms`, through ModemManager); `{ ok: true, id, sim }`, or 400 with `{ ok: false, error }`.  `card` (optional) 0 or 1: from that SIM card -- the other one than the card in use is switched to first (`e5-sim`; the data connection moves with it, and the send waits for its registration, up to 2 minutes); without it, the card in use.  The text up to 4000 bytes (about 1300 Chinese characters), split into parts by the modem. |
| `GET /qr` | the hotspot's join code, `image/svg+xml` |
| `GET /wifi-key` | `{ key }` -- the hotspot passphrase |
| `POST /wifi` | `{ on: true\|false }` -- hotspot on/off |
| `POST /backlight` | `{ level: 0-255, save: bool }` -- `save` makes it the level the screen comes back to |
| `POST /wan-reconnect` | restarts the mobile connection |
| `POST /donate-seen` | the 赞赏码 was shown: `donate_seen` in `/etc/config/e5-infoscreen`, so it is not shown at start again |
| `POST /vibrate` | `{ ms }`: one pulse of the motor, 20-1000 ms (e5-linux's `e5-vibrate`) |
| `GET /advanced` | device, baseband, locks, SIM details (the 高级信息 page) |
| `GET /bluetooth` | `{ available, adapter, autostart, powered, discovering, name, devices: [ { mac, name, paired, connected, state, icon } ] }`: bluetoothd's view; unnamed devices (BLE beacons) left out; `state` is e5-linux's `e5-bt-connect` progress: `pairing`, `connecting`, `failed: <reason>` (`notfound`, `forgot`, `noanswer`, or bluetoothd's) |
| `POST /bluetooth` | `{ action: "power", on }`, `{ action: "autostart", on }` (on at boot: e5-linux's `e5-bluetooth.main.autostart`, bluetoothd's AutoEnable), `{ action: "scan" }` (20 s), `{ action: "connect"\|"disconnect"\|"remove", mac }` -> `{ ok, error }` + the same as `GET`; `connect` runs in the background (poll `GET`) |
| `GET /volume` | `{ available, level, max, card, output }`: the volume (`output`: `speaker` or `bluetooth`, where the sound goes); the speaker volume (e5-linux's `e5-volume`), 0 mute - `max` (15); `available: false` without it |
| `POST /volume` | `{ step: 1\|-1 }` or `{ level }` -> the same as `GET` |
| `GET /identity` | `{ imei, iccid, imsi, numbers[] }` |
| `POST /at` | `{ cmd, timeout }` -> `{ ok, reply }` or `{ ok: false, error }`, and `warning: true` for a command that can leave the CP's AT server (`ATZ`, `AT&F`, `+CPMS=`) or the SIM (`+CFUN=0`, `+SFUN=3`/`5`) gone until a reboot (e5-linux's `e5-at` warns the same way; nothing is refused): one AT command through ModemManager (the AT channel's one owner), `timeout` 1-60 s (default 10).  A raw console: its replies can hold identifiers. |
| `GET /at/presets` | `{ presets: [ { cmd, label } ] }`, the reads of the 高级 -> AT 指令 page |
| `GET /settings` | `{ categories: [ { id, label, view, plugin } ] }` |
| `GET /settings/<category>` | `{ id, label, items[] }` (section 3) |
| `POST /settings/<category>` | `{ id, value }` (or `{ id }` for an action) -> `{ ok, error, item }`, the item read back |
| `GET /devices` | `{ devices: [ { mac, ip, name, via, online, signal, blocked } ] }` |
| `POST /devices` | `{ mac, action: "block"\|"unblock"\|"kick" }` -> `{ ok, error, devices }` |
| `GET /plugins` | `{ api_version, plugins: [ manifest + { has_backend } ] }` |
| `GET /store` | the app store (section 6): `{ available, error, url, fetched, plugins: [ index entry + { installed, builtin } ] }`; `installed` the version on the device (`null`: not installed); `?refresh=1` fetches the index again |
| `POST /store-install` | `{ id }` -> `{ ok, error }`: the app from the store, installed or updated |
| `* /plugins/<id>/<path>` | the plugin's backend (section 5.3) |

`GET /status`:

```json
{
  "time": 1790479780, "tz_offset": 28800, "clock": "10:46",
  "modem": { "present": true, "state": "connected", "operator": "CHINA BROADNET",
             "registration": "home", "tech": "5gnr", "quality": 67, "sim": true, "sim_card": 0,
             "signal": { "rsrp": -103.0, "rsrq": -22.5, "snr": -10.5 },
             "cell": { "type": "5gnr", "serving": true, "pci": 169, "arfcn": 504990,
                       "band": "n41", "rsrp": -97.3, "rsrq": -12.3, "sinr": 4.6,
                       "bandwidth_mhz": 100 },
             "neighbours": 5,
             "qos": { "qci": 9, "dl_kbps": 1000000, "ul_kbps": 100000, "nr": true } },
  "wan": { "up": true, "uptime": 1519, "ipv4": "10.1.2.3", "ipv6": "240a:...",
           "ipv6_prefix": "240a:.../64", "dns": [ "..." ] },
  "traffic": { "rx_total": 101641661, "tx_total": 47712286, "rx_rate": 1204.5, "tx_rate": 88.0 },
  "wifi": { "ssid": "E5-Linux", "enabled": true, "up": true, "channel": "149", "band": "5g", "secured": true,
            "hidden": false, "qr_revision": 1790479700 },
  "clients": [ { "name": "phone", "ip": "192.168.9.12", "mac": "..", "via": "wifi", "signal": -52 } ],
  "battery": { "capacity": 99, "status": "Charging", "current_ma": 194, "voltage_mv": 4350, "limit": 80, "paused": false, "online": true },
  "usb": { "cable": true, "port": "CDP", "state": "configured", "speed": "high-speed", "ip": "192.168.9.2",
           "rx_rate": 812.0, "tx_rate": 20480.5, "link": "online" },
  "system": { "uptime": 5321, "load": 0.42, "mem_total": 1538670592, "mem_available": 794218496,
              "disk_total": 1020702720, "disk_used": 345812992, "lan_ip": "192.168.9.1",
              "temperatures": { "soc": 37.1, "cpu": 37.1, "gpu": 36.0,
                                "modem": 36.5, "lte": 36.5, "nr": 36.5, "mm": 36.0,
                                "board": 36.7, "pa": 36.6, "battery": 31.7 } },
  "screen": { "idle": 60, "brightness": 120, "lang": "zh" },
  "sms": { "unread": [ 3 ], "screen": true }
}
```

Rates are bytes per second over the time since the previous poll (`null` on
the first).  `battery.limit` is the charge limit when one is on (e5-linux's
`e5-charge`), `paused` whether it has stopped charging -- the gauge's own
`status` then reads `Full` or `Not charging`.  `system.disk_*` is the root
filesystem (`df /`, cached 60 s).  The modem part is cached for 10 s, except
`modem.sim_card`: the SIM card the modem is for, 0 or 1 (e5-linux's `e5-sim`), read
each time; the data interface follows it (`sipa_eth0`, `sipa_eth8`).  `modem.qos` is the
subscribed rate: the aggregate maximum bit rate the network grants the data
context (cid 1), in kbit/s, with its QCI (LTE, `AT+CGEQOSRDP=1`) or 5QI (`nr`,
`AT+C5GQOSRDP=1`); cached 60 s, `null` without a bearer.

`system.temperatures` is in degrees Celsius, cached for 5 seconds. Missing
readings are `null`. `cpu` is the maximum of the CPU core/cluster zones;
`modem` is the maximum of the LTE and NR zones inside the SoC, rather than a
temperature queried from the CP. `soc`, `gpu`, `board` and `pa` use their named
thermal zones. `lte` is the LTE zone, `nr` is the maximum of the two NR zones,
and `mm` is the multimedia zone. `battery` comes from `power_supply/battery/temp` (tenths of a
degree, converted to Celsius), with the battery thermal zone as a fallback.
There are no separate RAM, eMMC or SD-card temperature sensors exposed here.

`GET /advanced` adds `device.temperatures` with the same summaries and
`device.thermal_zones`, an array of
`{ zone, temp, estimated, unverified }`. `temp` is Celsius or `null`. The
front/back shell temperatures are estimates from the board/PA/charger zones;
the charger reading is around 85 °C at room temperature and remains
unverified, so it and those dependent estimates are marked `unverified`.
These values are available for diagnostics but excluded from the overview's
summaries. These flags do not modify the kernel's thermal or charging policy.
The overview puts battery temperature in its battery row and shows the other
eight summaries together. Details shows nine summaries; a Show more button
expands the remaining individual zones.

`wifi.qr_revision` is the wireless configuration modification time. Combined
with the SSID, security and hidden flags, it invalidates a previously drawn
QR code after a passphrase change. `GET /qr` encodes the configured security
type, SSID, passphrase and hidden flag, with special characters escaped.
Protected networks without a passphrase do not get an open-network QR code.

`usb` is the USB link as it is: `cable` a cable in (an extcon's `USB=1`), `port` what it
comes from as the charger sees it (`SDP`/`CDP` a computer's port, `DCP` a wall charger),
`state` the gadget's UDC state (`configured`: enumerated), `ip` the host's lease, the
rates the host's traffic on `usb0` (the E5's view: `tx` is what the host downloads).
`link`: `none` (no cable), `charger` (charging only), `host` (a computer, the gadget
not enumerated -- e5-linux's `e5-usb-watch` connects it again after a replug),
`enumerated`, `lease` (the host has an address), `online` (its traffic over 2 KB/s).

`usb.reset` reports recovery availability, `busy`, `state`, `stage`, `message`
and `updated`. `POST /settings/usb` with `{ "id": "reset" }` queues recovery;
`{ "id": "check" }` runs read-only connection diagnostics and saves the report.
`GET /settings/usb` shows its conclusions and recovery result. LuCI exposes
authenticated `e5-infoscreen.usb_reset` and `e5-infoscreen.network_check` RPCs.
`wifi.state` is `off`, `on`, `starting` or `failed`; pending startup is bounded
to 30 seconds and `wifi.error` carries the native error or timeout reason.

## 3. Settings items

A category's `items` are drawn by type; a plugin's settings use the same shape.

| Field | |
|---|---|
| `id` | unique in the category |
| `type` | `toggle` (value `true`/`false`), `choice` (value + `options`), `number` (value + `min`, `max`, `step`, `unit`), `multi` (value = array of option values; none chosen means "no restriction"), `action` (no value; pressing it runs it), `info` (read-only text), `image` (`src`, optional `caption`: pressing the row shows the picture full screen) |
| `label`, `note` | `{ zh, en }`; `note` is shown under the item |
| `options` | `[ { value, label } ]` |
| `confirm` | `true`: the change needs a second press within 3 s (used for what can cut the connection) |
| `value` | the current value, read back after a change |
| `reload` | `true`: after a change the page reads the whole category again (the change shows in other items too) |

## 4. Keys

The page and the SDK report keys as kinds, measured on the E5's keypad:

| Kind | Key | Notes |
|---|---|---|
| `left` `right` `up` `down` | the navigation keys | on the host: pages, focus |
| `ok` | confirm (`KEY_SELECT`, reported by WebKit as "Unidentified"/0) | |
| `back` | back (`KEY_BACK` + BackSpace, one press) | the SDK folds the pair into one |
| `digit` | `0`-`9`, `*` | `key` has the character |
| `power` | power (`PowerOff`) | always the host's: screen off; power then `*` within 2 s is the key lock |
| `other` | side key (`F1`), volume, ... | volume up/down (`AudioVolumeUp`/`AudioVolumeDown`) are always the host's: the speaker volume |

## 5. Plugins

### 5.1 Layout

```
/usr/share/e5-infoscreen/www/plugins/<id>/
    manifest.json      required
    index.html         the page (the manifest's "entry")
    backend.uc         optional: /api/plugins/<id>/...
    ...                anything else the page loads (relative URLs)
```

`<id>`: lower-case letters, digits, `-` and `_`, starting with a letter or digit;
it is the directory name and the manifest's `id`.  Install by copying the
directory; remove by deleting it.  No restart: the Apps page reads the list
each time it is opened, the settings menu each time it is loaded.

### 5.2 `manifest.json`

```json
{
  "id": "nettest",
  "api_version": 1,
  "version": "1.0",
  "name": { "zh": "网络测试", "en": "Network test" },
  "description": { "zh": "延迟和丢包", "en": "Latency and loss" },
  "entry": "index.html",
  "order": 30,
  "settings": [
    { "id": "count", "type": "number", "uci": "e5-plugin-nettest.settings.count",
      "default": "4", "min": 1, "max": 10, "step": 1,
      "label": { "zh": "每个目标的次数", "en": "Pings per target" } }
  ]
}
```

| Field | |
|---|---|
| `id`, `api_version`, `name` | required |
| `version`, `description` | shown in the Apps list |
| `entry` | the page, relative to the directory; default `index.html` |
| `order` | position in the Apps list, low first; default 50 |
| `input_method` | `true`: the app may send committed text and backspace actions to the host's last focused `input`, `textarea` or `contenteditable` target through the SDK. Use this only for an input method; the host ignores these messages from other apps. |
| `settings` | items (section 3) of type `toggle`, `choice` or `number`, each stored in the uci option `uci` (`config.section.option`), with `default` when it is unset.  They appear as a category of their own in 高级.  Use a config named `e5-plugin-<id>`; it is created on the first change. |

### 5.3 Frontend

The page runs in a frame over the pages (320×480, the status bar above it, the
footer below: about 320×424 CSS pixels).  Load the SDK first:

```html
<script src="/sdk/e5.js"></script>
```

| SDK | |
|---|---|
| `e5.id`, `e5.lang`, `e5.version` | the plugin's id, `zh`/`en`, the API version |
| `e5.api(path, { body, method })` | `fetch` of `/api/plugins/<id><path>`; with `body` it is a POST of JSON; resolves to the reply (JSON or text), rejects with `.status` and `.data` on a non-2xx |
| `e5.core(path, opts)` | the same for the core API, `/api/<path>` (section 2) |
| `e5.sms.list()`, `e5.sms.send(number, text, card)` | the messages (`GET /sms`); sending one (`POST /sms-send`, `card` optional), rejects with the error |
| `e5.onKey(fn)` | `fn({ kind, key, code, repeat })` for each key; return `true` when taken |
| `e5.onBack(fn)` | `fn()` on back when `onKey` did not take it; return `true` to stay, anything else closes the plugin |
| `e5.onLang(fn)` | the screen's language changed, `fn(lang)` |
| `e5.input(text)` | send committed text to the host's last focused editable target; returns `true` when the SDK currently knows a target is available |
| `e5.inputBackspace()` | delete one character or the current selection in that target; returns `true` when a target is available |
| `e5.inputAvailable` | `true` when the host currently has an editable target |
| `e5.onInputTarget(fn)` | `fn(available)` when the host target becomes available or unavailable |
| `e5.onInputResult(fn)` | `fn({ ok, error })` after the host handles an input operation |
| `e5.toast(text)` | a short message over the screen |
| `e5.keepAwake(on)` | `true`: the screen does not go dark while the plugin is open (a timer, a test running); set it back to `false` |
| `e5.exit()` | close the plugin |
| `e5.press(el)` | click `el` from a key.  With touch off (高级 -> 屏幕 -> 触摸) the SDK drops every touch and every click -- WebKit's own click after a tap is untrusted, so it cannot be told apart -- except these. |
| `e5.t({ zh, en })` | the text in the screen's language |
| `e5.tzOffset`, `e5.time(ms)` | the device's offset from UTC (s); a time as `HH:MM:SS` in the device's zone.  WebKit has no zoneinfo on OpenWrt, so `Date`'s local time is UTC: use these (or `tz_offset` from `/status`). |

Key kinds are `left`, `right`, `up`, `down`, `ok`, `back`, `power`, `menu`,
`call`, `digit` and `other`. The E5 session maps the physical call key to `F13`
and `kind: "call"`, and # to a literal `#` with `kind: "digit"`. Unknown keys
remain `other`; they never act as confirmation. Menu is handled by the host
and returns to the application list without invoking the plugin key handler.

While the screen is dark the SDK swallows keys (the host wakes the screen); the
power key is the host's unless `e5.capturePower(true)` is enabled by the active
application. Touch works as in any page. Use large text and a dark
background (the screen's own look: `#0b0e13`, cards `#161b23`, text `#e8ecf2`);
the fonts are Noto Sans CJK SC and DejaVu Sans.

When an installed application focuses an editable control, the host opens the
first installed manifest with `input_method: true` in a second overlay frame.
The original application stays underneath it. `e5.input()` inserts at that
target's selection and dispatches a
standard `input` event, so the page's own validation and bindings continue to
work. The target is an `input`, `textarea` or `contenteditable` element that is
not disabled or read-only. `e5.inputAvailable` and `e5.onInputTarget()` expose
whether one is currently available. The host accepts these operations only
from a manifest with `input_method: true`, and limits one insertion to 4096
characters.

Without the SDK, the protocol is `postMessage` with the parent frame:
plugin -> host `{ e5: "ready" }`, `{ e5: "key", kind, key, code, keyCode, repeat }`
(every key, so the host can wake the screen and reset its idle timer),
`{ e5: "exit" }`, `{ e5: "toast", text }`, `{ e5: "keep-awake", on }`,
`{ e5: "input", action: "insert", text }` or `{ e5: "input", action: "backspace" }`;
host -> plugin `{ e5: "hello", lang, api_version, blank, tz_offset, touch, input_available }`,
`{ e5: "input-target", available }`, `{ e5: "input-result", ok, error }`,
`{ e5: "blank", on }`, `{ e5: "touch", on }`.

### 5.4 Backend

`backend.uc` is ucode (raw mode, no `{% %}`) that returns a function taking the
context and returning the routes:

```js
'use strict';
return function(ctx) {
	return {
		'GET /hello': function(req) {
			return { text: 'hello', uptime: ctx.read_trim('/proc/uptime') };
		},
		'POST /echo': function(req) {
			return { got: req.body };
		}
	};
};
```

* A route is `"<METHOD> <path>"`, the path after `/api/plugins/<id>`; `/` for
  none.
* `req` is `{ method, path, query, body }`: `query` the parsed query string,
  `body` the parsed JSON of a POST.
* A handler returns an object (sent as JSON), or `{ status, type, body }` for
  anything else (`body` a string).
* It runs in the API server for each request -- nothing survives in a
  variable between requests (keep state with `ctx.state_put`), and a request
  has 30 s.  Start long work in the background (`ctx.run('... &')`) and let
  the page poll a route that reads its state.

`ctx`:

| | |
|---|---|
| `ctx.api_version` | 1 |
| `ctx.sh(cmd)` | runs a shell command, its output (string) or `null`.  Never build `cmd` from `req` without quoting: `req` is data. |
| `ctx.sh_json(cmd)` | the same, parsed as JSON (`null` if it is not) |
| `ctx.run(cmd)` | runs a command, its exit status |
| `ctx.at(cmd, timeout)` | an AT command through ModemManager (the AT channel has one owner), the reply without `OK`, or `null`; nothing is refused |
| `ctx.at_console(cmd, timeout)` | the same as `POST /at`: `{ ok, reply }` or `{ ok: false, error }`, with `warning: true` for a command that can leave the modem's AT server or SIM dead until a reboot |
| `ctx.modem()` | the `modem` part of `/status` |
| `ctx.sim_card()` | the SIM card the modem is for, 0 or 1 |
| `ctx.cells()` | the serving and neighbour cells, `[ { type, serving, pci, arfcn, band, rsrp, rsrq, sinr, bandwidth_mhz } ]` |
| `ctx.ubus(object, method, args)` | a ubus call, its reply or `null` |
| `ctx.uci()` | a uci cursor (`get`, `set`, `commit`, `foreach`, ...) |
| `ctx.state_get(name, max_age_s)`, `ctx.state_put(name, value)` | JSON state that outlives the request, per plugin (under `/tmp/run`, so not across a reboot); `state_get` returns `null` when older than `max_age_s` |
| `ctx.forget(name)` | removes a state |
| `ctx.read_trim(path)`, `ctx.read_num(path)` | a file's content, trimmed / as a number |
| `ctx.log(text)` | a line in the system log (`logread -e e5-infoscreen/<id>`) |

### 5.5 Packages

An app is installed from a package: a `.tar.gz` (`.tgz`) or `.zip` with the
plugin directory's files at its top, or inside one top directory:

```sh
tar -czf nettest-1.0.tar.gz -C www/plugins nettest     # or: cd www/plugins && zip -r nettest-1.0.zip nettest
```

Install it in LuCI (`http://192.168.9.1`, 服务 -> 信息屏应用, "上传并安装"), or on
the device with `/usr/libexec/e5-infoscreen/plugin install FILE`.  The
manifest has to be valid (an `id` of lower-case letters, digits, `-` and `_`,
an `api_version` the screen has, a `name`); the archive may not hold absolute
paths, `..`, or links, nor more than 20 MB.  An app with the same id is
replaced (the upgrade).  Installed apps live in `/etc/e5-infoscreen/plugins`
and survive an update of the system image; the built-in ones are in the
image.  Uninstall in LuCI, on the screen (高级 -> 应用管理) or with
`plugin remove ID`.  `GET /plugins` marks each manifest `builtin` (true for the
image's own); `POST /plugins-remove { id }` uninstalls.

The two plugins in `www/plugins/` are the examples: `calculator` (a page, keys,
`onBack`) and `nettest` (a backend, settings, `keepAwake`).

## 6. The app store and the screen's own update

**The store** is [`Enceka/infoscreen-plugins`](https://github.com/Enceka/infoscreen-plugins): apps
in `plugins/<id>/`, checked by its CI (`tools/check.py`: the manifest, the files, the style, what
a frontend must not do; a backend is flagged for review) and published on GitHub Pages as
`index.json` plus one package per app.  On the device `plugin store` fetches the index
(`e5-infoscreen.main.store_url`, default `https://enceka.github.io/infoscreen-plugins/index.json`)
and `plugin get ID` downloads the package, checks its size and SHA-256 against the index and
that it holds the app the index said, then installs it as any package (5.5).  On the screen:
高级 -> 应用管理 -> 应用商店 (install, update, a second press confirms).

**The screen's own update** needs no new system image: `/usr/libexec/e5-infoscreen/update`
reads a release description (`e5-infoscreen.main.update_url`, default
`https://enceka.github.io/infoscreen/latest.json`: `{ version, url, sha256, size, notes }`), and
`update apply` downloads the package
(this repository's `root/`, made by `tools/make-release.sh`), checks its size, SHA-256 and every
path in it (under `usr/`, `www/` or `etc/`, never `etc/config` or `etc/e5-infoscreen`; no `..`,
no absolute path, no link), saves the files it replaces to
`/etc/e5-infoscreen/update-backup.tar.gz`, unpacks it and restarts the screen; `update rollback`
puts the saved files back.  The version is `/usr/share/e5-infoscreen/VERSION`.  On the screen:
高级 -> 系统 -> 检查更新, then 更新到 x.y.z. The overview also offers an update
notice with expandable release notes, Update now and Later. Later persists
`update_defer_version` and `update_defer_until` in `e5-infoscreen.main`: the same
release is suppressed for 24 hours, including after a screen restart; a newer
release is not suppressed. Checks and installs run through `update-job`, with
one job at a time. The overview never installs without a button press and
never wakes a blank or locked screen. A release that needs packages or a newer system than
the image has cannot be installed this way: that takes an image update.

To publish a version: raise `VERSION`, commit and push. CI makes the package and publishes the
Pages feed at `https://enceka.github.io/infoscreen/latest.json`; it also uploads the same package
and JSON to the GitHub release `v<version>` so installations using the old update URL keep working.

## Application notifications (API 2, core 1.5.0+)

`e5.notify({id, title, body, wake, ttl})` publishes a notification in the current
plugin namespace. `e5.clearNotification(id)` removes it. Backends have the same
operations as `ctx.notify(notice)` and `ctx.clear_notification(id)`. IDs use
letters, numbers, `_` or `-` (1–64 characters). `title` may be a `{zh,en}` label;
`body` is plain text, `wake` requests screen wake, and `ttl` is seconds (default
60; maximum 86400). Requests use `POST /api/plugins/<id>/_notify`; only installed
plugins can publish. `GET /api/notifications` lists current notifications.

For a plugin that must notify with its page closed, set `notifications: true`
in its manifest and expose `GET /notifications` in `backend.uc`, returning
`{notifications:[{id,title,body,wake}]}`. This route must only read state; the
core never invokes any plugin action automatically. The status poll includes
these results as `notifications`. Use stable IDs and return an empty list when
the condition ends. The core provides a status indicator, Open and Later
buttons, text-safe display, and wake control respecting key lock. Open loads
the installed plugin, and does not answer calls or perform other plugin actions.

API 2 also provides `e5.capturePower(on)` for an app that handles the existing
power/lock event during an operation; it does not alter hardware key mappings.
Core-owned notification overlays block input to the underlying plugin until
dismissed. Input-method apps using `e5.input()` must declare `api_version: 2`;
API 1 apps continue to work.
