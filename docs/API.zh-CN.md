# e5-infoscreen API，第 1 版

> English: [`API.md`](API.md)

信息屏由网页（`www/`）和本地 HTTP 接口（`api.uc`）组成，由独立的 uhttpd 提供服务，
地址是 **`http://127.0.0.1:8088`**。插件可以扩展这两部分：在“应用”列表里放自己的页面，
也可以在 `/api/plugins/<id>/` 下提供后端，还可以在“高级”菜单里加入自己的设置项。本文是
这三者第 1 版的约定。

## 1. 约定

* **仅限本机。** 服务只监听 127.0.0.1，设备外部访问不到，所以没有登录。不要把它暴露
  出去（不做端口转发，也不从 LuCI 代理）。
* **信任。** 插件后端运行在接口服务里，身份是 root，可以使用 5.4 节的辅助函数。安装
  插件就等于把设备交给它，只安装你看过代码的插件。
* 输入输出都是 **JSON**（`Content-Type: application/json`）。POST 的请求体是一个 JSON
  对象，最大 4 KiB。出错时返回非 2xx 状态码和 `{ "error": "<说明>" }`。
* **显示给人看的文字**（标签、说明、名称）写成 `{ "zh": "...", "en": "..." }`，页面按屏幕
  语言选择，缺省用 `zh`。普通字符串原样显示。
* **识别码**（IMEI、ICCID、IMSI、本机号码）只由 `/api/identity` 提供。插件不得在用户没有
  要求时显示或发送它们。
* **版本。** `api_version` 为 1。在第 1 版之内，字段和接口只增不改、不删；插件应忽略自己
  不认识的字段。manifest 要求的 `api_version` 高于信息屏的插件不会被加载。

## 2. 核心接口

未注明 POST 的都是 `GET /api/<名称>`。设备上没有的值为 `null`。

