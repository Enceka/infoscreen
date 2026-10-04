These tests use a disposable OpenWrt container and mocked browser requests.
They never send modem call-control commands. Do not run the API fixture test
on a live E5; it creates a temporary installed test plugin.

Run `notifications.uc` with a writable copy of
`root/usr/share/e5-infoscreen` at `/usr/share/e5-infoscreen`, and copy
`tests/fixtures/fixture` into its `www/plugins/fixture` directory first.
Use an OpenWrt image with ucode/fs/uci/ubus modules.

For the browser test, install Playwright in a separate tool directory and run
`E5_PLAYWRIGHT=/path/to/playwright node tests/notifications-ui.cjs`.
All requests are intercepted at `screen.test`; the test never connects to E5.

`keypad-ui.cjs` uses the same browser setup to verify the host and SDK key
handling, including unknown keys, menu, call, #, confirm and power capture.
The XKB rules are checked separately on OpenWrt with
`XKB_CONFIG_EXTRA_PATH=/usr/share/e5-infoscreen/xkb xkbcli compile-keymap --rules e5 --layout us --options e5:keypad --test`.

`python3 tests/usb-reset.py` runs recovery against fake sysfs, ip and DHCP,
covering successful enumeration and failures without touching USB hardware.
`sh tests/network-checks.sh` runs USB settings and Wi-Fi startup-state checks
in a disposable OpenWrt Docker image (`E5_TEST_IMAGE` overrides the image).
`network-ui.cjs` uses the same Playwright setup to test the diagnostic button,
recovery confirmation/result and failed hotspot display with mocked requests.
