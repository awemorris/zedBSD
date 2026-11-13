#!/bin/sh
# Builds the host's Venus renderer that libvulkan needs for QEMU guests: virglrenderer 1.1.0 with WS014's
# strict-queue patch (plan/ws014/phase008/renderer-strict/virglrenderer-1.1.0-strict.patch) and its native
# quiescence patch (plan/ws014/phase009/renderer-quiesce/host-quiesce-proposal.patch), configured as WS014 did
# (Venus, EGL, process worker, release), installed into a private prefix of this tree.  The stock Debian
# virglrenderer enumerates no physical device to libvulkan (no strict queue completion, no native quiescence).
#
#   sh plan/ws353/tests/build-venus-renderer.sh [PREFIX]
#
# PREFIX (default build/ws035-sq-venus/install, the path plan/ws035/tests/zdesktop-guest.sh and the Venus tests read)
# must not exist yet: the script never removes or overwrites an earlier build.  Its work goes beside it in
# PREFIX.work.XXXXXX (the tarball, the patched source, the build and the logs) and provenance.txt in PREFIX records
# the tarball's URL and SHA-256, the patches' and the changed sources' SHA-256 and the installed files'.  Nothing is
# installed on the system and sudo is not used.  The build needs cc, meson, ninja, pkg-config and the development
# files of epoxy, libdrm, gbm and vulkan; a missing one is named and the script stops.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
tree=$PWD
prefix=${1:-build/ws035-sq-venus/install}
case $prefix in
/*) ;;
*) prefix=$tree/$prefix ;;
esac
url=https://gitlab.freedesktop.org/virgl/virglrenderer/-/archive/1.1.0/virglrenderer-1.1.0.tar.gz
strict=$tree/plan/ws014/phase008/renderer-strict/virglrenderer-1.1.0-strict.patch
strict_sha=b8b2e4cff8be97f007955f59d558dea0ae13476ca28b0bb3043a283eb93d75c0
strict_provenance=$tree/plan/ws014/phase008/renderer-strict/provenance.json
quiesce=$tree/plan/ws014/phase009/renderer-quiesce/host-quiesce-proposal.patch
quiesce_sha=04f75fad2ee92c1ba0b78bf8751efd08ed6d3503774cc1d0e0e0d0ebb1ed8cf9
quiesce_proposal=$tree/plan/ws014/phase009/renderer-quiesce/proposal.json

fail() {
	echo "build-venus-renderer: FAIL $*" >&2
	exit 1
}

# The tools and development files the build needs.
for tool in cc meson ninja pkg-config patch curl tar sha256sum python3; do
	command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool"
done
missing=
for module in epoxy libdrm gbm vulkan; do
	pkg-config --exists "$module" || missing="$missing $module"
done
[ -z "$missing" ] || fail "missing development files (pkg-config):$missing (Debian: libepoxy-dev libdrm-dev libgbm-dev libvulkan-dev)"

# A finished prefix is kept as it is.
[ ! -e "$prefix" ] || fail "$prefix exists; give another PREFIX"
mkdir -p "$(dirname -- "$prefix")"
work=$(mktemp -d "$prefix.work.XXXXXX")
echo "build-venus-renderer: work in $work"

# The patches are the reviewed ones.
[ "$(sha256sum < "$strict" | cut -d' ' -f1)" = "$strict_sha" ] || fail "strict patch hash"
[ "$(sha256sum < "$quiesce" | cut -d' ' -f1)" = "$quiesce_sha" ] || fail "quiesce patch hash"

# The upstream tarball, downloaded into a directory of its own and unpacked without running anything from it.
mkdir -p "$work/dist" "$work/source"
timeout 300 curl --fail --location --silent --show-error --output "$work/dist/virglrenderer-1.1.0.tar.gz" "$url"
tarball_sha=$(sha256sum < "$work/dist/virglrenderer-1.1.0.tar.gz" | cut -d' ' -f1)
tar -xzf "$work/dist/virglrenderer-1.1.0.tar.gz" --strip-components=1 -C "$work/source"
grep -q 'MIT' "$work/source/COPYING" || fail "the source's COPYING is not the MIT license"

# The changed files are the 1.1.0 ones WS014 patched (its recorded before hashes), then its after hashes.
python3 -I - "$strict_provenance" "$work/source" before > "$work/check-before.txt" <<'EOF' || fail "source differs from WS014's 1.1.0 (see $work/check-before.txt)"
import hashlib, json, sys
provenance, source, which = sys.argv[1], sys.argv[2], sys.argv[3]
files = json.load(open(provenance))['profiles']['full']['changed_files']
bad = 0
for name, sums in sorted(files.items()):
	key = which + '_sha256'
	if sums.get(key) is None:
		continue
	try:
		actual = hashlib.sha256(open(source + '/' + name, 'rb').read()).hexdigest()
	except FileNotFoundError:
		actual = 'absent'
	state = 'ok' if actual == sums[key] else 'DIFFERS'
	bad += state != 'ok'
	print(state, name, actual)
sys.exit(1 if bad else 0)
EOF
patch --batch --fuzz=0 -p1 -d "$work/source" -i "$strict" > "$work/patch-strict.log" 2>&1 || fail "strict patch (see $work/patch-strict.log)"
python3 -I - "$strict_provenance" "$work/source" after > "$work/check-after.txt" <<'EOF' || fail "strict source differs from WS014's (see $work/check-after.txt)"
import hashlib, json, sys
provenance, source, which = sys.argv[1], sys.argv[2], sys.argv[3]
files = json.load(open(provenance))['profiles']['full']['changed_files']
bad = 0
for name, sums in sorted(files.items()):
	key = which + '_sha256'
	if sums.get(key) is None:
		continue
	actual = hashlib.sha256(open(source + '/' + name, 'rb').read()).hexdigest()
	state = 'ok' if actual == sums[key] else 'DIFFERS'
	bad += state != 'ok'
	print(state, name, actual)
sys.exit(1 if bad else 0)
EOF
patch --batch --fuzz=0 -p1 -d "$work/source" -i "$quiesce" > "$work/patch-quiesce.log" 2>&1 || fail "quiesce patch (see $work/patch-quiesce.log)"
python3 -I - "$quiesce_proposal" "$work/source" > "$work/check-quiesce.txt" <<'EOF' || fail "quiesced source differs from WS014's (see $work/check-quiesce.txt)"
import hashlib, json, sys
proposal, source = sys.argv[1], sys.argv[2]
files = json.load(open(proposal))['host_proposed_sha256']
bad = 0
for name, expected in sorted(files.items()):
	actual = hashlib.sha256(open(source + '/' + name, 'rb').read()).hexdigest()
	state = 'ok' if actual == expected else 'DIFFERS'
	bad += state != 'ok'
	print(state, name, actual)
sys.exit(1 if bad else 0)
EOF

# WS014's configuration, four jobs at most, into the private prefix.
timeout 600 meson setup "$work/build" "$work/source" --prefix="$prefix" \
    -Dvenus=true -Dplatforms=egl -Dtests=false -Dvideo=false -Drender-server-worker=process \
    -Dminigbm_allocation=false -Dbuildtype=release > "$work/setup.log" 2>&1 || fail "meson setup (see $work/setup.log)"
timeout 1800 ninja -C "$work/build" -j4 > "$work/build.log" 2>&1 || fail "ninja (see $work/build.log)"
timeout 600 meson install -C "$work/build" --no-rebuild > "$work/install.log" 2>&1 || fail "meson install (see $work/install.log)"
sync

# What was built from what.
library=$prefix/lib/x86_64-linux-gnu/libvirglrenderer.so.1.9.0
server=$prefix/libexec/virgl_render_server
[ -f "$library" ] || library=$(find "$prefix" -name 'libvirglrenderer.so.*.*.*' | head -n 1)
[ -x "$server" ] || fail "no server in $prefix"
{
	echo "tarball $url"
	echo "tarball_sha256 $tarball_sha"
	echo "strict_patch_sha256 $strict_sha"
	echo "quiesce_patch_sha256 $quiesce_sha"
	echo "license_sha256 $(sha256sum < "$work/source/COPYING" | cut -d' ' -f1)"
	echo "library $library"
	echo "library_sha256 $(sha256sum < "$library" | cut -d' ' -f1)"
	echo "server $server"
	echo "server_sha256 $(sha256sum < "$server" | cut -d' ' -f1)"
	echo "work $work"
	echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(uname -srm)"
} > "$prefix/provenance.txt"
cp "$work/source/COPYING" "$prefix/COPYING.virglrenderer"
cat "$prefix/provenance.txt"
echo "build-venus-renderer: PASS"
