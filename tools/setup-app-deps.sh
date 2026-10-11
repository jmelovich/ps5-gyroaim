#!/usr/bin/env bash
# Fetches the pinned native-app boilerplate used to build the control panel.
#
# The boilerplate is the title build pipeline: the cross toolchain wrapper, the
# ELF to fake-signed-executable converter, the clean-room libc module, and the
# packaging. It expects to be the project root, and its build script refuses
# absolute paths, so our app directory is linked into its tree rather than
# copied.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache="$root/.deps"
bp="$cache/ps5-native-app-boilerplate"

url="https://github.com/blackbearreloaded/ps5-native-app-boilerplate"
commit="7928889d33261833bf87d23269444d830d68ff48"

command -v git >/dev/null || { echo "git is required" >&2; exit 2; }
mkdir -p "$cache"

if [[ ! -d "$bp/.git" ]]; then
    echo "fetching the native-app boilerplate"
    git clone -q "$url" "$bp"
fi

if [[ $(git -C "$bp" rev-parse HEAD) != "$commit" ]]; then
    git -C "$bp" fetch -q origin "$commit" 2>/dev/null || git -C "$bp" fetch -q origin
    git -C "$bp" checkout -q "$commit"
fi
echo "boilerplate at $commit"

# One link, so the build script sees a relative path it will accept.
ln -sfn "$root/app" "$bp/gyroaim"

# The app reuses the boilerplate's presentation assets, which carry no
# identity of their own, and overrides only the icon and param.json.
for f in pic0.dds pic1.dds snd0.at9 background-source.png launch-background-source.png; do
    [[ -f "$bp/sce_sys/$f" && ! -f "$root/app/sce_sys/$f" ]] && \
        cp "$bp/sce_sys/$f" "$root/app/sce_sys/$f"
done
# The renderer is copied rather than linked: the build script finds sources
# with `find -type f`, which skips symlinks.
mkdir -p "$root/app/src/vendor"
for f in demo_renderer.cpp demo_renderer.hpp; do
    cp "$bp/src/$f" "$root/app/src/vendor/$f"
done

echo "app assets and vendored renderer ready"
