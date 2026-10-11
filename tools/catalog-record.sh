#!/usr/bin/env bash
# GyroAim - print the homebrew catalog record for a published release.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/catalog-record.sh v1.0.0
#
# The record is apps/<TITLEID>.json in the ps5-homebrew-catalog repository.
# Writing it by hand means copying a digest and three URLs without a typo, so
# this reads them from the release instead. The release has to be published:
# the catalog cannot download a draft.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

if [[ $# -ne 1 ]]; then
    echo "usage: tools/catalog-record.sh <tag>      # e.g. v1.0.0" >&2
    exit 2
fi
tag=$1

command -v gh >/dev/null || { echo "the GitHub CLI is required" >&2; exit 2; }

title=$(python3 -c \
    'import json;print(json.load(open("app/sce_sys/param.json"))["titleId"])')
repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)

state=$(gh release view "$tag" --json isDraft,assets 2>/dev/null) || {
    echo "no release $tag in $repo" >&2
    exit 2
}

draft=$(printf '%s' "$state" | python3 -c 'import json,sys;print(json.load(sys.stdin)["isDraft"])')
digest=$(printf '%s' "$state" | python3 -c "
import json, sys
assets = json.load(sys.stdin)['assets']
for a in assets:
    if a['name'] == '$title.zip':
        print((a.get('digest') or '').removeprefix('sha256:'))
        break
else:
    print('')
")

if [[ -z $digest ]]; then
    echo "release $tag has no $title.zip with a digest yet" >&2
    exit 2
fi

cat <<JSON
{
  "titleid": "$title",
  "name": "GyroAim",
  "kind": "app",
  "description": "Gyro aiming in any game: the controller's gyroscope drives the stick, tuned from your phone while you play.",
  "license": "GPL-3.0",
  "author": "jmelovich",
  "version": "${tag#v}",
  "source_repo": "https://github.com/$repo",
  "artifact_url": "https://github.com/$repo/releases/download/$tag/$title.zip",
  "sha256": "$digest",
  "icon_url": "https://raw.githubusercontent.com/$repo/$tag/app/sce_sys/icon0.png"
}
JSON

echo >&2
if [[ $draft == True ]]; then
    echo "NOTE: $tag is still a draft. Publish it before opening the pull" >&2
    echo "request; the catalog cannot download a draft's asset." >&2
fi
echo "The repository must be public for source_repo and icon_url to resolve." >&2
