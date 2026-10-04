#!/bin/sh
# Tests in a disposable OpenWrt userspace; no device or modem is contacted.
set -eu
TOP=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 - "$TOP" "$T" <<'PY'
from pathlib import Path
import shutil
import sys
top, temp = map(Path, sys.argv[1:])
# Fresh inodes avoid stale file lengths on macOS Docker bind mounts after edits.
shutil.copytree(top / 'root/usr/share/e5-infoscreen', temp / 'share')
shutil.copytree(top / 'root/usr/libexec/e5-infoscreen', temp / 'helpers')
shutil.copytree(top / 'tests', temp / 'tests')
source = (top / 'root/usr/share/e5-infoscreen/api.uc').read_text()
source = source[source.index('function wifi_config()'):source.index('\nfunction clients()')]
wrapper = 'return function(ctx) {\nconst cursor=ctx.cursor, ubus_call=ctx.ubus_call, state_get=ctx.state_get, state_put=ctx.state_put, stat=ctx.stat;\n'
(temp / 'wifi-under-test.uc').write_text(wrapper + source + '\nreturn wifi_status;\n};\n')
(temp / 'bin').mkdir()
for name, text in {
    'timeout': '#!/bin/sh\nshift; exec "$@"\n',
    'ubus': '#!/bin/sh\ncat /test/wireless.json\n',
}.items():
    path = temp / 'bin' / name
    path.write_text(text); path.chmod(0o755)
(temp / 'wireless.json').write_text('{"radio0":{"up":false,"errors":[{"code":"NO_RADIO"}],"config":{"key":"must-not-appear","nested":{"password":"must-not-appear"}}}}')
PY
docker run --rm -v "$T/share":/usr/share/e5-infoscreen:ro \
    -v "$T/helpers":/helpers:ro \
    -v "$T/tests":/tests:ro -v "$T":/test \
    "${E5_TEST_IMAGE:-e5-openwrt-base:25.12.5}" \
    sh -ec 'ucode /tests/usb-settings.uc; ucode /tests/wifi-status.uc;
        PATH=/test/bin:$PATH ucode /helpers/net-check.uc > /test/report.json;
        ! grep -q must-not-appear /test/report.json;
        grep -q NO_RADIO /test/report.json;
        echo "Network diagnostic redaction checks passed"'
