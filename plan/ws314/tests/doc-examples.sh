#!/bin/sh
# The examples of docs/howto/keiland-programming.md (docs/howto/keiland-examples/*.c).  Run from the worktree's root.
#
#   sh plan/ws314/tests/doc-examples.sh [LIBDIR]
#
# compiles every example against the tree's headers with clang and gcc (-Wall -Wextra -Werror), links each with a
# Linux build of libkeiland, checks that the guide shows each example whole (the ```c block after its
# "<!-- example: keiland-examples/NAME.c -->" marker is the file, byte for byte; a catalog's ```text block after
# "<!-- example: keiland-examples/locale/ja/NAME.tr -->" likewise), and that each Japanese catalog gives a
# translation for every key it holds (read with kl_tr_open_directory).  LIBDIR: the lib/ of a Linux build
# (make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=DIR DIR/lib/libkeiland.so); default
# build/p009-linux/lib.  Without libkeiland.so there, the link is reported as not done.
#
#   sh plan/ws314/tests/doc-examples.sh guest [BUILD [NAME...]]
#
# runs every example on the zedBSD Venus guest and photographs it: builds the lean desktop image
# (plan/tools/titlebar/build-menu-image.sh BUILD, default build/amd64), compiles the examples against that build's
# sysroot and libkeiland, boots the image (plan/ws035/tests/zdesktop-guest.sh, runtime build/p009-run, its disk with
# cache=unsafe through a wrapper of qemu-system-x86_64 on PATH), puts the examples' Japanese catalogs where the
# desktop's are, starts the compositor with glass, and for each example starts it, gives it a few keys or a click, and
# takes the compositor's picture into build/review/ws314-doc-NAME.png.  07-services runs twice: first, and last after
# 08-language left the desktop in Japanese (the picture 07-services-ja).
# NAMEs (pictures' names, such as 09-shell) run only those.
# The judgement is the pictures and the programs' exit status read over SSH; no console log is read.  Needs the Venus
# renderer in build/ws035-sq-venus and /dev/dri/renderD128 (plan/tools.md).
#
# Both write into build/tmp/doc-examples (a new directory each run, plan/tools/fresh-out.sh) and end with a SUMMARY
# line.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u

. plan/tools/fresh-out.sh

examples=docs/howto/keiland-examples
guide=docs/howto/keiland-programming.md
flags="-std=gnu17 -Wall -Wextra -Werror -Iuserland/desktop/include"

# The run's own directory, not the link to the latest run: a run started meanwhile moves the link.
fresh_out build/tmp/doc-examples
out=build/tmp/$(basename "$fresh_dir")
pass=0
fail=0
skipped=0

report() {
	printf '%s %s\n' "$1" "$2"
	case $1 in
	PASS) pass=$((pass + 1)) ;;
	FAIL) fail=$((fail + 1)) ;;
	*) skipped=$((skipped + 1)) ;;
	esac
}

summary() {
	printf 'SUMMARY pass=%d fail=%d not-done=%d out=%s\n' "$pass" "$fail" "$skipped" "$(readlink -f "$out")"
	[ "$fail" -eq 0 ]
}

