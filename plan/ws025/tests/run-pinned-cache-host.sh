#!/bin/sh
# Exercises pinned-cache recovery with a credit-demanding backend and pending prefetch.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-pinned-cache.XXXXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM

for mode in normal sanitize; do
	flags=''
	if [ "$mode" = sanitize ]; then
		flags='-fsanitize=address,undefined -fno-omit-frame-pointer --param=asan-globals=0'
	fi
	# The merged translation units also contain services supplied by this fixture.
	# Keep those host services strong; test file/cache/VM ownership code unchanged.
	objects=''
	for source in file filedesc readahead backing-claim vm cache io writeback vmspace; do
		object="$out/$mode-$source.o"
		control=''
		if [ "$source" = file ] && [ "${CACHE_READ_NO_RETRY:-0}" = 1 ]; then
			control='-Dvm_object_cache_reclaim_pinned=vm_object_cache_reclaim_unavailable'
		fi
		${CC:-cc} -std=c11 -pthread -O0 -g -Dtid_t=int32_t -DKERN_USER_ABI_LP64 \
			-I"$repo/include" -I"$repo/include/uapi" -I"$repo" \
			-Wall -Wextra -Werror -ffunction-sections -fdata-sections \
			$flags $control -c "$repo/src/kern/$source.c" -o "$object"
		case "$source" in
		vm) symbols='vm_reclaim_private_one vm_reclaim_one vm_commit_release vm_commit_reserve vm_metadata_enter vm_metadata_leave vm_metadata_init vm_page_untrack';;
		readahead) symbols='readahead_consumed readahead_demand_begin readahead_demand_end readahead_cancel readahead_submit';;
		writeback) symbols='writeback_mount_admit writeback_pressure';;
		io) symbols='io_pool_borrow io_pool_release';;
		*) symbols='';;
		esac
		for symbol in $symbols; do objcopy --weaken-symbol="$symbol" "$object"; done
		objects="$objects $object"
	done
	${CC:-cc} -std=c11 -pthread -O0 -g -Dtid_t=int32_t -DKERN_USER_ABI_LP64 \
		-DFILE_CACHE_STANDALONE -DFILE_CACHE_FOCUSED -I"$repo/include" -I"$repo/include/uapi" -I"$repo" \
		-Wall -Wextra -Werror -ffunction-sections -fdata-sections $flags \
		"$repo/plan/ws025/tests/file-cache-host.c" $objects \
		-Wl,--gc-sections -pthread -o "$out/$mode"
	timeout 60 "$out/$mode"
done
