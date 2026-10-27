#!/usr/bin/env python3
"""Build/check the ARM64 screen helpers with an explicitly supplied OpenWrt SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

TOP=Path(__file__).resolve().parents[1]
SOURCE=TOP/'src/native'
OUT=TOP/'root/usr/libexec'
METADATA=TOP/'root/usr/share/e5-infoscreen/native-build.json'
TARGETS={
 't551m-screen-power':['screen-power.c'],
 't551m-sim-status':['sim-status.c','native-common.c'],
 'e5-voice-native':['voice.c','voice-hardware.c','screen-http.c','native-common.c'],
}

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def inputs(names):
    deps=names+([] if names==['screen-power.c'] else ['native.h'])
    return {n:digest(SOURCE/n) for n in deps}

def verify():
    meta=json.loads(METADATA.read_text())
    if meta.get('builder')!=digest(Path(__file__)):raise ValueError('Native build instructions changed; rebuild the helpers')
    for name,names in TARGETS.items():
        record=meta['targets'][name];p=OUT/name;data=p.read_bytes()
        if record['sources']!=inputs(names) or record['sha256']!=digest(p):
            raise ValueError('Native source/binary mismatch: '+name)
        if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=183:
            raise ValueError('Expected AArch64 ELF: '+name)
    print('PASS: all three ARM64 native helpers match their source and build records')

def build(volume,toolchain,image):
    if not volume:raise ValueError('Supply --sdk-volume; an existing SDK is required')
    compiler='/build/openwrt/staging_dir/'+toolchain+'/bin/aarch64-openwrt-linux-musl-gcc'
    sysroot='/build/openwrt/staging_dir/target-aarch64_generic_musl'
    meta={'format':1,'builder':digest(Path(__file__)),
          'sdk':{'volume':volume,'toolchain':toolchain,'image':image},'targets':{}}
    for name,names in TARGETS.items():
        args=[compiler,'-std=gnu11','-pthread','-Os','-g0','-Wall','-Wextra','-Werror',
              '-fstack-protector-strong','-ffunction-sections','-fdata-sections',
              '-Wl,--gc-sections','-Wl,-z,relro,-z,now','-s']
        if name=='t551m-screen-power':args+=['-static']
        else:args+=['-I'+sysroot+'/usr/include/glib-2.0','-I'+sysroot+'/usr/lib/glib-2.0/include',
                    '-I'+sysroot+'/usr/include','-L'+sysroot+'/usr/lib','-Wl,-rpath-link,'+sysroot+'/usr/lib']
        args+=['/source/'+n for n in names]+['-o','/out/'+name+'.tmp']
        if name!='t551m-screen-power':args+=['-lgio-2.0','-lgobject-2.0','-lglib-2.0','-ljson-c','-ldl','-lm']
        subprocess.run(['docker','run','--rm','--pull=never','--network','none',
                        '-e','STAGING_DIR='+sysroot,'-v',volume+':/build:ro',
                        '-v',str(SOURCE)+':/source:ro','-v',str(OUT)+':/out',image,*args],check=True)
        temp=OUT/(name+'.tmp');data=temp.read_bytes()
        if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=183:
            raise ValueError('Compiler output is not ARM64: '+name)
        temp.chmod(0o755);temp.replace(OUT/name)
        meta['targets'][name]={'sources':inputs(names),'sha256':digest(OUT/name)}
    METADATA.write_text(json.dumps(meta,indent=2)+'\n')
    verify()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--check',action='store_true')
    p.add_argument('--sdk-volume')
    p.add_argument('--toolchain',default='toolchain-aarch64_generic_gcc-14.3.0_musl')
    p.add_argument('--image',default='debian:trixie')
    a=p.parse_args()
    if a.check:verify()
    else:build(a.sdk_volume,a.toolchain,a.image)
