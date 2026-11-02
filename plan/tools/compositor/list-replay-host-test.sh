#!/bin/sh
# Host test of the display list's replay (ws213-p003, p001 9 (a)): the scenes'
# commands are compared with the golden file taken from the direct recording.
# Usage: plan/tools/compositor/list-replay-host-test.sh [build-dir]  (from the tree root)
set -eu
out=${1:-build/ws213-host}
mkdir -p "$out"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I. -Iinclude -Iuserland/desktop/wayland \
	plan/tools/compositor/list-replay-host-test.c userland/desktop/wayland/batch.c \
	userland/desktop/wayland/compose-bind.c userland/desktop/wayland/scene.c \
	userland/desktop/wayland/draw-list.c \
	-o "$out/list-replay-host-test"
"$out/list-replay-host-test" "$out/list-replay.txt"
if ! cmp -s "$out/list-replay.txt" plan/tools/compositor/list-replay.golden; then
	diff -u plan/tools/compositor/list-replay.golden "$out/list-replay.txt" | head -40
	echo "list-replay-host-test: FAIL"
	exit 1
fi
# The drawing writes the list only: Vulkan commands are recorded by draw-list.c's
# replay, by the host images' copies and new images' layouts (shm.c, import.c: one barrier as a render-thread task or in a one-time buffer),
# and by the cursor thread, which records its own command buffers (cursor-thread.c, ws213-p004).
walk=$(grep -l 'vkCmd' userland/desktop/wayland/*.c | grep -v -e '/draw-list.c$' -e '/cursor-thread.c$' -e '/shm.c$' -e '/import.c$' || true)
if [ -n "$walk" ] || [ "$(grep -c 'vkCmd' userland/desktop/wayland/shm.c)" != 4 ] || [ "$(grep -c 'vkCmd' userland/desktop/wayland/import.c)" != 2 ]; then
	echo "list-replay-host-test: FAIL (a Vulkan command recorded outside the list: $walk)"
	exit 1
fi
echo "list-replay-host-test: PASS ($(wc -l < "$out/list-replay.txt") lines)"
