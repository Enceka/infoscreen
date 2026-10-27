"""Standalone staging and native pairing checks. Temporary roots only."""
import hashlib
import json
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

TOP=Path(__file__).resolve().parents[2]
stage=runpy.run_path(str(TOP/'tools/stage.py'))
native=runpy.run_path(str(TOP/'tools/build-native.py'))

class Standalone(unittest.TestCase):
    def test_native_sources_match_binaries_and_recipe(self):
        native['verify']()

    def test_native_corruption_is_rejected_before_staging(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);meta=json.loads(native['METADATA'].read_text())
            name='t551m-screen-power';p=root/name;p.write_bytes(b'broken')
            with patch.dict(native['verify'].__globals__,OUT=root):
                with self.assertRaisesRegex(ValueError,'mismatch'):
                    native['verify']()

    def test_repeated_staging_preserves_user_data(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            keep={'etc/config/e5-infoscreen':b'user setting\n',
                  'etc/e5/contacts.json':b'[]\n',
                  'etc/e5-sms/inbox.json':b'{"messages":[]}\n',
                  'etc/e5-infoscreen/plugins/custom/settings.json':b'{"keep":true}\n'}
            for name,data in keep.items():
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
            for _ in range(2):
                stage['stage'](root)
                for name,data in keep.items():self.assertEqual((root/name).read_bytes(),data)
                for name in native['TARGETS']:
                    self.assertEqual((root/'usr/libexec'/name).read_bytes(),(TOP/'root/usr/libexec'/name).read_bytes())
                for n in ('S94t551m-screen-power','S96t551m-sim-status','S98e5-voice-audio'):
                    self.assertTrue((root/'etc/rc.d'/n).is_symlink())

    def test_stage_refuses_live_or_source_tree(self):
        for target in ('/',TOP,TOP/'root'):
            with self.subTest(target=target),self.assertRaises(ValueError):stage['stage'](target)

    def test_one_apn_and_phone_copy(self):
        sources=stage['sources']();app=sources['www/app.js'];index=sources['www/index.html']
        self.assertEqual(app.count('const APN_TEXT ='),1)
        self.assertNotIn('src="apn.js"',index)
        self.assertFalse((TOP/'root/usr/share/e5-infoscreen/www/apn.js').exists())
        self.assertFalse((TOP/'root/etc/e5-infoscreen/plugins/phone').exists())
        self.assertTrue((TOP/'root/usr/share/e5-infoscreen/www/plugins/phone/UPSTREAM.json').is_file())

    def test_services_use_native_programs_and_keep_abi(self):
        for name in ('t551m-screen-power','t551m-sim-status','e5-voice-audio'):
            service=(TOP/'root/etc/init.d'/name).read_text()
            self.assertNotIn('python3',service)
            self.assertIn('/usr/libexec/'+name,service)
        for script in ('transport.js','calls.js','power-menu.js'):
            self.assertEqual(stage['sources']()['www/index.html'].count('src="'+script+'"'),1)

    def test_export_has_no_device_identity_or_modem_driver(self):
        self.assertFalse((TOP/'root/etc/e5/device-profile').exists())
        self.assertFalse((TOP/'root/opt/e5/nv').exists())
        self.assertFalse((TOP/'root/lib/modules').exists())
        info=json.loads((TOP/'docs/E5-SOURCE.json').read_text(encoding='utf-8'))
        self.assertEqual(len(info['revision']),40)
        for name in info['files']:
            self.assertNotIn('/nv/',name)
            self.assertNotIn('contacts.json',name)

if __name__=='__main__':unittest.main()
