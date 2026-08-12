#!/usr/bin/env bash
set -euo pipefail

# Compile apps/holoris.nct to HOLORIS.NAP with the host Noct compiler.
# The 5M small-memory profile cannot compile the source on the target, so
# the boot volume ships bytecode, exactly like Remacs.
repo="$(cd "$(dirname "$0")/.." && pwd)"
noct_src="$repo/noct"
host_build="${NOCT_HOST_BUILD:-$repo/build/host-noct}"
output_dir="${HOLORIS_OUTPUT_DIR:-$repo/build/holoris}"
noct="${NOCT_HOST:-}"

test -f "$repo/apps/holoris.nct" || {
	echo "Holoris source not found: $repo/apps/holoris.nct" >&2
	exit 1
}

if test -z "$noct"; then
	test -f "$noct_src/CMakeLists.txt" || {
		echo "Noct submodule is missing; run git submodule update --init noct" >&2
		exit 1
	}
	command -v cmake >/dev/null 2>&1 || {
		echo "cmake is required" >&2
		exit 1
	}
	cmake -S "$noct_src" -B "$host_build" \
		-DCMAKE_BUILD_TYPE=Release \
		-DNOCT_ENABLE_STATIC=ON \
		-DNOCT_ENABLE_CLI=ON \
		-DNOCT_ENABLE_JIT=ON \
		-DNOCT_ENABLE_API=ON \
		-DNOCT_ENABLE_API_SYSTEM=ON \
		-DNOCT_ENABLE_API_CONSOLE=ON \
		-DNOCT_ENABLE_API_FILE=ON \
		-DNOCT_ENABLE_API_TERM=ON \
		-DNOCT_ENABLE_REPL=ON \
		-DNOCT_ENABLE_BCBACKEND=ON \
		-DNOCT_ENABLE_INSTALL=OFF
	cmake --build "$host_build" --target noctcli \
		--parallel "${JOBS:-$(nproc)}"
	noct="$host_build/noct"
fi

mkdir -p "$output_dir"
cp "$repo/apps/holoris.nct" "$output_dir/HOLORIS.NCT"
(cd "$output_dir" && "$noct" --compile HOLORIS.NCT)
mv "$output_dir/HOLORIS.nb" "$output_dir/HOLORIS.NAP"
rm -f "$output_dir/HOLORIS.NCT"
echo "Holoris bytecode: $output_dir/HOLORIS.NAP"
