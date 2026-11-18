#!/bin/sh
# Regenerates browser's committed tables from the published lists they are made from
# (plan/ws074/design.md §17 D4: the generated tables are committed, so the base build needs no
# network).  Each list is downloaded into build/browser-lists/ (not the source tree), checked
# against the SHA-256 pinned here, and turned into its table by the generator next to this script:
#
#   html/entities-table.c        the WHATWG HTML Standard's named character references (CC BY 4.0)
#   base/unicode-case-table.c    the Unicode Character Database 16.0.0's case mappings (Unicode License V3)
#
# The notices of both licences are installed with the program (userland/base/licenses/browser/).
# Changing a list means changing its URL and SHA-256 here and committing the regenerated table.
#
#   sh userland/desktop/libbrowser/tools/regenerate.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../../.."
src=userland/desktop/libbrowser
lists=build/browser-lists
mkdir -p "$lists"

# fetch URL SHA256 FILE: downloads a list unless it is there already, and checks its SHA-256.
fetch() {
	if [ ! -f "$lists/$3" ]; then
		curl --fail --location --silent --show-error --output "$lists/$3.part" "$1"
		mv "$lists/$3.part" "$lists/$3"
	fi
	actual=$(sha256sum "$lists/$3" | cut -d' ' -f1)
	if [ "$actual" != "$2" ]; then
		echo "regenerate: $lists/$3 has SHA-256 $actual, not the pinned $2" >&2
		exit 1
	fi
}

fetch https://html.spec.whatwg.org/entities.json \
	d741d877ac77c4194c4ad526b5b4a19aef8dfe411ab840a466891cdbb9f362e6 entities.json
fetch https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt \
	ff58e5823bd095166564a006e47d111130813dcf8bf234ef79fa51a870edb48f UnicodeData-16.0.0.txt
fetch https://www.unicode.org/Public/16.0.0/ucd/SpecialCasing.txt \
	8d5de354eef79f2395a54c9c7dcebbaf3d30fc962d0f85611ea97aa973a0c451 SpecialCasing-16.0.0.txt

python3 "$src/tools/gen-entities.py" "$lists/entities.json" "$src/html/entities-table.c"
python3 "$src/tools/gen-unicode-case.py" "$lists/UnicodeData-16.0.0.txt" "$lists/SpecialCasing-16.0.0.txt" \
	"$src/base/unicode-case-table.c"
echo "regenerate: $src/html/entities-table.c and $src/base/unicode-case-table.c"
