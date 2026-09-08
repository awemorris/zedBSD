#!/bin/bash
# kverify.sh SRC[=CANDIDATE]...
#
# Proves that a restyled translation unit still produces the same code as its
# git HEAD (or $KSTYLE_BASE, or $KSTYLE_BASEDIR/SRC for a file that is not in
# git) for amd64 and i386.  The compile command of each source is taken from
# the real build with "make -W SRC -n OBJ", so kernel, driver and soft-float
# sources are all handled; a source the build does not reach falls back to the
# command of a representative file in the same area.  $KSTYLE_EXTRA appends
# flags to every compile (the binary128 wrappers need -mlong-double-128).
#
# Stage 1 compares per-function disassembly with __LINE__/__FILE__ fixed.
# Stage 2 compares -O0 LLVM IR with allocas sorted and value names
# canonicalized, which accepts pure declaration placement changes.
# The default candidate is the working tree.
set -u

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
work=build/kstyle
mkdir -p "$work"
objdump=build/llvm/bin/llvm-objdump
irnorm=tools/kstyle/irnorm.py

# Prints the object paths that could hold the compiled form of one source.
objects_for() {
	local src=$1 arch=$2 rest
	case $src in
	src/kern/*)
		rest=${src#src/kern/}
		if [ "$arch" = 64 ]; then
			echo "build/amd64/kern64/${src%.c}.o"
		else
			echo "build/pcat/${src%.c}.o"
		fi
		;;
	src/drivers/*)
		rest=${src#src/drivers/}
		if [ "$arch" = 64 ]; then
			echo "build/amd64/kern64/${src%.c}.o"
		else
			echo "build/pcat/drivers/${rest%.c}.o"
		fi
		;;
	src/softfloat/*)
		rest=$(basename "$src" .c)
		if [ "$arch" = 64 ]; then
			echo "build/amd64/dynamic/float/$rest.o"
			echo "build/amd64/softfloat/$rest.o"
		else
			echo "build/pcat/softfloat/$rest.o"
		fi
		;;
	esac
}

# Prints a representative source of the same area, used when the build has no
# rule that reaches the requested source.
representative_for() {
	case $1 in
	src/kern/*)	echo src/kern/waitq.c ;;
	src/drivers/*)	echo src/drivers/ethernet/dp8390.c ;;
	src/softfloat/*) echo src/softfloat/zed-softfloat.c ;;
	esac
}

# Prints the compile command of one source, with continuation lines joined.
capture() {
	local cfg=$1 src=$2 arch=$3 obj line
	for obj in $(objects_for "$src" "$arch"); do
		line=$(make ZEDBSD_CONFIG="$cfg" -W "$src" -n "$obj" 2>/dev/null |
			sed -e ':a' -e '/\\$/N; s/\\\n//; ta' |
			tr '\t' ' ' | grep -F " -c $src " | head -1)
		if [ -n "$line" ]; then
			echo "$line"
			return 0
		fi
	done
	return 1
}

# Prints the compile command to use for one source, cached under build/kstyle.
command_for() {
	local src=$1 arch=$2 cfg tag cache rep cmd
	if [ "$arch" = 64 ]; then cfg=config/ci/config-amd64.mk; else cfg=config/ci/config-pcat.mk; fi
	tag=$(echo "$src" | tr '/.' '__')
	cache="$work/cmd-$tag-$arch"
	if [ ! -s "$cache" ]; then
		cmd=$(capture "$cfg" "$src" "$arch")
		if [ -z "$cmd" ]; then
			# Falls back to the area's representative command with the
			# source substituted, so an unbuilt file is still compared
			# under one consistent command.
			rep=$(representative_for "$src")
			cmd=$(capture "$cfg" "$rep" "$arch")
			[ -n "$cmd" ] || return 1
			cmd=${cmd% -c $rep *}
			cmd="$cmd -c $src"
			echo "$src: $arch-bit command taken from $rep" >&2
		fi
		printf '%s\n' "$cmd" > "$cache"
	fi
	# Strips the compile target, the dependency options and -Werror, and
	# fixes __LINE__/__FILE__ so that a pure line shift is not a difference.
	cmd=$(cat "$cache")
	cmd=${cmd%% -c src/*}
	cmd=${cmd// -Werror/}
	cmd=$(echo "$cmd" | sed -E 's/ -MMD -MF [^ ]+//; s/ -MMD//; s/ -MP//')
	echo "$cmd -w -Wno-builtin-macro-redefined -D__LINE__=0 -D__FILE__=\"kstyle\" ${KSTYLE_EXTRA:-}"
}

norm() {
	"$objdump" -d -r --no-show-raw-insn --symbolize-operands "$1" 2>/dev/null |
	awk '/^[0-9a-f]+ <.*>:$/ {name=$2} name != "" {print name "\t" $0}' |
	sed -E 's/\.rodata[^ +]*\+0x[0-9a-f]+/.rodata+?/g; s/\.L\.str(\.[0-9]+)?/.L.str/g' |
	sort -s -k1,1
}

status=0
for spec in "$@"; do
	f=${spec%%=*}
	cand=${spec#*=}
	[ "$cand" = "$spec" ] && cand=$f
	base=$(basename "$f" .c)
	orig="$work/$base.orig.c"
	if [ -n "${KSTYLE_BASEDIR:-}" ] && [ -f "$KSTYLE_BASEDIR/$f" ]; then
		cp "$KSTYLE_BASEDIR/$f" "$orig"
	elif ! git show "${KSTYLE_BASE:-HEAD}:$f" > "$orig" 2>/dev/null; then
		echo "$f: no baseline (not in ${KSTYLE_BASE:-HEAD}, no KSTYLE_BASEDIR)"
		status=1
		continue
	fi
	line="$f:"
	for arch in 64 32; do
		cmd=$(command_for "$f" "$arch") || { line="$line no-command($arch)"; status=1; continue; }
		$cmd -I"$(dirname "$f")" -c "$orig" -o "$work/$base.orig$arch.o" 2>"$work/$base.orig$arch.err" ||
			{ line="$line baseline-compile-failed($arch)"; status=1; continue; }
		$cmd -c "$cand" -o "$work/$base.new$arch.o" 2>"$work/$base.new$arch.err" ||
			{ line="$line candidate-compile-failed($arch)"; status=1; continue; }
		norm "$work/$base.orig$arch.o" > "$work/$base.orig$arch.txt"
		norm "$work/$base.new$arch.o" > "$work/$base.new$arch.txt"
		if cmp -s "$work/$base.orig$arch.txt" "$work/$base.new$arch.txt"; then
			line="$line IDENTICAL$arch($(grep -c ':$' "$work/$base.new$arch.txt"))"
			continue
		fi
		ir="${cmd/-Os/-O0} -fno-discard-value-names -S -emit-llvm"
		$ir -I"$(dirname "$f")" -o "$work/$base.orig$arch.ll" "$orig" 2>/dev/null
		$ir -o "$work/$base.new$arch.ll" "$cand" 2>/dev/null
		python3 "$irnorm" "$work/$base.orig$arch.ll" > "$work/$base.orig$arch.nll"
		python3 "$irnorm" "$work/$base.new$arch.ll" > "$work/$base.new$arch.nll"
		if cmp -s "$work/$base.orig$arch.nll" "$work/$base.new$arch.nll"; then
			line="$line EQUIVALENT$arch(O0-IR)"
		else
			line="$line DIFFERENT$arch"
			status=1
			diff "$work/$base.orig$arch.nll" "$work/$base.new$arch.nll" | head -12 > "$work/$base.diff$arch.txt"
		fi
	done
	echo "$line"
done
exit $status
