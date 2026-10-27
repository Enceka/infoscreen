#!/usr/bin/env python3
"""Install committed infoscreen assets into an offline root without patching sources."""
import argparse
from pathlib import Path
import shutil
import runpy

HERE = Path(__file__).resolve().parents[1]


def sources():
    base = HERE / 'root/usr/share/e5-infoscreen'
    return {p.relative_to(base).as_posix(): p.read_text(encoding='utf-8') for p in base.rglob('*')
            if p.is_file() and p.suffix in ('.js', '.css', '.uc', '.html')}


def stage(root):
    root = Path(root).resolve()
    source = HERE / 'root'
    if root == Path('/') or root == HERE or root in HERE.parents or HERE in root.parents:
        raise ValueError('Use a separate offline root')
    runpy.run_path(str(HERE/'tools/build-native.py'))['verify']()
    for item in source.rglob('*'):
        rel = item.relative_to(source)
        if any(p.startswith('.') or p == '__pycache__' for p in rel.parts):
            continue
        target = root / rel
        if item.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_symlink():
            target.unlink()
        shutil.copy2(item, target)
    for name, service in [('S96t551m-sim-status', 't551m-sim-status'),
                          ('K10t551m-sim-status', 't551m-sim-status'),
                          ('S94t551m-screen-power', 't551m-screen-power'),
                          ('K11t551m-screen-power', 't551m-screen-power'),
                          ('S98e5-voice-audio', 'e5-voice-audio'),
                          ('K09e5-voice-audio', 'e5-voice-audio')]:
        path = root / 'etc/rc.d' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.is_symlink():
            path.symlink_to('../init.d/' + service)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    stage(parser.parse_args().root)
