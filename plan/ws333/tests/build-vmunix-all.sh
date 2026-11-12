#!/bin/sh
# Builds the kernel of every platform of the build system, once with the
# platform's defaults and once with the thermal core on, and prints one line
# per build:
#   <platform> <variant> rc=<make status> warnings=<count> <log>
# Usage: build-vmunix-all.sh <log dir> [platform...]
# The variants are "default" (the platform's defaults.mk), "thermal"
# (CONFIG_KERN_THERMAL=y, and on x86 CONFIG_DRIVER_X86_POWER_CONTROL=y),
# "nothermal" (CONFIG_KERN_THERMAL=n, on the platforms whose defaults have
# it on) and, on x86_64, "inject" (the thermal build with
# CONFIG_THERMAL_TEST_INJECT=y).
# Each platform builds in its own build/<platform> one variant after another
# (the objects follow their flags), its sysroot first; each make has a
# 30-minute limit.  The kernel of each variant is copied to
# <log dir>/<platform>-<variant>.vmunix.
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:?log dir}
shift
cd "$repo" || exit 1
all="x86_64:config/config.x86_64.mk
intelmid64:config/config.x86_64.mk
i386:config/config.i386.mk
pc9821:config/config.pc98.mk
rpi4:config/config.rpi4.mk
rpi5:config/config.rpi5.mk
vf2:config/config.visionfive2.mk"
if [ $# -gt 0 ]; then
	sel=""
	for p in "$@"; do
		sel="$sel$(printf '%s\n' "$all" | grep "^$p:")
"
	done
	all=$sel
fi
mkdir -p "$out"
printf '%s\n' "$all" | grep . | while IFS=: read -r plat config; do
	vars="PLATFORM=$plat CONFIGMK=$config"
	log=$out/$plat-sysroot.log
	timeout 1800 make -j16 $vars sysroot >"$log" 2>&1 </dev/null
	rc=$?
	echo "$plat sysroot rc=$rc warnings=$(grep -c 'warning:' "$log") $log"
	case $plat in
	rpi4|rpi5|vf2)
		variants="default nothermal"
		;;
	x86_64)
		variants="default thermal inject"
		;;
	*)
		variants="default thermal"
		;;
	esac
	for v in $variants; do
		flags=
		case $v in
		thermal|inject)
			flags="CONFIG_KERN_THERMAL=y"
			case $plat in
			x86_64|intelmid64|i386)
				flags="$flags CONFIG_DRIVER_X86_POWER_CONTROL=y"
				;;
			esac
			;;
		nothermal)
			flags="CONFIG_KERN_THERMAL=n"
			;;
		esac
		test "$v" = inject && flags="$flags CONFIG_THERMAL_TEST_INJECT=y"
		log=$out/$plat-$v.log
		timeout 1800 make -j16 $vars $flags "build/$plat/vmunix" \
		    >"$log" 2>&1 </dev/null
		rc=$?
		test "$rc" = 0 && cp "build/$plat/vmunix" "$out/$plat-$v.vmunix"
		echo "$plat $v rc=$rc warnings=$(grep -c 'warning:' "$log") $log"
	done
done
