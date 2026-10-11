#!/usr/bin/env bash
# Fetches the pinned ps5-payload-dev SDK into the ignored .deps cache.
#
# Set PS5_PAYLOAD_SDK to use an SDK you already have instead; this script then
# only checks that it looks usable.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache="$root/.deps"
sdk="$cache/ps5-payload-sdk"

sdk_version="v0.42"
sdk_url="https://github.com/ps5-payload-dev/sdk/releases/download/$sdk_version/ps5-payload-sdk.zip"
sdk_hash="8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da"

if [[ -n ${PS5_PAYLOAD_SDK:-} ]]; then
    if [[ ! -x "$PS5_PAYLOAD_SDK/bin/prospero-clang" ]]; then
        echo "PS5_PAYLOAD_SDK=$PS5_PAYLOAD_SDK has no bin/prospero-clang" >&2
        exit 2
    fi
    echo "using PS5_PAYLOAD_SDK=$PS5_PAYLOAD_SDK"
    exit 0
fi

if [[ -x "$sdk/bin/prospero-clang" ]]; then
    echo "SDK already present at $sdk"
    exit 0
fi

command -v unzip >/dev/null || { echo "unzip is required" >&2; exit 2; }

mkdir -p "$cache"

# A copy may already be sitting in a sibling checkout; a hardlinked tree costs
# nothing and saves the download.
for sibling in "$root"/../*/.deps/native/ps5-payload-sdk "$root"/../*/deps/PS5_PayloadSDK; do
    if [[ -x "$sibling/bin/prospero-clang" ]]; then
        echo "copying SDK from $sibling"
        cp -al "$sibling" "$sdk" 2>/dev/null || cp -a "$sibling" "$sdk"
        [[ -x "$sdk/bin/prospero-clang" ]] && exit 0
        rm -rf "$sdk"
    fi
done

archive="$cache/ps5-payload-sdk.zip"
temporary="$archive.download"
echo "downloading SDK $sdk_version"
if command -v wget >/dev/null; then
    wget -q "$sdk_url" -O "$temporary"
else
    curl -fsSL "$sdk_url" -o "$temporary"
fi
printf '%s  %s\n' "$sdk_hash" "$temporary" | sha256sum --check --strict
mv "$temporary" "$archive"
unzip -q -o "$archive" -d "$cache"

[[ -x "$sdk/bin/prospero-clang" ]] || { echo "SDK unpacked but incomplete" >&2; exit 1; }
echo "SDK ready at $sdk"