| 接口 | 返回 / 作用 |
|---|---|
| `GET /status` | 各页面显示的全部数据，页面每 2 秒读一次（见下） |
| `GET /update` | `{ current, latest, available, busy, checking, checked_at, deferred, deferred_until, error, action, result, backup }`：信息屏更新状态 |
| `GET /update?check=1` | 同上；到期时启动后台检查，正常间隔 6 小时，失败后 10 分钟重试 |
| `POST /update` | `{ action: "check"\|"apply"\|"rollback"\|"defer", version }`，返回更新状态及 `{ ok, error }`；立即更新和推迟须匹配提示的版本 |
| `GET /traffic` | `{ wan, lan }`，各为 `{ available, since, today, month, total, days[] }`，其中 `{ rx, tx }` 单位字节（vnstat；`wan` 为模组数据接口，`lan` 为局域网网桥）；`days` 是最近 7 天，`{ date: "MM-DD", rx, tx }`；`since` 为开始统计的时间。`wan` 的字段在顶层也有一份。 |
| `GET /sms` | `{ messages: [ { id, number, text, time, state, type, unread } ] }`，新的在前；`time` 为 ISO 8601 |
| `POST /sms-read` | 全部标为已读（清空未读列表） |
| `POST /sms-delete` | `{ id }`，从 SIM 卡/模组删除该短信 |
| `POST /sms-send` | `{ number, text, card }`，发一条短信（e5-linux 的 `/usr/libexec/e5-sms`，经 ModemManager）；成功 `{ ok: true, id, sim }`，失败 400 `{ ok: false, error }`。`card`（可选）0 或 1：用这张 SIM 卡发；不是当前在用的卡时先切换过去（`e5-sim`，数据连接随之切换，发送前等它注册上网络，最多 2 分钟）；不给则用当前的卡。内容最多 4000 字节（约 1300 个汉字），由模组分条发送。 |
| `GET /qr` | 热点的连接二维码，`image/svg+xml` |
| `GET /wifi-key` | `{ key }`，热点密码 |
| `POST /wifi` | `{ on: true\|false }`，开关热点 |
| `POST /backlight` | `{ level: 0-255, save: bool }`；`save` 表示把它作为亮屏时恢复的亮度 |
| `POST /wan-reconnect` | 重新建立移动网络连接 |
| `POST /donate-seen` | 赞赏码已经显示过：写入 `/etc/config/e5-infoscreen` 的 `donate_seen`，以后开机不再弹出 |
| `POST /vibrate` | `{ ms }`：马达振动一次，20-1000 毫秒（e5-linux 的 `e5-vibrate`） |
| `GET /advanced` | 设备、基带、锁定、SIM 的详细信息（“高级信息”页） |
| `GET /bluetooth` | `{ available, adapter, autostart, powered, discovering, name, devices: [ { mac, name, paired, connected, state, icon } ] }`：bluetoothd 的状态；没有名字的设备（BLE 信标）不列出；`state` 是 e5-linux 的 `e5-bt-connect` 进度：`pairing`、`connecting`、`failed: <原因>`（`notfound`、`forgot`、`noanswer` 或 bluetoothd 的错误） |
| `POST /bluetooth` | `{ action: "power", on }`、`{ action: "autostart", on }`（开机启动：e5-linux 的 `e5-bluetooth.main.autostart`，即 bluetoothd 的 AutoEnable）、`{ action: "scan" }`（20 秒）、`{ action: "connect"\|"disconnect"\|"remove", mac }`，返回 `{ ok, error }` 加上 `GET` 的内容；`connect` 在后台进行（轮询 `GET`） |
| `GET /volume` | `{ available, level, max, card, output }`：音量（`output` 为 `speaker` 或 `bluetooth`，即声音从哪里出）；扬声器音量（e5-linux 的 `e5-volume`），0 为静音，最大 `max`（15）；没有该程序时 `available: false` |
| `POST /volume` | `{ step: 1\|-1 }` 或 `{ level }`，返回同 `GET` |
| `GET /identity` | `{ imei, iccid, imsi, numbers[] }` |
| `POST /at` | `{ cmd, timeout }`，返回 `{ ok, reply }` 或 `{ ok: false, error }`；对可能让 CP 的 AT 服务（`ATZ`、`AT&F`、`+CPMS=`）或 SIM 卡（`+CFUN=0`、`+SFUN=3`/`5`）直到重启都不可用的指令，额外返回 `warning: true`（e5-linux 的 `e5-at` 同样只是提醒，不再拒绝任何指令）：通过 ModemManager（AT 通道唯一的所有者）发一条 AT 指令，`timeout` 为 1-60 秒（默认 10）。这是原始的指令通道，回复里可能含有识别码。 |
| `GET /at/presets` | `{ presets: [ { cmd, label } ] }`，即“高级 → AT 指令”页的只读指令 |
| `GET /settings` | `{ categories: [ { id, label, view, plugin } ] }` |
| `GET /settings/<分类>` | `{ id, label, items[] }`（见第 3 节） |
| `POST /settings/<分类>` | `{ id, value }`（动作只传 `{ id }`），返回 `{ ok, error, item }`，item 为修改后读回的值 |
| `GET /devices` | `{ devices: [ { mac, ip, name, via, online, signal, blocked } ] }` |
| `POST /devices` | `{ mac, action: "block"\|"unblock"\|"kick" }`，返回 `{ ok, error, devices }` |
| `GET /plugins` | `{ api_version, plugins: [ manifest + { has_backend } ] }` |
| `GET /store` | 应用商店（第 6 节）：`{ available, error, url, fetched, plugins: [ 索引条目 + { installed, builtin } ] }`；`installed` 是设备上的版本（`null` 表示未安装）；`?refresh=1` 重新获取索引 |
| `POST /store-install` | `{ id }` → `{ ok, error }`：从商店安装或更新这个应用 |
| `* /plugins/<id>/<路径>` | 插件自己的后端（第 5.4 节） |

