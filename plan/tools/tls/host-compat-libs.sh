#!/bin/sh
# Builds libcrypto-compat and libssl-compat for the host as static archives, for the host tests that link the
# browser's net/tls.c (which calls the two libraries directly).
#   sh plan/tools/tls/host-compat-libs.sh <outdir>
# The sources are the lists of the zedBSD build: libcrypto-compat's common and POSIX files and libssl-compat's.
# The environment's HOST_COMPAT_CFLAGS is added to the compile flags (default none; a sanitizer's flags go here).
# The output, under <outdir>:
#   include/compat           a link to include/libc/compat of this tree (-I<outdir>/include; not -Iinclude/libc,
#                            which would hide the host C library's headers)
#   obj/<library>-<name>.o   one object per source, compiled again only when the source, a header of the two
#                            libraries or the flags changed
#   libcrypto-compat.a, libssl-compat.a
#   flags                    the compile flags of the objects
# A program links them as: <outdir>/libssl-compat.a <outdir>/libcrypto-compat.a -lpthread   (in this order).
# The last line is host-compat-libs: <outdir> (<N> crypto, <M> ssl sources); a failure says why on stderr and exits 1.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
if [ $# -ne 1 ]; then
	echo "host-compat-libs: usage: sh plan/tools/tls/host-compat-libs.sh <outdir>" >&2
	exit 1
fi
out=$1
extra=${HOST_COMPAT_CFLAGS:-}
flags="-std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fPIC -I$out/include -I. $extra"
mkdir -p "$out/include" "$out/obj"
ln -sfn "$top/include/libc/compat" "$out/include/compat"

# A changed flag string invalidates the objects.
if [ ! -f "$out/flags" ] || [ "$(cat "$out/flags")" != "$flags" ]; then
	printf '%s\n' "$flags" > "$out/flags"
fi

# The sources of each library, from its sources.mk (the common files and, for libcrypto-compat, the POSIX one).
crypto_sources=$(grep -v '^#' userland/base/libcrypto-compat/sources.mk | grep -o 'userland/base/libcrypto-compat/[a-z0-9-]*\.c' | grep -v -- '-win32\.c')
ssl_sources=$(grep -v '^#' userland/base/libssl-compat/sources.mk | grep -o 'userland/base/libssl-compat/[a-z0-9-]*\.c')

# Compiles one library's sources into objects and archives them: build_library <name> <source>...
build_library() {
	name=$1
	shift
	objects=
	for file in "$@"; do
		object=$out/obj/$name-$(basename "$file" .c).o
		if [ ! -f "$object" ] || [ "$file" -nt "$object" ] || [ "$out/flags" -nt "$object" ] ||
		    [ -n "$(find userland/base/libcrypto-compat userland/base/libssl-compat include/libc/compat -name '*.h' -newer "$object" | head -1)" ]; then
			if ! timeout 300 cc $flags -c "$file" -o "$object"; then
				echo "host-compat-libs: cannot compile $file" >&2
				return 1
			fi
		fi
		objects="$objects $object"
	done

	# The archive is written under a fixed name and moved into place (nothing is deleted; an empty file is not an
	# archive for GNU ar, so it starts with the archive's header).
	printf '!<arch>\n' > "$out/$name.a.new"
	if ! ar rcs "$out/$name.a.new" $objects; then
		echo "host-compat-libs: cannot archive $name" >&2
		return 1
	fi
	mv -f "$out/$name.a.new" "$out/$name.a"
}

build_library libcrypto-compat $crypto_sources
build_library libssl-compat $ssl_sources
echo "host-compat-libs: $out ($(echo $crypto_sources | wc -w) crypto, $(echo $ssl_sources | wc -w) ssl sources)"
