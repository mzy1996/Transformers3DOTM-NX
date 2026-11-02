#!/usr/bin/env bash
# T3DOTM - repackage the Switch port delivery zip (in place).
#
# Produces T3DOTM-switch-port.zip at the project root, mirroring the original
# packaging layout:
#   T3DOTM/README.md  icon_T3.png  patched/  update/
#   T3DOTM/recomp/    (out/sw8 only T3DOTM.nro)
#   T3DOTM/repos/     (mesa-switch, nfsmw-nx; no .git dirs)
#   T3DOTM/scripts/   tools/
#
# Excluded: game/ (7 GB data), extracted/, .dumate/, docs/, .git dirs,
# __pycache__, and recomp/out build intermediates other than the NRO.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TMP="T3DOTM-switch-port.zip.tmp"
OUT="T3DOTM-switch-port.zip"

rm -f "$TMP"

python - "$ROOT" "$TMP" "$OUT" <<'PY'
import os, sys, zipfile

root, out, final_out = sys.argv[1], sys.argv[2], sys.argv[3]

def prune(name):
    return (name == '.git' or name == '__pycache__')

EXCLUDE_DIRS = {'game', 'extracted', '.dumate', 'docs'}

def walk():
    # yield (arcdir, abspath, is_dir)
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        parts = [] if rel == '.' else rel.split(os.sep)
        if parts and parts[0] in EXCLUDE_DIRS:
            dirnames[:] = []
            continue
        dirnames[:] = [d for d in dirnames if not prune(d)]
        if len(parts) >= 1 and parts[0] == 'recomp' and len(parts) >= 3 and parts[1] == 'out' and parts[2] == 'sw8':
            # only ship the NRO from out/sw8
            filenames = [f for f in filenames if f == 'T3DOTM.nro']
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            if prune(fn) or fn.endswith('.tmp'):
                continue
            ab = os.path.abspath(full)
            if ab == os.path.abspath(os.path.join(root, out)) or ab == os.path.abspath(os.path.join(root, final_out)):
                continue
            if os.path.isfile(full):
                arc = os.path.join('T3DOTM', rel, fn) if rel != '.' else os.path.join('T3DOTM', fn)
                yield arc, full

entries = list(walk())
print(f'packaging {len(entries)} files...', flush=True)

with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
    for i, (arc, full) in enumerate(entries):
        z.write(full, arc)
        if i % 2000 == 0:
            print(f'  {i}/{len(entries)}', flush=True)
print('done', flush=True)
PY

rm -f "$OUT"
mv "$TMP" "$OUT"
echo "==> Done: $OUT"