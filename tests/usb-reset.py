#!/usr/bin/env python3
"""Exercise recovery with fake sysfs, ip and DHCP; never touches USB hardware."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'root/usr/libexec/e5-infoscreen/usb-reset'


class Recovery(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.sys = self.base / 'sys'
        self.run = self.base / 'run'
        self.udc = self.sys / 'class/udc/controller'
        gadget = self.sys / 'kernel/config/usb_gadget/linux'
        for path in [self.udc, gadget, self.sys / 'class/net/usb0', self.sys / 'class/net/br-lan/brif', self.run / 'lock']:
            path.mkdir(parents=True)
        (gadget / 'UDC').write_text('controller\n')
        (self.udc / 'soft_connect').write_text('initial\n')
        (self.udc / 'state').write_text('not attached\n')
        self.bin = self.base / 'bin'; self.bin.mkdir()
        self.command('ip', '''echo "$*" >> "$E5_TEST_LOG"
case "$*" in '-4 -o addr show dev br-lan')
 [ "${E5_TEST_NO_ADDRESS:-0}" = 1 ] || echo '1: br-lan inet 192.168.9.1/24'
 ;; esac
exit 0
''')
        self.command('dnsmasq', '''echo "dnsmasq $*" >> "$E5_TEST_LOG"
[ "${E5_TEST_DHCP_FAIL:-0}" != 1 ] || { echo 'bad dhcp configuration' >&2; exit 1; }
''')
        self.command('sleep', '''if [ "${E5_TEST_ENUM_FAIL:-0}" != 1 ] && [ "$(cat "$E5_TEST_SOFT")" = connect ]; then
 echo configured > "$E5_TEST_STATE"
fi
''')
        self.command('dmesg', "echo 'usb: simulated status'\n")
        self.env = {**os.environ, 'PATH': str(self.bin) + ':' + os.environ['PATH'],
                    'E5_USB_SYSFS': str(self.sys), 'E5_USB_RESET_RUN': str(self.run),
                    'E5_USB_DHCP_SERVICE': str(self.bin / 'dnsmasq'), 'E5_USB_RESET_WAIT': '2',
                    'E5_TEST_SOFT': str(self.udc / 'soft_connect'), 'E5_TEST_STATE': str(self.udc / 'state'),
                    'E5_TEST_LOG': str(self.base / 'commands')}

    def command(self, name, body):
        file = self.bin / name; file.write_text('#!/bin/sh\n' + body); file.chmod(0o755)

    def reset(self, **changes):
        return subprocess.run([str(TOOL), 'run'], env={**self.env, **changes}, capture_output=True, text=True, timeout=5)

    def test_reenumerates_without_taking_ncm_down(self):
        result = self.reset()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.run / 'state').read_text().strip(), 'done')
        log = (self.base / 'commands').read_text()
        self.assertIn('link set usb0 master br-lan', log)
        self.assertIn('dnsmasq reload', log)
        self.assertNotIn('down', log)
        self.assertEqual((self.udc / 'soft_connect').read_text().strip(), 'connect')
        self.assertFalse((self.run / 'lock').exists())

    def test_missing_ipv4_reports_lan_stage(self):
        result = self.reset(E5_TEST_NO_ADDRESS='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('br-lan has no IPv4', (self.run / 'message').read_text())
        self.assertEqual((self.run / 'stage').read_text().strip(), 'lan')

    def test_dhcp_failure_does_not_disconnect_host(self):
        result = self.reset(E5_TEST_DHCP_FAIL='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.run / 'stage').read_text().strip(), 'dhcp')
        self.assertEqual((self.udc / 'soft_connect').read_text().strip(), 'initial')
        self.assertIn('bad dhcp configuration', (self.run / 'message').read_text())
        self.assertFalse((self.run / 'lock').exists())

    def test_timeout_records_actual_controller_state(self):
        result = self.reset(E5_TEST_ENUM_FAIL='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('controller state: not attached', (self.run / 'message').read_text())
        self.assertEqual((self.run / 'state').read_text().strip(), 'failed')

    def test_unavailable_controller_does_not_queue(self):
        (self.udc / 'soft_connect').unlink()
        result = subprocess.run([str(TOOL), 'start'], env=self.env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unavailable', result.stderr)


if __name__ == '__main__':
    unittest.main()
