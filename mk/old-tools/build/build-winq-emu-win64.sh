#!/usr/bin/env bash
# Build the paired WINQ-EMU QEMU and virglrenderer in an MSYS2 UCRT64 shell.
# Layout and explicit dependency/configuration steps follow the cross-build
# example in linux-pc98/scripts/build-qemu-win64.sh. The source trees are
# edited and reviewed directly in vendor/; this script never applies patches.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
jobs=${JOBS:-8}
qemu_src="$root/vendor/winq-emu-qemu"
virgl_src="$root/vendor/winq-emu-virglrenderer"
qemu_build="$root/build/winq-emu-qemu-win64"
virgl_build="$root/build/winq-emu-virglrenderer-win64"
dist="$root/build/winq-emu-win64"

[[ ${MSYSTEM:-} == UCRT64 ]] || {
    echo 'Run this script from MSYS2 UCRT64 on Windows.' >&2
    exit 1
}

for tool in git meson ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "Missing $tool" >&2; exit 1; }
done

if [[ ${1:-} == bootstrap ]]; then
    pacman -S --needed --noconfirm \
        git diffutils \
        mingw-w64-ucrt-x86_64-gcc \
        mingw-w64-ucrt-x86_64-meson \
        mingw-w64-ucrt-x86_64-ninja \
        mingw-w64-ucrt-x86_64-libepoxy \
        mingw-w64-ucrt-x86_64-vulkan-headers \
        mingw-w64-ucrt-x86_64-vulkan-loader \
        mingw-w64-ucrt-x86_64-glib2 \
        mingw-w64-ucrt-x86_64-pixman \
        mingw-w64-ucrt-x86_64-SDL2 \
        mingw-w64-ucrt-x86_64-libslirp \
        mingw-w64-ucrt-x86_64-dtc
fi

prepare_source() {
    local path=$1 url=$2 relative=$3
    if [[ ! -e "$path/.git" ]]; then
        git -c core.symlinks=false -C "$root" submodule update --init -- "$relative" || true
    fi
    if [[ ! -e "$path/.git" ]]; then
        mkdir -p "$(dirname "$path")"
        git -c core.symlinks=false clone "$url" "$path"
    fi
}

prepare_source "$virgl_src" git@github.com:awemorris/virglrenderer.git \
    vendor/winq-emu-virglrenderer
prepare_source "$qemu_src" git@github.com:awemorris/qemu-win32-vulkan.git \
    vendor/winq-emu-qemu

mkdir -p "$virgl_build" "$qemu_build" "$dist"

if [[ ! -f "$virgl_build/build.ninja" ]]; then
    meson setup "$virgl_build" "$virgl_src" --prefix="$MINGW_PREFIX" \
        --buildtype=release --default-library=shared \
        -Dvenus=true -Dvideo=true -Dvulkan-dload=true
fi
meson compile -C "$virgl_build" -j "$jobs"
meson install -C "$virgl_build"

if [[ ! -f "$qemu_build/build.ninja" ]]; then
    (cd "$qemu_build" && "$qemu_src/configure" \
        --target-list=x86_64-softmmu --prefix="$MINGW_PREFIX" \
        --enable-whpx --enable-opengl --enable-virglrenderer \
        --enable-sdl --enable-slirp --disable-docs --disable-plugins)
fi
ninja -C "$qemu_build" -j "$jobs" qemu-system-x86_64.exe

cp -f "$qemu_build/qemu-system-x86_64.exe" "$dist/"
cp -f "$MINGW_PREFIX/bin/libvirglrenderer-1.dll" "$dist/"
echo "Built paired WINQ-EMU files in $dist"
echo 'Copy runtime DLLs and QEMU share data from the matching UCRT64 installation when deploying.'
