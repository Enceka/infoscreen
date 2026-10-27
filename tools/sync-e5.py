#!/usr/bin/env python3
"""Import the shared screen component from an explicitly selected E5 checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import tempfile

TOP=Path(__file__).resolve().parents[1]
NATIVE=('screen-power.c','sim-status.c','native-common.c','native.h',
        'voice.c','voice-hardware.c','screen-http.c')
PROGRAMS=('t551m-screen-power','t551m-sim-status','e5-voice-native')
PREFIXES=('usr/share/e5-infoscreen/', 'usr/libexec/e5-infoscreen/',
          'www/luci-static/resources/view/e5-infoscreen/')
EXTRA={
 'etc/init.d/e5-infoscreen','etc/init.d/t551m-screen-power','etc/init.d/t551m-sim-status',
 'usr/libexec/t551m-screen-details','usr/libexec/rpcd/t551m-screen',
 'usr/share/luci/menu.d/luci-app-t551m-screen.json',
 'usr/share/rpcd/acl.d/luci-app-t551m-screen.json',
 'usr/share/e5/voice-routes.json',
 'usr/libexec/e5-voice-audio','etc/init.d/e5-voice-audio',
 *('usr/libexec/'+n for n in PROGRAMS),
}

def sha(data):return hashlib.sha256(data).hexdigest()

def asset(data):
    if b'\0' in data:return data
    try:data.decode('utf-8')
    except UnicodeDecodeError:return data
    return data.replace(b'\r\n',b'\n')

def sync(source):
    source=source.resolve()
    if source==TOP:raise ValueError('Select the E5 firmware repository')
    shared=source/'devices/common/infoscreen'
    if not (shared/'stage.py').is_file():raise ValueError('Shared screen source not found')
    previous=TOP/'docs/E5-SOURCE.json'
    if previous.exists():
        for change in json.loads(previous.read_text(encoding='utf-8')).get('local_adjustments',[]):
            local=TOP/change['path'];upstream=shared/'native'/local.name
            if not upstream.is_file() or asset(local.read_bytes())!=asset(upstream.read_bytes()):
                raise ValueError('Sync the standalone fix back to E5 before reimporting: '+change['path'])
    if subprocess.check_output(['git','-C',str(source),'status','--porcelain']):
        raise ValueError('Commit the E5 source before importing')
    revision=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    outputs={}
    with tempfile.TemporaryDirectory(prefix='infoscreen-e5-') as folder:
        root=Path(folder)
        for directory in ('etc/e5','etc/rc.d','etc/uci-defaults'):(root/directory).mkdir(parents=True,exist_ok=True)
        (root/'etc/e5/device-profile').write_text('e5\n')
        runpy.run_path(str(shared/'stage.py'))['stage'](root)
        for p in sorted(root.rglob('*')):
            if not p.is_file() or p.is_symlink():continue
            relative=p.relative_to(root).as_posix()
            # The standalone updater owns atomic replacement of its running
            # native helpers; do not replace it with the parent's older script.
            if relative=='usr/libexec/e5-infoscreen/update':continue
            if not (relative.startswith(PREFIXES) or relative in EXTRA):continue
            data=asset(p.read_bytes());target=TOP/'root'/relative
            target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
            target.chmod(p.stat().st_mode&0o777)
            outputs[relative]=sha(data)
    # One built-in Phone copy, retaining its original attribution. The parent
    # firmware handles installed /etc plugins and application data itself.
    phone=TOP/'root/usr/share/e5-infoscreen/www/plugins/phone'
    for p in sorted((shared/'phone').iterdir()):
        if p.is_file():
            target=phone/p.name;target.write_bytes(p.read_bytes().replace(b'\r\n',b'\n'))
            outputs[target.relative_to(TOP/'root').as_posix()]=sha(target.read_bytes())
    # This branch bundles APN code in app.js, as in E5; do not ship a second copy.
    (TOP/'root/usr/share/e5-infoscreen/www/apn.js').unlink(missing_ok=True)
    native=TOP/'src/native';native.mkdir(parents=True,exist_ok=True)
    for name in NATIVE:(native/name).write_bytes(asset((shared/'native'/name).read_bytes()))
    for relative in ('usr/libexec/e5-voice-audio','etc/init.d/e5-voice-audio'):
        target=TOP/'root'/relative
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes((source/'openwrt/overlay'/relative).read_bytes().replace(b'\r\n',b'\n'))
        target.chmod(0o755);outputs[relative]=sha(target.read_bytes())
    source_info={'format':1,'repository':'https://github.com/sprd-linux-devs/e5-linux',
                 'revision':revision,'source':'devices/common/infoscreen',
                 'upstream_base':'7e031de0c7bb952e490c81bb038733f06e8bf8f6',
                 'files':outputs,'native_sources':{n:sha((native/n).read_bytes()) for n in NATIVE}}
    dest=TOP/'docs/E5-SOURCE.json'
    dest.write_text(json.dumps(source_info,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'Imported {len(outputs)} screen files and {len(NATIVE)} native sources from {revision[:12]}')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    sync(parser.parse_args().source)
