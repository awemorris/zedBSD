#!/bin/sh
# WS032: 取得済み tarball のライセンス機械監査。
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# 展開した全ファイルから GPL/LGPL の文言を検索し、見つかったものを列挙する。
# 既知・判定済みの件（plan/ws032/provenance.md §4.1）だけであれば 0、
# 未知のものが増えていれば非 0 で終わる。
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
dist="$root/build/distfiles"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

# 判定済みの既知ファイル（provenance.md §4.1）。
known='openssh-10.5p1/config.guess
openssh-10.5p1/config.sub
openssl-3.5.8/crypto/camellia/asm/cmll-x86.pl
openssl-3.5.8/crypto/camellia/asm/cmll-x86_64.pl
openssl-3.5.8/external/perl/Text-Template-1.56/LICENSE'

status=0
for archive in openssl-3.5.8.tar.gz openssh-10.5p1.tar.gz; do
	if test ! -f "$dist/$archive"; then
		echo "audit-licenses: missing archive: $dist/$archive" >&2
		exit 1
	fi
	tar -xzf "$dist/$archive" -C "$work"
done

found=$(cd "$work" && grep -rlE \
	'GNU General Public License|GNU Lesser General Public|SPDX-License-Identifier: *L?GPL' \
	. 2>/dev/null | sed 's|^\./||' | LC_ALL=C sort)

for file in $found; do
	case "
$known
" in
	*"
$file
"*) ;;
	*)
		echo "audit-licenses: UNKNOWN GPL-bearing file: $file" >&2
		status=1
		;;
	esac
done

count=$(printf '%s\n' "$found" | grep -c . || true)
echo "audit-licenses: $count GPL-bearing file(s), all known: $(test "$status" = 0 && echo yes || echo NO)"
exit "$status"
