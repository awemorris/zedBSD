#!/bin/sh
# OmniGrammaton from its zip under Wine (x86_64; the reference, not a real Windows): make browser-win64-zip,
# the zip extracted into a new directory, Wine's own listing of the folder (wine cmd /c dir /s /b), the icon
# in OmniGrammaton.exe's resources (wrestool: a group of 8 icons, and ImageMagick's identify reads the
# group extracted as an .ico of 8 images), the version resource's strings, and OmniGrammaton.exe started
# from the extracted folder in Xvfb on its start page: a screenshot (zip-start.png) and the 256 pixel icon
# (icon-256.png) in build/review/ws334-p031/.  Wine runs under flock /tmp/zedbsd-wine.lock.
#   sh plan/ws334/tests/zip-wine.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# the Wine prefix is build/tmp/ws334-wine-gui.  Last line: zip-wine: PASS or zip-wine: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd)
. plan/tools/fresh-out.sh
out=build/tmp/ws334-zip-wine
fresh_out "$out"
out=$root/$out
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
build=build/tmp/ws334-zip-build/browser-win64
review=build/review/ws334-p031
mkdir -p "$review"
status=0

# The zip.
if ! timeout 1800 make --no-print-directory browser-win64-zip LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" \
    BROWSER_WIN32_SOURCES=build/tmp/ws334-win32-sources > "$out/make.log" 2>&1; then
	echo "zip-wine: make browser-win64-zip failed (log $out/make.log)"
	tail -5 "$out/make.log"
	echo "zip-wine: FAIL"
	exit 1
fi
zip=$(ls "$build"/OmniGrammaton-*-win64.zip | head -1)
echo "zip-wine: $zip ($(wc -c < "$zip") bytes)"

# Extracted into a new directory.
mkdir -p "$out/extracted"
unzip -q "$zip" -d "$out/extracted" || { echo "zip-wine: cannot extract"; echo "zip-wine: FAIL"; exit 1; }
program=$out/extracted/OmniGrammaton/OmniGrammaton.exe

# Wine's listing of the folder.
WINEPREFIX=$root/build/tmp/ws334-wine-gui WINEDEBUG=-all timeout 120 flock /tmp/zedbsd-wine.lock \
    wine cmd /c dir /s /b "Z:$(echo "$out/extracted/OmniGrammaton" | tr '/' '\\')" > "$out/dir.out" 2>&1
tr -d '\r' < "$out/dir.out" > "$out/dir.txt"
files=$(grep -c '\.[A-Za-z0-9]*$' "$out/dir.txt")
if grep -q 'OmniGrammaton\.exe$' "$out/dir.txt" && grep -q 'ca-certificates\.pem$' "$out/dir.txt"; then
	printf "zip-wine: Wine lists the folder: %s entries with a file name's extension, OmniGrammaton.exe and %s among them\n" "$files" 'share\certs\ca-certificates.pem'
else
	echo "zip-wine: Wine's listing lacks OmniGrammaton.exe or the roots ($out/dir.txt)"
	status=1
fi

# The icon and the version in the resources.
wrestool -l "$program" > "$out/resources.txt" 2> /dev/null
icons=$(grep -c 'type=icon ' "$out/resources.txt")
wrestool -x --type=14 --name=1 "$program" > "$out/OmniGrammaton.ico" 2> /dev/null
read_icons=$(identify "$out/OmniGrammaton.ico" 2> /dev/null | wc -l)
convert "$out/OmniGrammaton.ico[7]" "$review/icon-256.png" 2> /dev/null
strings -e l "$program" | grep -A1 -E '^(ProductName|FileDescription|CompanyName|FileVersion|LegalCopyright)$' | grep -v '^--$' | paste - - > "$out/version.txt"
if [ "$icons" = 8 ] && [ "$read_icons" = 8 ] && grep -q 'OmniGrammaton Web Browser' "$out/version.txt"; then
	echo "zip-wine: the program's icon has 8 images (wrestool, identify); its version: $(tr '\t\n' '= ' < "$out/version.txt")"
else
	echo "zip-wine: the resources: $icons icons, $read_icons read, version '$(cat "$out/version.txt")'"
	status=1
fi

# Started from the extracted folder.
cat > "$out/start.sh" << 'EOF'
#!/bin/sh
cd "$1" || exit 2
WINEDEBUG=-all timeout 90 wine OmniGrammaton.exe share/browser/start.html > "$2/start.out" 2> "$2/start.err" &
sleep 25
import -window root "$2/start.png"
wineserver -k 2> /dev/null
wait
EOF
WINEPREFIX=$root/build/tmp/ws334-wine-gui VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
    timeout 600 flock /tmp/zedbsd-wine.lock xvfb-run -a -s "-screen 0 1280x800x24" \
    sh "$out/start.sh" "$out/extracted/OmniGrammaton" "$out" > "$out/xvfb.log" 2>&1
tr -d '\r' < "$out/start.out" > "$out/start.txt" 2> /dev/null
colours=$(convert "$out/start.png" -format %k info: 2> /dev/null || echo 0)
cp "$out/start.png" "$review/zip-start.png" 2> /dev/null
if grep -q '^ZBROWSER READY' "$out/start.txt" && [ "$colours" -gt 100 ]; then
	echo "zip-wine: started from the extracted folder and drew start.html ($colours colours, $review/zip-start.png)"
else
	echo "zip-wine: the program from the zip did not draw its start page ($out/start.txt)"
	status=1
fi
sync
if [ "$status" = 0 ]; then echo "zip-wine: PASS"; else echo "zip-wine: FAIL"; fi
exit "$status"
