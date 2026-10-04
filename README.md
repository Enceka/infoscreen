# e5-infoscreen

> 中文：[`README.zh-CN.md`](README.zh-CN.md)

An info screen for the Rongyue E5's 320×480 panel under OpenWrt, the OpenWrt
that [e5-linux](https://github.com/Enceka/e5-linux) runs on the device
(`openwrt/` there).  It shows the modem, the traffic, the hotspot and the
device at a glance, and is driven by touch and by the keypad.

| page | shows |
|---|---|
| Overview | download/upload rate side by side, traffic since boot, network state (5G/4G, IPv4/IPv6), hotspot, clients, battery with its temperature, current (+ charging, - discharging) and voltage, memory and storage in use, CPU/GPU/SoC/LTE/NR/multimedia/board/RF PA temperatures; update notice with expandable release notes, Update now and Later (24 hours) |
| Signal | technology and band (n41, B3, ...), RSRP/RSRQ/SINR with grades, PCI, ARFCN, bandwidth, neighbour cells, the subscribed rate (the network's AMBR for the data context: `AT+CGEQOSRDP` / `AT+C5GQOSRDP`, with the QCI/5QI) |
| Traffic | today's and this month's download/upload, WAN (the modem's interface, what the carrier counts) and LAN (the bridge of USB and hotspot) apart; since boot and the last 7 days for the WAN (vnstat, kept in `/etc/vnstat`; 高级 -> 系统 clears it) |
| SMS | the received messages, newest first, unread ones marked; open one to read it, delete it (press twice) |
| Hotspot | SSID, a QR code to join, the passphrase on request, on/off, the clients (Wi-Fi and USB) |
| Device | battery, uptime, time online, load, memory, LAN/IPv4/IPv6 addresses, brightness, reconnect |
| Details (高级信息) | device (system, image, kernel, storage, temperature, battery voltage); baseband (model, firmware, 5G SA, modes); band locks (LTE, NR) and cell locks, decoded from `AT+SPLBAND` / `AT+SPFORCEFRQ`; SIM (active slot, operator, registration); the identifiers on request |
| Settings (高级) | by function: **network** (network mode 5G/4G/3G, 5G/4G, 5G only (SA), 4G only; 5G access SA + NSA or NSA only; APN switch, automatic from the SIM by default; LTE/NR band lock, default bands, cell lock; reconnect), **AT commands** (a list of reads, run and shown; any command through `/api/at`), **devices** (block internet, kick off Wi-Fi), **Bluetooth** (on/off, search, pair and connect headphones or speakers -- the sound then plays there -- disconnect, forget), **charging** (limit, resume level, charge to full once -- e5-linux's `e5-charge`), **notifications** (SMS vibration, light up, SMS sound), **sound** (volume, a test sound -- when e5-linux's `e5-volume` is there), **screen** (brightness, screen-off time, touch on/off, language), **system** (time zone, clock with seconds, clear traffic records, what the next reboot boots, the default boot (Linux or Android), reboot, power off, boot Android once; Debian once is `e5-os debian --once` on the command line), **app management** (store, installed apps, and each app's own settings when available) |
| Apps | the installed plugins; the core image includes a calculator and network test, and the E5 OpenWrt image also preinstalls the separately updated Phone app |

The status bar carries the operator, the technology, signal bars, the battery
and the time, and ✉ with the number of unread messages.  A new message lights
the screen and opens itself (e5-linux's `e5-sms-notify` vibrates and keeps the
unread list, `/tmp/run/e5-sms/unread`; `e5-notify.sms.screen=0` turns the
lighting up off).  Opening the SMS page marks the messages read.  The SIM's
and the device's identities (IMEI, ICCID, IMSI, own number) are shown only on
the Advanced page, after "show identifiers", and served only by the one
endpoint that button calls (`/api/identity`).

## How it works

Everything is an OpenWrt package except the files in `root/`:

* **Display**: `cage`, a single-application Wayland compositor (wlroots), on
  the panel's KMS device, rendering with Mesa's **panfrost** driver on the
  Mali-G57 (`libmesa-panfrost`); `seatd` hands it the devices.
* **The page**: `cog`, the WPE WebKit browser, full screen, showing
  `http://127.0.0.1:8088/` (`root/usr/share/e5-infoscreen/www`).
* **The data**: a second `uhttpd` instance, on 127.0.0.1:8088 only, with a
  ucode handler (`api.uc`): ModemManager (`mmcli -J`), netifd, hostapd and
  the wireless configuration (ubus, uci), `/sys` for the battery, the
  backlight and the traffic counters.  Nothing outside the device reaches it,
  so it has no login; LuCI stays where it is, on port 80.
* **Fonts**: Noto Sans CJK, in the image when OpenWrt has one of its own
  (e5-linux's standalone install), else from the Debian root image it runs
  from, bound in under `/usr/share/fonts` (WebKit's sandboxed web process sees
  `/usr`, not `/mnt`); DejaVu Sans as the fallback.
* **Screen power**: the backlight goes off after the idle time (60 s by
  default) or on the power key; the first touch or key after that only wakes
  the screen -- unless the keys are locked (power, then `*`: see Keys).

`/etc/init.d/e5-infoscreen` runs the three parts (`seatd`, `api`, `ui`) as procd
instances.

## Keys

Measured on the device (WebKit's names in brackets):

| key | does |
|---|---|
| left / right | previous / next page |
| up / down | move between the buttons of a page, or scroll it |
| confirm (`KEY_SELECT`, `Enter`) | press the focused button |
| menu (`KEY_MENU`, `ContextMenu`) | open the application list; leave an open application |
| call (`KEY_PHONE`, `F13`) | the open application receives `kind: "call"`; Phone dials or answers |
| # (`KEY_NUMERIC_POUND`, `#`) | a literal # in an application, separate from call and confirm |
| back (`KEY_BACK` + BackSpace) | close the message, leave the button, or go to the first page |
| 1-9 | go to that page |
| side key (`F1`) | the hotspot page (to show the QR code) |
| power (`PowerOff`) | screen off |
| power, then `*` within 2 s | key lock: the screen stays dark -- no key, touch or new SMS lights it (the motor pulses once); power, then `*` again unlocks and lights it.  The volume keys still work |
| volume up / down | the speaker volume (16 levels, 0 mute), silently, with the level over the page; works with the screen dark, without lighting it |

The session uses the E5 XKB rules to translate these keys before WPE sees them.
The kernel evdev map stays intact, including the hangup/power key. Unknown keys
never confirm a control. Phone takes the power key only while a call exists.

## Plugins

A plugin is a directory under `/usr/share/e5-infoscreen/www/plugins/<id>/`: a
`manifest.json`, a page that loads `/sdk/e5.js`, and optionally a ucode
`backend.uc` and settings.  [`docs/API.md`](docs/API.md) is the reference for
the core API, the settings items, the SDK and the backend context.

## Apps

The Apps page shows the plugins; the core image contains the calculator and
network-test apps, while the OpenWrt E5 image preinstalls the separately
updatable Phone app (`1.4`). Phone provides the dialer, contacts, call
controls, incoming-call notification, ringtone/vibration settings and the
shared cellular voice route. Install more in LuCI (服务 -> 信息屏应用: upload
a `.tar.gz` or `.zip`), uninstall there or on the screen (高级 -> 应用管理).
The package format: [`docs/API.md`](docs/API.md) 5.5.

LuCI has two matching pages under Services: **Info screen apps** lists, uploads
and removes plugins, and **Info screen settings** edits the screen's UCI options.
The apps page also accepts an official `e5-infoscreen-<version>.tar.gz` update
package and installs it with the same archive checks as the on-screen updater.

The current core release is `1.6.5` (API 2). Its XKB rules give the physical
menu, call, confirm and `#` keys distinct browser events; unknown keys never
activate the focused control. Phone navigation follows the visible keypad
grid: up/down move within a column and left/right within a row.

Settings → USB offers **Check connections** and **Reset USB connection**.
The check reads USB enumeration, Wi-Fi, LAN, DHCP and relevant logs without
changing network settings. Results appear on the panel and are saved in
`/etc/e5-infoscreen/diagnostics/network.json`; photograph the results when the
device cannot be reached. LuCI's Info screen settings can also run the check
and download its report. Wi-Fi credentials are removed from the report.
USB recovery briefly disconnects the computer, restores the bridge/DHCP path
and re-enumerates the gadget while preserving settings. Errors identify the
failed stage. An AP startup that exceeds 30 seconds is shown as failed.

The project page is [`enceka.github.io/infoscreen`](https://enceka.github.io/infoscreen/).
It reads the same `latest.json` as the device updater and links to the Pages-hosted
package. The GitHub Release feed remains available for older installations that
still use the previous update URL.

## Install

On an E5 running e5-linux's OpenWrt, with the WAN up (the packages come from
OpenWrt's repositories):

```sh
./install.sh              # 192.168.9.1 over SSH
./install.sh 192.168.9.1
```

It installs `packages.txt` (about 200 MB with WebKit and Mesa), copies `root/`,
and enables and starts the service.  A reinstall keeps `/etc/config/e5-infoscreen`.

## Settings

`/etc/config/e5-infoscreen` (`uci`, then `/etc/init.d/e5-infoscreen restart`):

| option | default | |
|---|---|---|
| `touch` | `1` | `0`: the panel ignores touch, the keys only |
| `enabled` | `1` | `0`: no info screen, the panel stays dark |
| `idle` | `60` | seconds before the backlight goes off, `0` never |
| `brightness` | `120` | backlight level while on, 1-255 (the Device page changes and saves it) |
| `lang` | `zh` | `zh` or `en` |
| `update_url` | (unset) | optional update JSON override; empty uses the Pages feed |
| `store_url` | (unset) | optional app-store JSON override; empty uses the official store |
| `donate_seen` | (unset) | `1` once the 赞赏码 has been shown at the first start after an install; after that it is under 高级 -> 关于 -> 赞赏 only |
| `update_defer_version` | (unset) | Release whose reminder is postponed; a newer release still appears |
| `update_defer_until` | (unset) | Unix timestamp of the next reminder, 24 hours after choosing Later |

## Debugging

* `logread -e e5-infoscreen`: the compositor, and the page's console.
* `wget -q -O - http://127.0.0.1:8088/api/status` on the device: all the data.
* `/tmp/e5-infoscreen-keys.log`: the first 200 keys each start of the page,
  as WebKit reports them.

## Maintainer

Enceka <enceka@yeah.net>.  The screen shows this under 高级 -> 关于 (About).

## Disclaimer

Unofficial software, not affiliated with or endorsed by Rongyue or the makers
of the device and its chips.  It is provided "as is", without warranty of any
kind (see [`LICENSE`](LICENSE)).  What it can change -- the network mode, band
and cell locks, raw AT commands, charging -- can cut the connection, leave the
modem or the battery in an unexpected state, or void the device's warranty.
You use it at your own risk; check the local rules for the bands and radio
settings you choose.

## License

MIT, © 2026 Enceka, see [`LICENSE`](LICENSE).
