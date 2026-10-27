"""Production C SIM parsing; synthetic AT responses, no modem access."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from native_support import Simulator

class SimsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.sim=Simulator()
    @classmethod
    def tearDownClass(cls):cls.sim.close()
    def test_lte_and_nr_are_independent(self):
        a,b=self.sim.collect(),self.sim.collect(nr=True)
        self.assertEqual((a['tech'],a['signal']['rsrp']),('lte',-91))
        self.assertEqual((b['tech'],b['signal']['rsrp']),('5gnr',-104))
        self.assertEqual((a['registration'],b['registration']),('camped','camped'))
        self.assertIsNone(b['reg5g'])
        self.assertEqual((a['signal']['snr'],b['signal']['snr']),(4,10.5))
        self.assertIn('+SPENGMD=0,0,6',a['queries']);self.assertNotIn('+SPENGMD=0,0,6',b['queries'])
    def test_lte_phy_values(self):
        for raw,expected in [('400',4),('-350',-3.5),('+350',-3.5),('0',0),
                             ('-2000',-20),('4000',40),('4001',None),('-2001',None),('NA',None),('bad',None)]:
            with self.subTest(raw=raw):
                reply='255-255-'+raw+'-0,1-0,0-4-0\nOK'
                self.assertEqual(self.sim.collect({'+SPENGMD=0,0,6':reply})['signal']['snr'],expected)
    def test_lte_phy_failure_keeps_other_readings(self):
        row=self.sim.collect({'+SPENGMD=0,0,6':None})
        self.assertEqual((row['tech'],row['signal']['rsrp'],row['registration']),('lte',-91,'camped'))
        self.assertIsNone(row['signal']['snr'])
    def test_no_lte_phy_without_lte_measurement(self):
        row=self.sim.collect({'+CESQ':'+CESQ: 99,99,255,255,255,255,255,255,255\nOK'})
        self.assertNotIn('+SPENGMD=0,0,6',row['queries']);self.assertIsNone(row['signal']['rsrp'])
    def test_registered_and_roaming(self):
        self.assertEqual(self.sim.collect({'+CEREG?':'+CEREG: 2,5,"1234","5678",7\nOK'})['registration'],'roaming')
        self.assertEqual(self.sim.collect({'+C5GREG?':'+C5GREG: 2,1,"1234","5678",11\nOK'},nr=True)['registration'],'home')
    def test_ucs2_operator_keeps_registration_and_signal(self):
        row=self.sim.collect({'+CSCS?;+COPS?':'+CSCS: "UCS2"\n+COPS: 0,0,"4E2D56FD8054901A",7\nOK'})
        self.assertEqual(row['operator'],'中国联通');self.assertEqual(row['signal']['rsrp'],-91)
        self.assertFalse(any('CSCS=' in cmd for cmd in row['queries']))
    def test_absent_locked_off(self):
        for reply,state in [('+CME ERROR: 10','absent'),('+CPIN: SIM PIN\nOK','locked')]:
            row=self.sim.collect({'+CPIN?':reply});self.assertEqual(row['registration'],state)
            self.assertEqual(row['queries'],['+CPIN?'])
        self.assertEqual(self.sim.collect({'+CFUN?':'+CFUN: 4\nOK'})['registration'],'disabled')
        self.assertEqual(self.sim.collect({'+CPIN?':'+CME ERROR: 100'})['registration'],'unknown')
    def test_failure_does_not_reuse_signal(self):
        row=self.sim.collect({'+CPIN?':None})
        self.assertEqual(row['signal'],{});self.assertIsNone(row['tech']);self.assertEqual(row['registration'],'unknown')
if __name__=='__main__':unittest.main()