`GET /status` 的内容与英文版相同（见 [`API.md`](API.md) 第 2 节的示例）：`modem`、`wan`、
`traffic`（`rx_rate`/`tx_rate` 为距上次读取的每秒字节数，第一次为 `null`）、`wifi`、
`clients`、`battery`、`system`（含 `disk_total`/`disk_used`：根文件系统，`df /`，缓存 60 秒）、`screen`、`sms`。模组部分缓存 10 秒，`modem.sim_card` 除外：模组对应的 SIM 卡，0 或 1（e5-linux 的 `e5-sim`），每次现读；数据接口跟着它走（`sipa_eth0`、`sipa_eth8`）。`modem.qos` 是签约速率：网络给数据上下文（cid 1）的聚合最大比特率（AMBR），单位 kbit/s，附 QCI（LTE，`AT+CGEQOSRDP=1`）或 5QI（`nr`，`AT+C5GQOSRDP=1`）；缓存 60 秒，没有承载时为 `null`。

`system.temperatures` 为摄氏度，缓存 5 秒，缺失读数为 `null`：
`soc`（SoC 温控区）、`cpu`（CPU 核心/集群的最高值）、`gpu`、
`modem`（SoC 内 LTE/NR 温控区的最高值，并非向 CP 查询的温度）、
`lte`（4G）、`nr`（两个 5G 温控区的最高值）、`mm`（多媒体）、
`board`（主板）、`pa`（射频功放）、`battery`（电池）。电池温度优先读取
`power_supply/battery/temp` 并从 0.1°C 换算；其余 thermal sysfs 从毫摄氏度换算。
没有独立暴露的内存、eMMC 或 SD 卡温度传感器，不用 SoC 温度代替它们。

`GET /advanced` 的 `device.temperatures` 为同一套汇总值，`device.thermal_zones` 返回所有温控区：
`{ zone, temp, estimated, unverified }`，`temp` 为摄氏度或 `null`。
前/后壳温度为主板、功放和充电器读数计算的估算值。
充电器温控区在室温下约 85°C，尚待核实，因此它和依赖它的壳温估算都标记
`unverified`，不参与概览汇总。这些标记只影响信息展示，不修改内核的温控或充电保护。
概览把电池温度放在电池项，另显示八项汇总；温度详情仍默认显示九项，
“查看更多”展开其余温控区。

`wifi.qr_revision` 为无线配置文件的修改时间，与热点名、加密标志和隐藏标志一起用于刷新
二维码，避免改密码后继续显示旧码。`GET /qr` 按实际加密类型编码热点名、密码和隐藏标志，
并转义特殊字符；加密热点缺少密码时不生成“无密码网络”的二维码。

## 3. 设置项

分类的 `items` 按类型绘制；插件的设置项格式相同。

| 字段 | 说明 |
|---|---|
| `id` | 在分类内唯一 |
| `type` | `toggle`（值为 `true`/`false`）、`choice`（值 + `options`）、`number`（值 + `min`、`max`、`step`、`unit`）、`multi`（值为选项值的数组；一个都不选表示不限制）、`action`（无值，按下即执行）、`info`（只读文字）、`image`（`src`，可带 `caption`：按下这一行全屏显示图片） |
| `label`、`note` | `{ zh, en }`；`note` 显示在该项下面 |
| `options` | `[ { value, label } ]` |
| `confirm` | `true`：需要在 3 秒内再按一次才生效（用于可能断网的操作） |
| `value` | 当前值，修改后会重新读回 |
| `reload` | `true`：修改后页面重新读取整个分类（该修改也会影响其他项的显示） |

## 4. 按键

页面和 SDK 以“类型”报告按键，均在 E5 的键盘上实测：

| 类型 | 按键 | 说明 |
|---|---|---|
| `left` `right` `up` `down` | 方向键 | 主程序中用于翻页和移动焦点 |
| `ok` | 确认（`KEY_SELECT`，WebKit 报告为 "Unidentified"/0） | |
| `back` | 返回（`KEY_BACK` + BackSpace，一次按下） | SDK 会把这一对合并成一次 |
| `digit` | `0`-`9`、`*` | `key` 里是对应字符 |
| `power` | 电源（`PowerOff`） | 始终归主程序：关屏；按电源键后 2 秒内按 `*` 为按键锁 |
| `other` | 侧键（`F1`）、音量等 | 音量加/减（`AudioVolumeUp`/`AudioVolumeDown`）始终归主程序：调节扬声器音量 |

