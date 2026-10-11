#!/usr/bin/env bash
# GyroAim - keep the release tag, the title's contentVersion and the service's
# own version from drifting apart.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Three places carry a version, and they have to agree:
#
#   the release tag             v0.6.0        what the catalog lists
#   app/sce_sys/param.json      00.006.000    how a console spots an update
#   payload/version.h           0.6.0         what the control panel displays
#
# The PS5 wants contentVersion as NN.NNN.NNN, so the tag is mapped onto it:
#
#   v1.2.3        -> 01.002.003
#   v0.6.0        -> 00.006.000
#   v2.10.0-rc1   -> 02.010.000   (a pre-release suffix is ignored)
#
# The homebrew catalog reads contentVersion from the *committed* param.json at
# the release tag, not from the built artifact, so the value has to be in the
# commit the tag points at. tools/release.sh does that in the right order; CI
# then runs this with --check and refuses the release if they disagree.
#
#   tools/release.sh 0.6.0                        bump, commit, tag
#   tools/set-content-version.sh --check v0.6.0   verify, change nothing
#   tools/set-content-version.sh v0.6.0           write the values

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
param=${PARAM_JSON:-$root/app/sce_sys/param.json}
header=${VERSION_H:-$root/payload/version.h}

check_only=false
if [[ ${1:-} == --check ]]; then
    check_only=true
    shift
fi

if [[ $# -ne 1 ]]; then
    echo "usage: tools/set-content-version.sh [--check] <version>   # e.g. v0.6.0" >&2
    exit 2
fi

version=${1#v}                  # tolerate a leading v
version=${version%%[-+]*}       # drop any -rc1 or +build suffix

if [[ ! $version =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    echo "not a MAJOR.MINOR.PATCH version: $1" >&2
    exit 2
fi
major=${BASH_REMATCH[1]}
minor=${BASH_REMATCH[2]}
patch=${BASH_REMATCH[3]}

# NN.NNN.NNN has no room for more, and truncating would ship a version that
# moves backwards, which consoles read as "no update".
if ((major > 99)); then
    echo "major $major exceeds 99, which NN.NNN.NNN cannot hold" >&2
    exit 2
fi
if ((minor > 999 || patch > 999)); then
    echo "minor or patch $minor.$patch exceeds 999, which NN.NNN.NNN cannot hold" >&2
    exit 2
fi

content_version=$(printf '%02d.%03d.%03d' "$major" "$minor" "$patch")

[[ -f $param ]] || { echo "no such file: $param" >&2; exit 2; }
[[ -f $header ]] || { echo "no such file: $header" >&2; exit 2; }

CONTENT_VERSION="$content_version" PLAIN_VERSION="$version" PARAM="$param" \
    HEADER="$header" CHECK_ONLY="$check_only" TAG="$1" python3 - <<'PY'
import os
import re
import sys

param = os.environ["PARAM"]
header = os.environ["HEADER"]
want_content = os.environ["CONTENT_VERSION"]
want_plain = os.environ["PLAIN_VERSION"]
check_only = os.environ["CHECK_ONLY"] == "true"
tag = os.environ["TAG"]

param_text = open(param, encoding="utf-8").read()
header_text = open(header, encoding="utf-8").read()

content_re = re.compile(r'("contentVersion"\s*:\s*")(\d{2}\.\d{3}\.\d{3})(")')
version_re = re.compile(r'(#define\s+GA_VERSION\s+")([0-9]+\.[0-9]+\.[0-9]+)(")')

content_match = content_re.search(param_text)
if not content_match:
    sys.exit(f"{param}: no NN.NNN.NNN contentVersion to read")
version_match = version_re.search(header_text)
if not version_match:
    sys.exit(f"{header}: no GA_VERSION to read")

have_content = content_match.group(2)
have_plain = version_match.group(2)

if check_only:
    problems = []
    if have_content != want_content:
        problems.append(
            f"  {param}\n"
            f"    has contentVersion {have_content}, but tag {tag} means {want_content}"
        )
    if have_plain != want_plain:
        problems.append(
            f"  {header}\n"
            f"    has GA_VERSION {have_plain}, but tag {tag} means {want_plain}"
        )
    if problems:
        sys.exit(
            "the tag and the committed versions disagree:\n"
            + "\n".join(problems)
            + "\n\nThe catalog reads contentVersion from the committed param.json at\n"
            "the release tag, so both have to be bumped before tagging:\n"
            f"    tools/release.sh {tag.lstrip('v')}"
        )
    print(f"contentVersion {have_content} and GA_VERSION {have_plain} match tag {tag}")
    sys.exit(0)

# Rewrite one value in each file, leaving the formatting alone.
new_param, n = content_re.subn(rf"\g<1>{want_content}\g<3>", param_text)
if n != 1:
    sys.exit(f"{param}: expected one contentVersion, found {n}")
new_header, n = version_re.subn(rf"\g<1>{want_plain}\g<3>", header_text)
if n != 1:
    sys.exit(f"{header}: expected one GA_VERSION, found {n}")

open(param, "w", encoding="utf-8").write(new_param)
open(header, "w", encoding="utf-8").write(new_header)
print(f"contentVersion {have_content} -> {want_content}")
print(f"GA_VERSION {have_plain} -> {want_plain}")
PY
