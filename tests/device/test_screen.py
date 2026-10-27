"""Execute the production C screen-power code against fake sysfs/UCI."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

SOURCE=Path(__file__).resolve().parents[2]/'src/native/screen-power.c'


class ScreenPower(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory();cls.binary=Path(cls.build.name)/'screen-power'
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-DSCREEN_POWER_TEST',str(SOURCE),'-o',str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):cls.build.cleanup()

    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.light=self.root/'sys/class/backlight/panel/brightness'
        self.touch=self.root/'sys/class/input/input7/inhibited'
        for p in (self.light,self.touch):p.parent.mkdir(parents=True,exist_ok=True);p.write_text('0\n')
        (self.touch.parent/'properties').write_text('2\n')
        for d in ('run','etc/config','tmp/.uci','bin'):(self.root/d).mkdir(parents=True,exist_ok=True)
        self.setting=self.root/'etc/config/e5-infoscreen';self.setting.write_text('1\n')
        uci=self.root/'bin/uci';uci.write_text('#!/bin/sh\ncat "$E5_SCREEN_ROOT/etc/config/e5-infoscreen"\n');uci.chmod(0o755)
        self.trace=self.root/'trace'
        self.env=dict(os.environ,E5_SCREEN_ROOT=str(self.root),SCREEN_POWER_TRACE=str(self.trace),PATH=str(self.root/'bin')+':'+os.environ['PATH'])

    def call(self,*args,ok=True):
        r=subprocess.run([str(self.binary),*args],env=self.env,capture_output=True,text=True,timeout=4)
        self.assertEqual(r.returncode==0,ok,r.stderr)
        return r

    def test_dark_wake_order_and_repeated_operations(self):
        self.call('set','0');self.call('set','180')
        self.assertEqual(self.trace.read_text().splitlines(),['inhibited=1','inhibited=0','brightness=180'])
        for _ in range(4):self.call('set','0');self.call('set','180')
        self.assertEqual(self.touch.read_text(),'0\n')

    def test_explicit_touch_off_and_config_change(self):
        self.setting.write_text('0\n');self.call('set','255');self.assertEqual(self.touch.read_text(),'1\n')
        self.setting.write_text('1\n');self.call('sync');self.assertEqual(self.touch.read_text(),'0\n')

    def test_same_brightness_skips_driver_write_but_reconciles_touch(self):
        self.call('set','120')
        self.trace.write_text('')
        self.setting.write_text('0\n');self.call('set','120')
        self.assertEqual(self.trace.read_text().splitlines(),['inhibited=1'])
        self.setting.write_text('1\n');self.call('set','120')
        self.assertEqual(self.trace.read_text().splitlines(),['inhibited=1','inhibited=0'])
        self.call('set','120')
        self.assertEqual(self.trace.read_text().splitlines(),['inhibited=1','inhibited=0'])

    def test_each_backlight_is_reconciled_independently(self):
        second=self.root/'sys/class/backlight/second/brightness'
        second.parent.mkdir();second.write_text('20\n')
        self.light.write_text('120\n')
        self.call('set','120')
        self.assertEqual(second.read_text(),'120\n')
        self.assertEqual(self.trace.read_text().splitlines(),['brightness=120'])

    def test_driver_names_and_indirect_input(self):
        for name in ('tlsc6x_touch','fts_ts','other-panel'):
            (self.touch.parent/'name').write_text(name);self.call('sync')
        (self.touch.parent/'properties').write_text('0\n');self.call('sync',ok=False)

    def test_external_state_and_restart(self):
        self.light.write_text('120\n');self.touch.write_text('1\n');self.call('sync');self.assertEqual(self.touch.read_text(),'0\n')
        self.light.write_text('0\n');self.call('sync');self.assertEqual(self.touch.read_text(),'1\n')

    def test_invalid_commands_and_clamping(self):
        for args in [('bad',),('set',),('set','bad'),('sync','1')]:self.call(*args,ok=False)
        self.call('set','999');self.assertEqual(self.light.read_text(),'255\n')
        self.call('set','-20');self.assertEqual(self.light.read_text(),'0\n')

    def test_event_watch_config_external_brightness_idle_and_shutdown(self):
        p=subprocess.Popen([str(self.binary),'watch'],env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        def wait_value(path,value):
            deadline=time.monotonic()+2
            while time.monotonic()<deadline:
                if path.read_text()==value:return
                time.sleep(.01)
            self.fail('Event was not reconciled: '+str(path))
        try:
            wait_value(self.touch,'1\n')
            time.sleep(.05)
            before=self.trace.read_text()
            self.light.write_text('90\n');wait_value(self.touch,'0\n')
            replacement=self.root/'new';replacement.write_text('0\n');replacement.replace(self.setting)
            wait_value(self.touch,'1\n')
            before=self.trace.read_text();time.sleep(.15);self.assertEqual(self.trace.read_text(),before)
        finally:
            p.terminate();out,err=p.communicate(timeout=3);self.assertEqual(p.returncode,0,err)


if __name__=='__main__':unittest.main()
