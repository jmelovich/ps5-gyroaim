#!/usr/bin/env bash
# GyroAim - print the release notes for a tag.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/release-notes.sh v1.0.2 [owner/repo]
#
# Two parts. What is worth saying about a particular release is written by
# hand and kept in docs/releases/<tag>.md, so it survives a tag being re-cut
# and can be reviewed like anything else. The install and warning sections are
# the same every time, so they are generated here.
#
# The build workflow calls this, and so can you, which is the point: notes
# written straight into a draft release cannot be checked before they are
# published, and are lost the moment the tag moves.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

[[ $# -ge 1 ]] || { echo "usage: tools/release-notes.sh <tag> [owner/repo]" >&2; exit 2; }
tag=$1
repo=${2:-jmelovich/ps5-gyroaim}
lead="docs/releases/$tag.md"

if [[ -f $lead ]]; then
    cat "$lead"
    printf '\n'
else
    echo "no $lead; publishing without a summary or a changelog" >&2
fi

cat <<'BOILERPLATE'
## Installing

Unpack `PPSA99510.zip` into `/data/homebrew/` on the console, then launch
**GyroAim** from the home screen and choose **start the service**.
The app carries the service inside it, so there is nothing else to install.

## What you need

- A jailbroken PS5 with an ELF loader on port 9021 and kstuff loaded.
- ShadowMount+ to register the title, and new enough to mount `/data`
  if you want the service to start with the console.

## Settings

The panel shows a QR code for the settings page. Open it on a phone on the
same network and tune the aiming while you play.

BOILERPLATE

previous=$(git describe --tags --abbrev=0 "$tag^" 2>/dev/null || true)
if [[ -n $previous ]]; then
    printf '## Every change\n\n'
    printf 'https://github.com/%s/compare/%s...%s\n\n' "$repo" "$previous" "$tag"
fi

printf 'Experimental homebrew. It writes into a running game: save first.\n'
printf 'Not affiliated with Sony. Contains no licensed code.\n'