## 5. 插件

### 5.1 目录结构

```
/usr/share/e5-infoscreen/www/plugins/<id>/
    manifest.json      必需
    index.html         页面（manifest 的 "entry"）
    backend.uc         可选：/api/plugins/<id>/...
    ...                页面用到的其他文件（相对路径）
```

`<id>` 由小写字母、数字、`-` 和 `_` 组成，以字母或数字开头；它就是目录名，也是
manifest 的 `id`。复制目录即安装，删除目录即卸载，不需要重启：“应用”页每次打开都会
重新读取列表，“高级”菜单每次加载也会重新读取。

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

| 字段 | 说明 |
|---|---|
| `id`、`api_version`、`name` | 必需 |
| `version`、`description` | 显示在“应用”列表里 |
| `entry` | 页面文件，相对于插件目录；默认 `index.html` |
| `order` | 在“应用”列表中的位置，小的在前；默认 50 |
| `input_method` | `true`：应用可以通过 SDK 把已确认的文字和退格操作发送给宿主最近聚焦的 `input`、`textarea` 或 `contenteditable`；只有输入法应用应设置此项，宿主会忽略其他应用的这些消息。 |
| `settings` | 设置项（第 3 节），类型限 `toggle`、`choice`、`number`，每项存在 uci 选项 `uci`（`配置.节.选项`）里，未设置时用 `default`。它们在“高级”里单独成为一个分类。配置名请用 `e5-plugin-<id>`，第一次修改时会自动创建。 |

### 5.3 前端

页面运行在覆盖各页面的框架里（屏幕 320×480，上有状态栏，下有页脚，可用区域约
320×424 CSS 像素）。先加载 SDK：

```html
<script src="/sdk/e5.js"></script>
```

| SDK | 说明 |
|---|---|
| `e5.id`、`e5.lang`、`e5.version` | 插件 id、`zh`/`en`、接口版本 |
| `e5.api(路径, { body, method })` | 对 `/api/plugins/<id><路径>` 的 `fetch`；带 `body` 时以 JSON 发 POST；成功时得到回复（JSON 或文本），非 2xx 时失败，错误带 `.status` 和 `.data` |
| `e5.core(路径, opts)` | 同上，用于核心接口 `/api/<路径>`（第 2 节） |
| `e5.sms.list()`、`e5.sms.send(号码, 内容, 卡)` | 短信列表（`GET /sms`）；发一条短信（`POST /sms-send`，卡可选），失败时带错误信息 |
| `e5.onKey(fn)` | 每次按键调用 `fn({ kind, key, code, repeat })`；返回 `true` 表示已处理 |
| `e5.onBack(fn)` | `onKey` 没有处理返回键时调用 `fn()`；返回 `true` 表示留在插件里，其他情况关闭插件 |
| `e5.onLang(fn)` | 屏幕语言改变时调用 `fn(lang)` |
| `e5.input(text)` | 把已确认的文字插入宿主最近聚焦的可编辑目标；当前知道有目标时返回 `true` |
| `e5.inputBackspace()` | 在该目标删除一个字符或当前选区；当前有目标时返回 `true` |
| `e5.inputAvailable` | 宿主当前有可编辑目标时为 `true` |
| `e5.onInputTarget(fn)` | 宿主目标可用性改变时调用 `fn(available)` |
| `e5.onInputResult(fn)` | 宿主处理输入操作后调用 `fn({ ok, error })` |
| `e5.toast(文字)` | 在屏幕上显示一条短消息 |
| `e5.keepAwake(on)` | `true`：插件打开期间屏幕不自动熄灭（计时器、测试进行中等）；结束后设回 `false` |
| `e5.exit()` | 关闭插件 |
| `e5.press(el)` | 由按键触发点击 `el`。触摸关闭时（高级 → 屏幕 → 触摸），SDK 会丢弃所有触摸和点击（WebKit 在轻触后自己合成的点击是 untrusted 的，无法区分），只有这种点击能通过。 |
| `e5.t({ zh, en })` | 按屏幕语言取文字 |
| `e5.tzOffset`、`e5.time(ms)` | 设备相对 UTC 的偏移（秒）；把时间格式化为设备时区的 `HH:MM:SS`。OpenWrt 上 WebKit 没有时区数据，`Date` 的本地时间就是 UTC，请用这两个（或 `/status` 里的 `tz_offset`）。 |

