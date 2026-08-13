#!/usr/bin/env bash
set -euo pipefail

# End-to-end test for the external M11 Noct utilities.  The shell now has
# temporary ls/cp builtins, so these scripts are invoked explicitly through
# /bin/noct.  A private native zedBSD BIOS image is used; no release image is
# modified and no linux-pc98 artifact is required.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
work="$build/tests/m11-utilities"
image="$work/m11-ide.raw"
files="$work/files"
cfg="$work/ZINIT.RC"
source_file="$files/SOURCE.BIN"
copied_file="$work/COPY.BIN"
fallback_file="$work/FALLBACK.TXT"
elf_search_file="$work/ELFTEST.TXT"
search_file="$work/SEARCH.NCT"
cwd_file="$work/CWDTEST.TXT"
cwd_script="$work/CWDCHECK.NCT"
offset=$((2048 * 512))

test "$arch" = pc98 || { echo "M11 utilities require ARCH=pc98" >&2; exit 2; }
command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || {
	echo "PC-98 BIOS directory not found: $bios_dir" >&2
	exit 1
}
for command in cmp mcopy mmd python3 timeout; do
	command -v "$command" >/dev/null || {
		echo "$command is required" >&2
		exit 1
	}
done

rm -rf -- "$files"
mkdir -p "$work" "$files"
cat >"$search_file" <<'EOF'
func main(args) {
    FileUtil.writeText("/home/" + args[0] + ".TXT", args[0]);
    return 0;
}
EOF
cat >"$cwd_script" <<'EOF'
func main(args) {
    for (entry in Directory.list(".")) {
        if (entry.name == "ls.nct") {
            FileUtil.writeText("/home/CWDTEST.TXT", "apps");
            return 0;
        }
    }
    return 1;
}
EOF
python3 - "$source_file" <<'PY'
import sys

with open(sys.argv[1], "wb") as stream:
    stream.write(bytes((index * 37 + 11) & 0xff for index in range(16417)))
PY
printf '%s\n' \
	'cd /apps' \
	'cwdcheck' \
	'noct /apps/ls.nct' \
	'noct /apps/ls.nct -l' \
	'cd /' \
	'search FALLBACK' \
	'noct /apps/search.nct ELFTEST' \
	'noct /apps/cp.nct SOURCE.BIN COPY.BIN' \
	'halt' >"$cfg"

"$repo/build.sh" bios-hdd-image pc98 build/pc98/bin/noct
cp --reflink=auto "$build/bios-hdd-image.img" "$image"
mmd -i "$image@@$offset" ::/apps
mmd -i "$image@@$offset" ::/etc
mmd -i "$image@@$offset" ::/home
mcopy -o -i "$image@@$offset" "$build/bin/noct" ::/bin/noct
mcopy -o -i "$image@@$offset" "$repo/apps/ls.nct" ::/apps/ls.nct
mcopy -o -i "$image@@$offset" "$repo/apps/cp.nct" ::/apps/cp.nct
mcopy -o -i "$image@@$offset" "$search_file" ::/apps/search.nct
mcopy -o -i "$image@@$offset" "$cwd_script" ::/apps/cwdcheck.nct
mcopy -o -i "$image@@$offset" "$source_file" ::/source.bin
mcopy -o -i "$image@@$offset" "$cfg" ::/etc/zinit.rc

set +e
timeout --signal=INT --kill-after=5 45 \
	"$qemu" -M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios_dir" \
	-nic none -drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
	-display none -serial none -monitor none -no-reboot >/dev/null 2>&1
status=$?
set -e
if test "$status" -ne 0 && test "$status" -ne 124; then
	echo "M11 QEMU failed with status $status" >&2
	exit 1
fi

rm -f -- "$copied_file"
mcopy -i "$image@@$offset" ::/copy.bin "$copied_file"
cmp -s "$source_file" "$copied_file" || {
	echo "M11 CP.NCT result differs from SOURCE.BIN" >&2
	exit 1
}
rm -f -- "$fallback_file" "$elf_search_file" "$cwd_file"
mcopy -i "$image@@$offset" ::/home/fallback.txt "$fallback_file"
mcopy -i "$image@@$offset" ::/home/elftest.txt "$elf_search_file"
mcopy -i "$image@@$offset" ::/home/cwdtest.txt "$cwd_file"
test "$(cat "$fallback_file")" = FALLBACK || {
	echo "shell /apps .nct fallback result mismatch" >&2
	exit 1
}
test "$(cat "$elf_search_file")" = ELFTEST || {
	echo "shell /bin ELF search result mismatch" >&2
	exit 1
}
test "$(cat "$cwd_file")" = apps || {
	echo "shell cwd was not inherited by an /apps Noct command" >&2
	exit 1
}
printf 'M11 explicit LS.NCT/CP.NCT QEMU test: PASS (%s)\n' "$image"
