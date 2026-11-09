#!/bin/sh
# Build sloperinaGZ and pack a release: release/sloperinaGZ-<version>.zip, holding
#   install.bat, install.ps1, install.sh       the installers (see INSTALLATION.md)
#   files/subsdk9, files/<build id>.ips          the module and both game-version patches
#   files/lang/template.json                     the translation template
#   files/atmosphere/, files/exefs/              the same files laid out for copying by hand
#   README.md, INSTALLATION.md, USAGE.md, MANUAL.md, CONTRIBUTORS.md, THANKS.md, LICENSE (if there is one)
set -e
cd "$(dirname "$0")/.."
./docker-make dist
VERSION=$(cat VERSION)
OUT=release/sloperinaGZ-$VERSION
rm -rf "$OUT" "$OUT.zip"
mkdir -p "$OUT/files/lang"
cp dist/exefs/* "$OUT/files/"
cp lang/template.json "$OUT/files/lang/"
cp -r dist/atmosphere "$OUT/files/"
mkdir -p "$OUT/files/exefs" && cp dist/exefs/* "$OUT/files/exefs/"
cp scripts/install.bat scripts/install.ps1 scripts/install.sh "$OUT/"
cp README.md INSTALLATION.md CONTRIBUTORS.md THANKS.md docs/USAGE.md docs/MANUAL.md "$OUT/"
[ -f LICENSE ] && cp LICENSE "$OUT/"
# links in the copied documents point next to them in the release
sed -i 's#](docs/USAGE.md)#](USAGE.md)#g; s#](docs/MANUAL.md)#](MANUAL.md)#g; s#](\.\./INSTALLATION.md#](INSTALLATION.md#g' \
  "$OUT"/*.md
python3 - "$OUT" <<'PY'
import os, sys, zipfile
out = sys.argv[1]
base = os.path.dirname(out)
with zipfile.ZipFile(out + ".zip", "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(out):
        for f in sorted(files):
            p = os.path.join(root, f)
            info = zipfile.ZipInfo.from_file(p, os.path.relpath(p, base))
            if f.endswith(".sh"):
                info.external_attr = 0o100755 << 16  # a regular file, executable when unzipped
            with open(p, "rb") as fh:
                z.writestr(info, fh.read(), zipfile.ZIP_DEFLATED)
print(out + ".zip")
PY