按键类型为 `left`、`right`、`up`、`down`、`ok`、`back`、`power`、`menu`、`call`、
`digit` 和 `other`。E5 会话将实体拨号键转换为 `F13` 和 `kind: "call"`，# 键转换为
字面量 `#` 和 `kind: "digit"`。未知按键保持为 `other`，不会触发确认。菜单键由主程序
处理，返回应用列表，不调用插件的按键处理器。

屏幕熄灭时 SDK 会吞掉按键（由主程序点亮屏幕）；电源键归主程序，当前应用可通过
`e5.capturePower(true)` 接管它。触摸和普通网页
一样使用。请用大字号和深色背景（与信息屏一致：背景 `#0b0e13`、卡片 `#161b23`、文字
`#e8ecf2`）；可用字体为 Noto Sans CJK SC 和 DejaVu Sans。

已安装的应用聚焦可编辑控件时，主程序会在第二个覆盖框里自动打开第一个设置了
`input_method: true` 的输入法应用，原应用保持在下面。
`e5.input()` 按目标当前选区插入文字，并派发标准 `input` 事件，因此页面自己的校验和绑定仍会执行。
目标可以是未禁用、未只读的 `input`、`textarea` 或 `contenteditable`。
`e5.inputAvailable` 和 `e5.onInputTarget()` 表示当前是否有目标；宿主只接受
manifest 设置 `input_method: true` 的应用发来的操作，每次插入最多 4096 个字符。

不用 SDK 时，协议是与父框架之间的 `postMessage`：
插件 → 主程序：`{ e5: "ready" }`、`{ e5: "key", kind, key, code, keyCode, repeat }`（每次按键
都要发，主程序据此点亮屏幕、重置息屏计时）、`{ e5: "exit" }`、`{ e5: "toast", text }`、
`{ e5: "keep-awake", on }`、`{ e5: "input", action: "insert", text }` 或
`{ e5: "input", action: "backspace" }`；
主程序 → 插件：`{ e5: "hello", lang, api_version, blank, tz_offset, touch, input_available }`、
`{ e5: "input-target", available }`、`{ e5: "input-result", ok, error }`、
`{ e5: "blank", on }`、`{ e5: "touch", on }`。

### 5.4 后端

`backend.uc` 是 ucode 代码（raw 模式，不写 `{% %}`），返回一个函数：接收上下文，返回路由表：

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

* 路由写成 `"<方法> <路径>"`，路径是 `/api/plugins/<id>` 之后的部分，没有时为 `/`。
* `req` 为 `{ method, path, query, body }`：`query` 是解析后的查询参数，`body` 是 POST
  的 JSON。
* 处理函数返回一个对象（以 JSON 发送），或者返回 `{ status, type, body }` 发送其他内容
  （`body` 为字符串）。
* 每个请求都在接口服务里重新运行，变量不会保留到下一个请求（状态请用
  `ctx.state_put` 保存），每个请求最多 30 秒。耗时的工作请放到后台（`ctx.run('... &')`），
  让页面轮询一个读取其状态的路由。

`ctx`：

