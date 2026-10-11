#!/usr/bin/env bash
# GyroAim - check the built title against the homebrew catalog's rules.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The catalog's own checks run when the pull request is opened, by which point
# a release is already published and a mistake means cutting another one. These
# are the same rules, applied to what was just built.

set -uo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

title=PPSA99510
zip=app/dist/$title.zip
folder=app/dist/$title
param=app/sce_sys/param.json
icon=app/sce_sys/icon0.png
problems=0

ok()   { printf '  ok   %s\n' "$1"; }
bad()  { printf '  BAD  %s\n' "$1"; problems=$((problems + 1)); }

echo "checking $title for submission"

# --- the archive ---------------------------------------------------------
if [[ -f $zip ]]; then
    ok "the archive exists"
    top=$(python3 -c "
import sys, zipfile
names = zipfile.ZipFile('$zip').namelist()
roots = {n.split('/')[0] for n in names if n.strip('/')}
print(','.join(sorted(roots)))
")
    [[ $top == "$title" ]] && ok "the app folder is at the top of the archive" \
        || bad "the archive's top level is '$top', not '$title'"
    for want in eboot.bin sce_sys/param.json sce_sys/icon0.png; do
        python3 -c "
import sys, zipfile
sys.exit(0 if '$title/$want' in zipfile.ZipFile('$zip').namelist() else 1)" \
            && ok "the archive has $want" || bad "the archive is missing $want"
    done
else
    bad "no archive at $zip; run make release"
fi

# --- identity -----------------------------------------------------------
if [[ -f $param ]]; then
    read -r id content < <(python3 -c "
import json
p = json.load(open('$param'))
print(p.get('titleId', ''), p.get('contentVersion', ''))
")
    [[ $id == "$title" ]] && ok "titleId is $id" || bad "titleId is '$id', not $title"
    [[ $id != PPSA99999 ]] && ok "not the boilerplate's default title id" \
        || bad "PPSA99999 is the boilerplate default and is rejected"
    [[ $content =~ ^[0-9]{2}\.[0-9]{3}\.[0-9]{3}$ ]] \
        && ok "contentVersion $content is in NN.NNN.NNN form" \
        || bad "contentVersion '$content' is not NN.NNN.NNN"
    [[ $(python3 -c "
import json; print(json.load(open('$param')).get('requiredSystemSoftwareVersion'))") \
        == "0x0000000000000000" ]] \
        && ok "no firmware floor, so any console can launch it" \
        || bad "requiredSystemSoftwareVersion should be zeroed"
else
    bad "no $param"
fi

# --- the icon -----------------------------------------------------------
if [[ -f $icon ]]; then
    read -r w h fmt < <(python3 -c "
from PIL import Image
i = Image.open('$icon')
print(i.width, i.height, i.format)
" 2>/dev/null || echo "0 0 none")
    [[ $fmt == PNG ]] && ok "the icon is a PNG" || bad "the icon is '$fmt', not PNG"
    [[ $w == "$h" ]] && ok "the icon is square" || bad "the icon is ${w}x${h}, not square"
    [[ $w -ge 256 ]] && ok "the icon is ${w}x${h}, at least 256" \
        || bad "the icon is ${w}x${h}, under the 256 minimum"
    size=$(stat -c%s "$icon")
    [[ $size -le 2097152 ]] && ok "the icon is $size bytes, under 2 MiB" \
        || bad "the icon is $size bytes, over the 2 MiB limit"
else
    bad "no $icon"
fi

# --- licence and notices -------------------------------------------------
[[ -f LICENSE ]] && ok "LICENSE is present for GitHub to detect" || bad "no LICENSE"
[[ -f THIRD_PARTY_NOTICES.md ]] && ok "third-party notices are present" \
    || bad "no THIRD_PARTY_NOTICES.md"

# --- nothing borrowed left in the package -------------------------------
if [[ -d $folder ]]; then
    stray=$(find "$folder" -type f \
        \( -name 'pic*.dds' -o -name 'snd0.at9' \) | wc -l)
    [[ $stray -eq 0 ]] \
        && ok "no borrowed presentation assets in the package" \
        || bad "$stray borrowed presentation asset(s) still in the package"
    echo "  --   package is $(du -sh "$folder" | cut -f1), archive $(du -h "$zip" | cut -f1)"
fi

echo
if [[ $problems -eq 0 ]]; then
    echo "ready to submit"
else
    echo "$problems problem(s) to fix"
fi
exit $((problems > 0))
