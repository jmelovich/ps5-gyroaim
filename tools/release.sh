#!/usr/bin/env bash
# GyroAim - cut a release: bump the versions, commit, tag.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/release.sh 0.6.0
#
# The order matters. The homebrew catalog reads contentVersion from the
# committed app/sce_sys/param.json *at the release tag*, and a console compares
# that value with the copy it has installed. So the bump has to be in the commit
# the tag points at, or the listing shows a release that no console ever offers
# as an update. This does the steps in that order and leaves the push to you.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

if [[ $# -ne 1 ]]; then
    echo "usage: tools/release.sh <version>      # e.g. tools/release.sh 0.6.0" >&2
    exit 2
fi

version=${1#v}
tag="v$version"

# A tag has to point at something reproducible, so refuse a dirty or detached
# tree rather than tagging a state nobody can get back to.
branch=$(git rev-parse --abbrev-ref HEAD)
[[ $branch != HEAD ]] || { echo "detached HEAD; check out a branch first" >&2; exit 2; }
if [[ -n $(git status --porcelain) ]]; then
    echo "working tree is not clean; commit or set aside your changes first" >&2
    git status --short >&2
    exit 2
fi
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    echo "tag $tag already exists" >&2
    exit 2
fi

# Prove it builds and the tests pass before putting a tag on it.
echo "==> checking the build and the tests"
make --no-print-directory release >/dev/null

# Validates the version and rejects anything NN.NNN.NNN cannot hold.
echo "==> stamping the version"
tools/set-content-version.sh "$version"
content_version=$(python3 -c \
    'import json;print(json.load(open("app/sce_sys/param.json"))["contentVersion"])')

if git diff --quiet -- app/sce_sys/param.json payload/version.h; then
    echo "the versions are already $version ($content_version); nothing to commit." >&2
    echo "Raise the version, or delete the tag and re-cut if you are redoing a release." >&2
    exit 2
fi

git add app/sce_sys/param.json payload/version.h
git commit -q -m "release: $tag (contentVersion $content_version)"
git tag -a "$tag" -m "$tag"

cat <<MESSAGE

Tagged $tag at $(git rev-parse --short HEAD), contentVersion $content_version.

Push both. The tag on its own would point at a commit GitHub does not have:

    git push origin $branch $tag

The build workflow then assembles the title and opens a draft release with the
archive attached. Check the notes and press Publish.
MESSAGE