| | 说明 |
|---|---|
| `ctx.api_version` | 1 |
| `ctx.sh(cmd)` | 运行 shell 命令，返回输出（字符串）或 `null`。不要未经转义就用 `req` 拼接 `cmd`：`req` 是数据。 |
| `ctx.sh_json(cmd)` | 同上，结果按 JSON 解析（不是 JSON 时为 `null`） |
| `ctx.run(cmd)` | 运行命令，返回退出状态 |
| `ctx.at(cmd, timeout)` | 通过 ModemManager 发 AT 指令（AT 通道只有一个所有者），返回不含 `OK` 的回复或 `null`；不拒绝任何指令 |
| `ctx.at_console(cmd, timeout)` | 同 `POST /at`：`{ ok, reply }` 或 `{ ok: false, error }`，对可能让基带直到重启前不可用的指令带 `warning: true` |
| `ctx.modem()` | `/status` 中的 `modem` 部分 |
| `ctx.sim_card()` | 模组对应的 SIM 卡，0 或 1 |
| `ctx.cells()` | 服务小区和邻区，`[ { type, serving, pci, arfcn, band, rsrp, rsrq, sinr, bandwidth_mhz } ]` |
| `ctx.ubus(对象, 方法, 参数)` | ubus 调用，返回回复或 `null` |
| `ctx.uci()` | uci cursor（`get`、`set`、`commit`、`foreach` 等） |
| `ctx.state_get(名称, 最大秒数)`、`ctx.state_put(名称, 值)` | 跨请求保存的 JSON 状态，按插件区分（在 `/tmp/run` 下，重启后不保留）；超过 `最大秒数` 时 `state_get` 返回 `null` |
| `ctx.forget(名称)` | 删除一项状态 |
| `ctx.read_trim(路径)`、`ctx.read_num(路径)` | 读文件内容，去掉首尾空白 / 转成数字 |
| `ctx.log(文字)` | 写一行系统日志（`logread -e e5-infoscreen/<id>`） |

### 5.5 应用包

应用通过应用包安装：一个 `.tar.gz`（`.tgz`）或 `.zip`，插件目录里的文件放在压缩包根目录，或放在唯一的顶层目录里：

```sh
tar -czf nettest-1.0.tar.gz -C www/plugins nettest     # 或：cd www/plugins && zip -r nettest-1.0.zip nettest
```

在 LuCI（`http://192.168.9.1`，服务 → 信息屏应用，“上传并安装”）里安装，或在设备上运行
`/usr/libexec/e5-infoscreen/plugin install 文件`。manifest 必须有效（`id` 由小写字母、数字、`-`、`_` 组成，
`api_version` 不高于信息屏的版本，有 `name`）；压缩包里不能有绝对路径、`..` 或链接，解压后不超过 20 MB。
同一 id 的应用会被替换（即升级）。安装的应用保存在 `/etc/e5-infoscreen/plugins`，更新系统镜像后仍在；内置应用在
镜像里。可以在 LuCI、屏幕上（高级 → 应用管理）或用 `plugin remove ID` 卸载。`GET /plugins` 的每个 manifest 带
`builtin`（镜像自带的为 true）；`POST /plugins-remove { id }` 卸载。

`www/plugins/` 下的两个插件就是示例：`calculator`（页面、按键、`onBack`）和
`nettest`（后端、设置项、`keepAwake`）。

## 6. 应用商店和信息屏自身的在线更新

`GET /status` 里的 `usb` 是 USB 连接的真实状态：`link` 为 `none`（没插线）、`charger`（仅充电）、`host`
（接了电脑但没被识别，e5-linux 的 `e5-usb-watch` 会在插拔后重新连上）、`enumerated`（电脑已识别）、
`lease`（电脑已拿到地址）、`online`（电脑正在通过 E5 上网，流量超过 2 KB/s）。

`usb.reset` 返回修复能力、`busy`、`state`、`stage`、`message` 和 `updated`。
向 `POST /settings/usb` 提交 `{ "id": "reset" }` 开始修复；提交 `{ "id": "check" }`
执行只读连接检查并保存报告，`GET /settings/usb` 显示检查结论和修复结果。
LuCI 通过已认证的 `e5-infoscreen.usb_reset`、`e5-infoscreen.network_check` RPC 调用。
`wifi.state` 为 `off`、`on`、`starting` 或 `failed`；启动等待最多 30 秒，
`wifi.error` 显示原始错误或超时原因。