# The guest's run: image, programs, boot, one picture per example.
if [ "${1:-}" = guest ]; then
	build=${2:-build/amd64}
	shift
	[ $# -gt 0 ] && shift
	only=" $* "
	runtime=$PWD/build/p009-run
	sysroot=$build/sysroot
	clang=build/llvm/bin/clang
	export GUEST_RUNTIME="$runtime"
	guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
	keys() { timeout 60 python3 plan/ws035/tests/qmp-keys.py "$runtime/qmp.sock" "$@" >/dev/null 2>&1; }
	pointer() { timeout 60 python3 plan/ws035/tests/qmp-pointer.py "$runtime/qmp.sock" "$@" >/dev/null 2>&1; }
	# The compositor's picture through the Venus head's VNC, with a deadline a loaded host meets (up to 3 tries).
	shot() {
		tries=0
		while [ $tries -lt 3 ]; do
			if timeout 200 python3 -c '
import sys
sys.path.insert(0, "plan/ws014/tests")
sys.path.insert(0, "plan/ws035/tests")
from venus_rfb import capture
from ppm2png import read_ppm, write_png
if capture(sys.argv[1], sys.argv[2] + ".ppm", 120) is None:
	sys.exit(1)
width, height, pixels = read_ppm(sys.argv[2] + ".ppm")
write_png(sys.argv[2], width, height, pixels)
print("picture %s %dx%d" % (sys.argv[2], width, height))
' "$runtime/vnc.sock" "$1" >> "$2" 2>&1; then
				return 0
			fi
			tries=$((tries + 1))
		done
		return 1
	}

	# The image, made the standard way (a config.mk build and files of the tree), on the build's own sysroot.
	if timeout 1800 make -j16 ZEDBSD_CONFIG=plan/tools/titlebar/config-amd64-menu.mk BUILD="$build" sysroot-amd64 \
		> "$out/image.log" 2>&1 &&
		flock /tmp/zedbsd-image-build.lock timeout 5400 plan/tools/titlebar/build-menu-image.sh "$build" \
		>> "$out/image.log" 2>&1; then
		report PASS "image $build/hdd-image.img"
	else
		report FAIL "image (see $out/image.log)"
		summary
		exit 1
	fi

	# Each example for zedBSD, against the image's own sysroot and libraries.
	mkdir -p "$out/zedbsd"
	for source in "$examples"/*.c; do
		name=$(basename "$source" .c)
		if timeout 120 "$clang" --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -nostdinc \
			-isystem "$sysroot/usr/include" -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC -m64 -march=x86-64 -O2 -fPIC \
			-fno-stack-protector -std=gnu17 -Wall -Wextra -Werror -c "$source" -o "$out/zedbsd/$name.o" \
			> "$out/zedbsd/$name.log" 2>&1 &&
			timeout 120 "$clang" --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -m64 -nostdlib -pie \
			-Wl,--no-relax -Wl,--hash-style=sysv,-z,now -Wl,--allow-shlib-undefined -Wl,--dynamic-linker=/lib/ld.so \
			"$sysroot/usr/lib/crt1.o" "$out/zedbsd/$name.o" -L"$build/dynamic" -Wl,-rpath-link,"$build/dynamic" \
			-l:libkeiland.so -l:libc.so -o "$out/zedbsd/$name" >> "$out/zedbsd/$name.log" 2>&1; then
			report PASS "zedbsd build $name"
		else
			report FAIL "zedbsd build $name (see $out/zedbsd/$name.log)"
		fi
	done

	# The emulator with the boot disk's cache=unsafe (a slow host fsync stalls the guest's disk otherwise).
	mkdir -p "$out/qemu"
	cat > "$out/qemu/qemu-system-x86_64" <<'EOF'
#!/bin/sh
for argument in "$@"; do
	case $argument in
	if=none,id=boot,*) argument="$argument,cache=unsafe" ;;
	esac
	set -- "$@" "$argument"
	shift
done
exec /usr/bin/qemu-system-x86_64 "$@"
EOF
	chmod 0755 "$out/qemu/qemu-system-x86_64"

	# Boots the guest and waits for its SSH (a loaded host boots slowly).
	PATH="$PWD/$out/qemu:$PATH" timeout 120 plan/ws035/tests/zdesktop-guest.sh start "$build/hdd-image.img" \
		> "$out/start.log" 2>&1
	if timeout 600 python3 plan/tools/guest/guest.py wait --timeout 300 > "$out/wait.log" 2>&1; then
		report PASS "guest up"
	else
		report FAIL "guest up (see $out/wait.log; analyse with the gdbstub, plan/tools.md)"
		timeout 60 plan/ws035/tests/zdesktop-guest.sh stop > /dev/null 2>&1
		summary
		exit 1
	fi

	# The examples' catalogs where the desktop's are (KEILAND_DATADIR/keiland/locale).
	for catalog in "$examples"/locale/ja/*.tr; do
		timeout 120 python3 plan/tools/guest/guest.py put "$catalog" "/usr/share/keiland/locale/ja/$(basename "$catalog")" \
			>/dev/null 2>&1 </dev/null
	done

	# The programs in, and the compositor started with glass and the wallpaper.
	for program in "$out"/zedbsd/*; do
		case $program in
		*.o|*.log) continue ;;
		esac
		timeout 120 python3 plan/tools/guest/guest.py put "$program" "/tmp/$(basename "$program")" >/dev/null 2>&1 </dev/null
	done
	guest 'export XDG_RUNTIME_DIR=/tmp; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=3600 --width=1280 --height=800 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null & sleep 15; echo started' > "$out/compositor.txt"

	# Each example: started, given its input, photographed, and ended.  A picture's name and its program, in order:
	# 08-language leaves the desktop in Japanese, and 07-services-ja is 07-services started after it.
	mkdir -p build/review "$out/shots"
	for run in 01-app 02-window 03-titlebar 04-counter 04-counter-parts 05-widgets 06-menu 07-services 08-worker \
		09-sort 09-shell 10-instance 08-language 07-services-ja:07-services; do
		name=${run%%:*}
		program=${run#*:}
		case $only in
		"  ") ;;
		*" $name "*) ;;
		*) continue ;;
		esac
		# The notification of 01-app goes after a few seconds; the windows are waited for longer.
		wait_start=8
		[ "$name" = 01-app ] && wait_start=2
		# 10-instance's copies find each other in a runtime directory that is the user's alone, beside the compositor's socket.
		runtime_dir=/tmp
		if [ "$name" = 10-instance ]; then
			runtime_dir=/tmp/run10
			guest "mkdir -m 0700 -p $runtime_dir; for s in /tmp/wayland-*; do case \$s in *.lock) ;; *) ln -sf \$s $runtime_dir/ ;; esac; done; ls $runtime_dir" > "$out/$name.runtime.txt"
		fi
		guest "chmod 0755 /tmp/$program; export XDG_RUNTIME_DIR=$runtime_dir HOME=/tmp; cd /tmp; /tmp/$program > /tmp/$name.out 2>&1 </dev/null & echo \$! > /tmp/$name.pid; sleep $wait_start; echo started" > "$out/$name.start.txt"
		# The input of each, at the places of the windows the compositor centres on its 1280x800 output.
		case $name in
		02-window) pointer move 640 400 sleep 300 move 660 410 sleep 600 ;;
		03-titlebar) pointer move 536 258 sleep 300 down sleep 80 up sleep 600 down sleep 80 up sleep 600 move 640 420 sleep 300 ;;
		04-counter) keys '<up>' '<up>' '<up>'; sleep 2; pointer move 727 448 sleep 300 down sleep 80 up sleep 1500 ;;
		04-counter-parts) keys '<up>' '<up>' '<up>'; sleep 2; pointer move 727 448 sleep 1500 ;;
		05-widgets) keys '<tab>' '<ret>'; sleep 2
			pointer move 488 424 sleep 300 down sleep 80 up sleep 600 move 406 331 sleep 300 down sleep 80 up sleep 600 \
				move 713 369 sleep 300 down sleep 60 up sleep 100 down sleep 60 up sleep 1500 ;;
		06-menu) keys '<ctrl-equal>' '<ctrl-equal>'; sleep 2; pointer move 640 400 sleep 300 right-down sleep 80 right-up sleep 1200 ;;
		07-services) pointer move 670 542 sleep 300 down sleep 80 up sleep 1000 move 476 542 sleep 300 down sleep 80 up sleep 6000 ;;
		08-worker) pointer move 492 528 sleep 300 down sleep 80 up sleep 2500 ;;
		09-sort) pointer move 492 558 sleep 300 down sleep 80 up sleep 2000 ;;
		# 09-shell: a command, then a password read (zedBSD's sh edits its own prompt without the terminal's echo and
		# line mode; a read waits with the modes stty set).
		09-shell) keys 'echo hello from sh' '<ret>'; sleep 1; keys 'stty -echo; printf "Password: "; read word' '<ret>'
			sleep 1; keys 'secret'; sleep 1 ;;
		10-instance) pointer move 514 558 sleep 300 down sleep 80 up sleep 1500 down sleep 80 up sleep 1500
			guest "export XDG_RUNTIME_DIR=$runtime_dir HOME=/tmp; cd /tmp; /tmp/10-instance From-the-shell; echo handed=\$?" \
				> "$out/$name.second.txt"; sleep 2 ;;
		08-language) pointer move 534 518 sleep 300 down sleep 80 up sleep 200 down sleep 80 up sleep 200 down sleep 80 up sleep 3000 ;;
		*) sleep 1 ;;
		esac
		if shot "$out/shots/$name.png" "$out/$name.shot.txt"; then
			cp "$out/shots/$name.png" "build/review/ws314-doc-$name.png"
			report PASS "picture $name (build/review/ws314-doc-$name.png)"
		else
			report FAIL "picture $name (see $out/$name.shot.txt)"
		fi
		keys '<esc>'
		guest "kill \$(cat /tmp/$name.pid) 2>/dev/null; sleep 1; echo exit; cat /tmp/$name.out" > "$out/$name.end.txt"
	done

	# The guest stopped.
	timeout 60 plan/ws035/tests/zdesktop-guest.sh stop > /dev/null 2>&1
	summary
	exit $?
fi

libdir=${1:-build/p009-linux/lib}

# Each example: compiled by both compilers, then linked with libkeiland.
for source in "$examples"/*.c; do
	name=$(basename "$source" .c)
	for compiler in clang gcc; do
		if timeout 120 "$compiler" $flags -c "$source" -o "$out/$name.$compiler.o" > "$out/$name.$compiler.log" 2>&1; then
			report PASS "compile $compiler $name"
		else
			report FAIL "compile $compiler $name (see $out/$name.$compiler.log)"
		fi
	done
	if [ -f "$libdir/libkeiland.so" ]; then
		if timeout 120 clang "$out/$name.clang.o" -L"$libdir" -Wl,-rpath-link,"$libdir" -lkeiland -o "$out/$name" \
			> "$out/$name.link.log" 2>&1; then
			report PASS "link $name"
		else
			report FAIL "link $name (see $out/$name.link.log)"
		fi
	else
		report NOT-DONE "link $name (no $libdir/libkeiland.so)"
	fi
done

# Each Japanese catalog read as the examples read theirs: every msg and plural entry gives its translation.
cat > "$out/catalog-check.c" <<'CHECK'
#include <keiland/keiland.h>
#include <stdio.h>
#include <string.h>

/* catalog-check DIRECTORY DOMAIN (msg ENGLISH TEXT | plural SINGULAR PLURAL FORM)...: 0 when every entry translates. */
int
main(int argc, char *argv[])
{
	const char *text;
	int index;
	int status;
	int failed;

	status = kl_tr_open_directory(argv[1], argv[2], "ja");
	if (status != 0) {
		printf("open %s: %d\n", argv[2], status);
		return 1;
	}
	failed = 0;
	index = 3;
	while (index + 2 < argc) {
		if (strcmp(argv[index], "plural") == 0 && index + 3 < argc) {
			text = kl_trn(argv[index + 1], argv[index + 2], 2UL);
			if (strcmp(text, argv[index + 3]) != 0) {
				printf("plural %s: %s\n", argv[index + 1], text);
				failed = 1;
			}
			index += 4;
			continue;
		}
		text = kl_tr(argv[index + 1]);
		if (strcmp(text, argv[index + 2]) != 0) {
			printf("msg %s: %s\n", argv[index + 1], text);
			failed = 1;
		}
		index += 3;
	}
	return failed;
}
CHECK
for catalog in "$examples"/locale/ja/*.tr; do
	domain=$(basename "$catalog" .tr)
	if [ ! -f "$libdir/libkeiland.so" ]; then
		report NOT-DONE "catalog $domain (no $libdir/libkeiland.so)"
		continue
	fi
	if ! timeout 120 clang $flags "$out/catalog-check.c" -L"$libdir" -Wl,-rpath-link,"$libdir" -lkeiland \
		-o "$out/catalog-check" > "$out/catalog-check.log" 2>&1; then
		report FAIL "catalog $domain (see $out/catalog-check.log)"
		continue
	fi
	# The entries as arguments: each field of a msg or plural line, tab-separated, one argument.
	awk -F '\t' '$1 == "msg" || $1 == "plural" { for (i = 1; i <= NF; i++) printf "%s%c", $i, 0 }' "$catalog" \
		> "$out/$domain.entries"
	if xargs -0 env LD_LIBRARY_PATH="$libdir" timeout 60 "$out/catalog-check" "$examples/locale" "$domain" \
		< "$out/$domain.entries" > "$out/$domain.check.txt" 2>&1; then
		report PASS "catalog $domain"
	else
		report FAIL "catalog $domain (see $out/$domain.check.txt)"
	fi
done

# Each example shown whole in the guide (a program in a ```c block, a catalog in a ```text one), and each marker
# naming an example that exists.
for source in "$examples"/*.c "$examples"/locale/ja/*.tr; do
	name=${source#"$examples"/}
	fence='```c'
	case $name in
	*.tr) fence='```text' ;;
	esac
	awk -v marker="<!-- example: keiland-examples/$name -->" -v fence="$fence" '
		$0 == marker { found = 1; next }
		found == 1 && $0 == fence { inside = 1; found = 2; next }
		inside && /^```$/ { exit }
		inside { print }
	' "$guide" > "$out/$(basename "$name").shown"
	if cmp -s "$source" "$out/$(basename "$name").shown"; then
		report PASS "shown $name"
	else
		report FAIL "shown $name (the guide's block differs: diff $source $out/$(basename "$name").shown)"
	fi
done
for name in $(sed -n 's/^<!-- example: keiland-examples\/\([^ ]*\) -->$/\1/p' "$guide"); do
	if [ -f "$examples/$name" ]; then
		report PASS "marker $name"
	else
		report FAIL "marker $name (no such example)"
	fi
done

summary
