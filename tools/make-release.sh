#!/bin/sh
# A release for the screen's online update (/usr/libexec/e5-infoscreen/update, docs/API.md 6):
#
#   tools/make-release.sh ["说明" "notes"]   -> dist/e5-infoscreen-<VERSION>.tar.gz and dist/latest.json
#
# The version and the release notes come from .version at the repository root
# (its `version`, `notes_zh` and `notes_en` fields); the arguments override the
# notes.  Raising .version is what the release workflow watches
# (.github/workflows/release.yml), which builds this and opens the GitHub
# release v<VERSION> of Enceka/e5-infoscreen, marked latest. The Pages workflow
# copies the package and rewrites the feed URL for the device's default endpoint;
# this release feed remains available for older installations. Without .version the version is
# root/usr/share/e5-infoscreen/VERSION, raised by hand first.  Either way the
# package is root/ as it is in git, owned by root.
set -eu
cd "$(dirname "$0")/.."
python3 tools/build-native.py --check
field() { [ -f .version ] && sed -n "s/^$1=//p" .version | head -1; }
V=$(field version); V=${V:-$(cat root/usr/share/e5-infoscreen/VERSION)}
printf '%s\n' "$V" > root/usr/share/e5-infoscreen/VERSION
[ -z "$(git status --porcelain root | grep -v 'root/usr/share/e5-infoscreen/VERSION')" ] ||
    { echo "root/ has uncommitted changes" >&2; exit 1; }
mkdir -p dist
P=dist/e5-infoscreen-$V.tar.gz
python3 - "$P" <<'PY'
import os, subprocess, sys, tarfile, time
out = sys.argv[1]
files = subprocess.run(['git', 'ls-files', 'root'], capture_output=True, text=True, check=True).stdout.split()
mtime = int(subprocess.run(['git', 'log', '-1', '--format=%ct'], capture_output=True, text=True).stdout or time.time())
with tarfile.open(out, 'w:gz', format=tarfile.GNU_FORMAT) as t:
    dirs = set()
    for f in sorted(files):
        rel = os.path.relpath(f, 'root')
        parts = rel.split('/')
        for i in range(1, len(parts)):
            d = '/'.join(parts[:i])
            if d not in dirs:
                dirs.add(d)
                ti = tarfile.TarInfo('./' + d); ti.type = tarfile.DIRTYPE; ti.mode = 0o755; ti.mtime = mtime
                t.addfile(ti)
        ti = t.gettarinfo(f, './' + rel)
        ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'; ti.mtime = mtime
        ti.mode = 0o755 if os.access(f, os.X_OK) else 0o644
        with open(f, 'rb') as fh:
            t.addfile(ti, fh)
PY
SUM=$( (shasum -a 256 "$P" 2>/dev/null || sha256sum "$P") | cut -d' ' -f1)
SIZE=$(wc -c < "$P" | tr -d ' ')
ZH=${1:-$(field notes_zh)}
EN=${2:-$(field notes_en)}
python3 - "$V" "$SUM" "$SIZE" "$ZH" "$EN" > dist/latest.json <<'PY'
import json, sys
v, s, n, zh, en = sys.argv[1:6]
print(json.dumps({'version': v,
                  'url': f'https://github.com/Enceka/infoscreen/releases/download/v{v}/e5-infoscreen-{v}.tar.gz',
                  'sha256': s, 'size': int(n), 'notes': {'zh': zh, 'en': en or zh}}, ensure_ascii=False, indent=1))
PY
echo "$P ($SIZE bytes), dist/latest.json: upload both to the release v$V"