**应用商店**是 [`Enceka/infoscreen-plugins`](https://github.com/Enceka/infoscreen-plugins)：应用放在
`plugins/<id>/`，CI（`tools/check.py`）检查 manifest、文件、风格和前端不许做的事，含后台的应用会标出来供审阅；
通过后发布到 GitHub Pages（`index.json` 和每个应用一个包）。设备上 `plugin store` 获取索引
（`e5-infoscreen.main.store_url`，默认 `https://enceka.github.io/infoscreen-plugins/index.json`），
`plugin get ID` 下载应用包，按索引核对大小和 SHA-256、核对包里确实是这个应用，再照常安装（5.5 节）。
屏幕上：高级 → 应用管理 → 应用商店（安装、更新，按第二下确认）。

**信息屏在线更新**不需要新的系统镜像：`/usr/libexec/e5-infoscreen/update` 读取发布说明
（`e5-infoscreen.main.update_url`，默认是 `https://enceka.github.io/infoscreen/latest.json`：
`{ version, url, sha256, size, notes }`），`update apply` 下载更新包（本仓库的 `root/`，由
`tools/make-release.sh` 生成），核对大小、SHA-256 和包里每一个路径
（只能在 `usr/`、`www/`、`etc/` 下，不能碰 `etc/config` 和 `etc/e5-infoscreen`；不能有 `..`、绝对路径、链接），
先把要被替换的文件存到 `/etc/e5-infoscreen/update-backup.tar.gz`，再解包并重启信息屏；`update rollback` 恢复备份。
版本号在 `/usr/share/e5-infoscreen/VERSION`。屏幕上：高级 → 系统 → 检查更新，然后“更新到 x.y.z”。
概览发现新版本时显示浮窗，可展开更新日志、立即更新或推迟更新。推迟会把
`update_defer_version` 和 `update_defer_until` 存入 `e5-infoscreen.main`，同一版本
24 小时内不再提醒，重启信息屏也保留；更高版本仍会提示。
后台检查和安装通过 `update-job` 互斥执行。提示不自动安装，不点亮熄灭或锁定的屏幕。
需要新软件包或更新系统的版本不能这样装，要更新系统镜像。

发布新版本：提高 `VERSION` 并推送。CI 会自动生成安装包和
`https://enceka.github.io/infoscreen/latest.json`，同时把同一份安装包和 JSON 上传到 GitHub
的 `v<版本>` 发布中，让仍使用旧更新地址的设备继续兼容。

## 应用通知（API 2，信息屏 1.5.0 起）

前端：`e5.notify({id,title,body,wake,ttl})` 提交通知，
`e5.clearNotification(id)` 撤销。后台对应 `ctx.notify()` 和
`ctx.clear_notification()`。通知按插件 ID 隔离；title 支持中英文标签，
body 只显示文本，wake 请求亮屏，ttl 默认 60 秒、最大 86400 秒。
`POST /api/plugins/<id>/_notify` 只允许已安装插件提交；
`GET /api/notifications` 返回有效通知。

应用关闭后仍需提醒时，在 manifest 声明 `notifications: true`，并实现
后台 `GET /notifications`，返回 `{notifications:[{id,title,body,wake}]}`。
这个接口必须只读；条件结束时返回空列表。核心统一显示状态栏、弹窗、
“查看”和“稍后”，并遵守按键锁。查看只打开应用，不会自动接听、拨号等。
通知覆盖层显示时，底下的插件不会收到操作输入。

`e5.capturePower(on)` 允许应用在操作期间接收原有锁屏/电源事件，不改变物理
按键映射。输入法使用 `e5.input()` 时必须声明 `api_version: 2`，原 API 1 应用保持兼容。
