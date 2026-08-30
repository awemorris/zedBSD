#!/bin/sh
# WS018 KA-T090 consolidated FAT host runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)

# KA-T090 is the p010 compatibility checkpoint.  Once p011 removes FAT's
# bootfs interface, keep this maintained entry point useful by running the
# native fixture which repeats that behavior through VFS and adds the p011
# lifetime/error contracts.  On a p010 checkout the original fixture below
# remains directly runnable.
if ! grep -q 'bootfat12_driver' "$repo_dir/include/kern/fat.h"; then
	echo "KA-T090: native FAT supersedes the p010 bootfs checkpoint"
	exec "$test_dir/run-fat-native-vfs-host-test.sh"
fi

temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-fat-consolidation.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

common_flags="-std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror \
-ffunction-sections -fdata-sections -I$repo_dir/include \
-I$repo_dir/include/uapi -I$repo_dir/src -I$repo_dir/libc/include \
-I$repo_dir"
test_source=$test_dir/fat-consolidation-host-test.c

build_and_run()
{
	name=$1
	extra_flags=$2

	# shellcheck disable=SC2086
	"${CC:-cc}" $common_flags $extra_flags -c \
		"$repo_dir/src/drivers/fs/fat.c" -o "$temporary/fat-$name.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" $common_flags $extra_flags -c \
		"$repo_dir/src/kern/fs.c" -o "$temporary/fs-$name.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" $common_flags $extra_flags "$test_source" \
		"$temporary/fat-$name.o" "$temporary/fs-$name.o" \
		-Wl,--gc-sections -o "$temporary/ka-t090-$name"
	if [ "$name" = sanitize ]; then
		ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
			"$temporary/ka-t090-$name"
	else
		"$temporary/ka-t090-$name"
	fi
}

build_and_run ordinary "-O0"
build_and_run sanitize \
	"-O0 -g -fno-omit-frame-pointer -fsanitize=address,undefined"
