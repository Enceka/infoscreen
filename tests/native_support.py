"""Host build of the real SIM collector with synthetic AT replies only."""
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile
TOP=Path(__file__).resolve().parents[1]
NATIVE=TOP/'src/native'

class Simulator:
    def __init__(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='infoscreen-native-test-')
        self.root=Path(self.tmp.name);self.binary=self.root/'sim-test'
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0'],text=True))
        if subprocess.run(['pkg-config','--exists','json-c']).returncode==0:
            flags+=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','json-c'],text=True))
        else:
            volume=os.environ.get('INFOSCREEN_TEST_SDK_VOLUME')
            if not volume:raise RuntimeError('Install json-c development headers or set INFOSCREEN_TEST_SDK_VOLUME')
            wire=subprocess.check_output(['docker','run','--rm','--pull=never','--network','none',
                '-v',volume+':/build:ro','debian:trixie','tar',
                '-C','/build/openwrt/staging_dir/target-aarch64_generic_musl/usr/include','-c','json-c'])
            with tarfile.open(fileobj=io.BytesIO(wire)) as archive:
                for member in archive.getmembers():
                    if member.isfile():
                        p=self.root/member.name
                        if p.parent!=self.root/'json-c':raise ValueError('Unexpected SDK header path')
                        p.parent.mkdir(exist_ok=True);p.write_bytes(archive.extractfile(member).read())
            flags+=['-I'+str(self.root),'-l:libjson-c.so.5']
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(NATIVE),
            str(NATIVE/'test-sim.c'),str(NATIVE/'native-common.c'),'-o',str(self.binary),*flags,'-lm'],check=True)
    def close(self):self.tmp.cleanup()
    def collect(self,override=None,nr=False):
        replies={'+CPIN?':'+CPIN: READY\nOK\n','+CFUN?':'+CFUN: 1\nOK\n',
            '+CSCS?;+COPS?':'+CSCS: "GSM"\n+COPS: 0,0,"CHN-UNICOM",7\nOK',
            '+CEREG?':'OK\n','+C5GREG?':'OK\n',
            '+CESQ':'+CESQ: 99,99,255,255,14,50,255,255,255\nOK',
            '+SPENGMD=0,0,6':'255-255-400-0,1-0,0-4-0-3-600\nOK'}
        if nr:replies.update({'+CSCS?;+COPS?':'+CSCS: "UCS2"\n+COPS: 0,2,"46011",11\nOK',
                            '+CESQ':'+CESQ: 99,99,255,255,255,255,74,53,67\nOK'})
        replies.update(override or {})
        return json.loads(subprocess.check_output([str(self.binary)],input=json.dumps(replies)+'\n',text=True))
    def operator_name(self,reply):return self.collect({'+CSCS?;+COPS?':reply})['operator']
