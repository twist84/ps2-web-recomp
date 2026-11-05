#!/usr/bin/env bash
set -euo pipefail

if [ "${MSYSTEM:-}" != CLANG64 ]; then
    echo 'Open MSYS2 CLANG64 and run this script there.' >&2
    exit 1
fi

root=$(cd "$(dirname "$0")/.." && pwd)
game=${1:-SuperDragonBallZ}
if [[ ! "$game" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo 'SDBZ name must contain only letters, digits, underscores or hyphens.' >&2
    exit 1
fi
for tool in cmake ninja clang++ python3 curl; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
if [ ! -d "$root/generated/$game" ]; then
    echo "Missing generated/$game. Generate the game with port.sh first." >&2
    exit 1
fi

wgpu_dir="$root/third_party/wgpu-native"
if [ ! -f "$wgpu_dir/lib/libwgpu_native.dll.a" ] || [ ! -f "$wgpu_dir/lib/wgpu_native.dll" ] || [ ! -f "$wgpu_dir/include/webgpu/webgpu.h" ]; then
    version=v29.0.1.1
    archive_dir=$(mktemp -d)
    trap 'rm -rf "$archive_dir"' EXIT
    echo "Downloading wgpu-native $version for Windows x86-64 GNU..."
    curl --fail --location --retry 3 \
        "https://github.com/gfx-rs/wgpu-native/releases/download/$version/wgpu-windows-x86_64-gnu-release.zip" \
        --output "$archive_dir/wgpu.zip"
    mkdir -p "$wgpu_dir"
    python3 -I - "$archive_dir/wgpu.zip" "$wgpu_dir" <<'PY'
import sys
import zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    archive.extractall(sys.argv[2])
PY
fi

for required in include/webgpu/webgpu.h include/webgpu/wgpu.h lib/libwgpu_native.dll.a lib/wgpu_native.dll; do
    if [ ! -f "$wgpu_dir/$required" ]; then
        echo "Missing $wgpu_dir/$required. Check the GNU release archive layout." >&2
        exit 1
    fi
done

export CXX="$(cygpath -am /clang64/bin/clang++.exe)"
cmake -S "$root" -B "$root/build/native-$game" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_CXX_COMPILER=$CXX" \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL2=FALSE \
    "-DSDL2_DIR=$(cygpath -am /clang64/lib/cmake/SDL2)" \
    "-DPS2_GENERATED=$(cygpath -am "$root/generated/$game")" \
    "-DPS2_WGPU_DIR=$(cygpath -am "$wgpu_dir")"
cmake --build "$root/build/native-$game" --target ps2run
echo "Built build/native-$game/ps2run.exe and copied wgpu_native.dll beside it."
