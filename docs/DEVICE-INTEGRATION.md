# Device UI integration

The mobile interface formerly staged by e5-linux's T551M adaptation is now part of infoscreen itself.
The same committed frontend/API/services support E5, T551M and M8 through runtime device configuration.
`tools/stage.py` copies `root/` into an offline root; it does not patch a pinned base or generate JavaScript.

Audio, modem, SMS delivery, CPU/USB controllers and sound assets belong to the parent project's
`device/openwrt_device_common` and `device/linux_device_common` repositories. Phone is maintained
in `Enceka/infoscreen-plugins`. The common OpenWrt staging adapter installs these dependencies.
Installed t551m-* RPC/service names remain compatibility interfaces.

The adaptation marker is `e5-device-ui-v1`; a full device image rebuild normalizes older E5/M8
markers to this common marker. This source integration does not bump `.version` or publish a new
screen release. A future release must choose its version and retain these runtime dependencies.

The pre-migration implementation and attribution are retained in e5-linux's public history at
commit `b7282af`. In particular, the low-wakeup shared transport/memory work, phone/SIM updates,
wallpapers, navigation, power management and screen settings came from these commits:

```text
80af619 feat(m8): add VIPME M8 support and shared OpenWrt fixes
a5d3498 feat(audio): add customizable call and SMS sounds
d326974 chore: retire FM_U20 support and retain dashboard fixes
f4c29d1 fix(infoscreen): curb memory growth with low-wakeup shared transports
1c1b8e1 fix(voice): recover transient call state and settle repeated hangups
e348bc4 Add guarded userdata F2FS recovery and charger wake; validate E5 OTG
863ab8c feat(e5): verify boot voltage control and align CPU settings UI
1f0c918 feat(e5): port mobile features and correct native charging policy
37c5e7b fix(t551m): synchronize SIM views and refine interface spacing
b026b49 fix(modem): decode UCS2 operator names for screen and dashboard
85e8244 fix(screen): make swipe unlock more forgiving
cec3112 fix:styles
ed57a9d t551m: add CPU controls and synchronize web and screen features
0c1c937 t551m: center screen loading and refine header navigation
962114f t551m: improve dual-SIM, USB, power and call audio support
edbc146 t551m: fix modem startup and charging shutdown, improve calls and SMS UI
6b1fbac t551m: add OpenWrt device support and mobile interface
```
