"""Same operator-name cases for the screen and LuCI collectors; no modem I/O."""
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import unittest
import os
import sys

TOP=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(TOP/'tests'))
from native_support import Simulator
PARSER=Path(os.environ['INFOSCREEN_DASHBOARD_PARSER']) if os.environ.get('INFOSCREEN_DASHBOARD_PARSER') else None
CASES=[]
for name in ['中国联通','中国移动','中国电信','中国广电','CHN-UNICOM','Vodafone','Café','12','A\\B','A"B','📶']:
    CASES.append(('UCS2','0',name.encode('utf-16-be').hex().upper(),name))
CASES += [
    ('UCS2','1','0043005500430043','CUCC'),
    ('UCS2','0','中国联通','中国联通'),
    ('UCS2','0','CHN-UNICOM','CHN-UNICOM'),
    ('GSM','0','CAFE','CAFE'),
    ('IRA','0','4E2D56FD8054901A','4E2D56FD8054901A'),
    (None,'0','4E2D56FD8054901A','4E2D56FD8054901A'),
    ('UCS2','2','46011','46011'),
    ('UCS2','2','460001','460001'),
    ('UCS2','2','00460031','00460031'),
    ('UCS2','0','D800','D800'),
    ('UCS2','0','DC00','DC00'),
    ('UCS2','0','0041D8000042','0041D8000042'),
    ('UCS2','0','004100000042','004100000042'),
    ('UCS2','0','0041001F0042','0041001F0042'),
    ('UCS2','0','0041007F0042','0041007F0042'),
    ('UCS2','0','0041009F0042','0041009F0042'),
    ('UCS2','0','00410','00410'),
    ('UCS2','0','004X','004X'),
    ('UCS2','0','',''),
]

class Operators(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.sims=Simulator()
    @classmethod
    def tearDownClass(cls):cls.sims.close()
    def check_cases(self, web):
        for charset,fmt,raw,expected in CASES:
            with self.subTest(charset=charset,format=fmt,raw=raw):
                reply=(f'+CSCS: "{charset}"\n' if charset else '')+f'+COPS: 0,{fmt},"{raw}",7\nOK\n'
                if web:
                    result=json.loads(subprocess.check_output(['ucode',str(PARSER),reply],text=True))
                    self.assertEqual(result,{'name':None if fmt=='2' else expected or None,'plmn':(expected or None) if fmt=='2' else None,'act':7})
                else:self.assertEqual(self.sims.operator_name(reply),expected or None)
    def test_screen(self):self.check_cases(False)
    @unittest.skipUnless(shutil.which('ucode') and PARSER and PARSER.is_file(),'Set INFOSCREEN_DASHBOARD_PARSER and supply ucode for the separate dashboard comparison')
    def test_dashboard(self):self.check_cases(True)
    def test_no_operator(self):
        for reply in ['','ERROR','+COPS: 0\nOK']:
            self.assertIsNone(self.sims.operator_name(reply))
            if shutil.which('ucode') and PARSER and PARSER.is_file():
                self.assertIsNone(json.loads(subprocess.check_output(['ucode',str(PARSER),reply],text=True)))

if __name__=='__main__':unittest.main()
