#!/bin/sh
# WS032: 取得済み tarball のライセンス機械監査。
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# 展開した全ファイルから GPL/LGPL の文言を検索し、見つかったものを列挙する。
# 既知・判定済みの件（plan/ws032/provenance.md §4.1、ws129-p002 の表）だけであれば 0、
# 未知のものが増えていれば非 0 で終わる。
#
# ws129-p002（2026-10-04）: openssl・openssh だけでなく、distfiles の全ての tar の archive を見る。
# autotools・libtool の build の補助の file（config.guess・ltmain.sh など、GPL の例外付きで build の時だけ使い、
# image に入らない）は名前で既知とする。GPL・LGPL の package そのもの（GTK の依存の glib・gtk・pango など、
# image に入れるにはユーザーの判断が要る）は package ごとの判定として下の表に置く。
#   plan/tools/packages/audit-licenses.sh [DISTFILES]      （既定 build/distfiles。main の build/ を読み取り専用で渡してよい）
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
dist=${1:-$root/build/distfiles}
# 展開先は build/tmp の新しい directory（2026-10-06 ユーザー: 消すのは Q1 の plan/tools/q1-clean.sh）。
. "$root/plan/tools/fresh-out.sh"
fresh_out "$root/build/tmp/audit-licenses"
work=$fresh_dir

# 判定済みの既知ファイル（provenance.md §4.1）。
known='openssh-10.5p1/config.guess
openssh-10.5p1/config.sub
openssl-3.5.8/crypto/camellia/asm/cmll-x86.pl
openssl-3.5.8/crypto/camellia/asm/cmll-x86_64.pl
openssl-3.5.8/external/perl/Text-Template-1.56/LICENSE
fontconfig-2.17.1/ABOUT-NLS
freetype-2.14.3/builds/unix/ax_pthread.m4
freetype-2.14.3/builds/unix/pkg.m4
libffi-3.8.0/ChangeLog.old
libffi-3.8.0/LICENSE-BUILDTOOLS
libffi-3.8.0/libtool-ldflags
libffi-3.8.0/msvcc.sh
zlib-1.3.2/FAQ
Python-3.14.8/Doc/license.rst
Python-3.14.8/LICENSE
Python-3.14.8/Mac/BuildScript/resources/License.rtf
Python-3.14.8/README.rst'
# ws126-p001 の判定（2026-10-09 P1）: Python の 4 件は PSF の license の文と README で、「GPL と両立する」「GPL の code を
# 含まない」と述べるだけ（GPL の code ではない）。
# ws129-p002 の判定（2026-10-04）: 上の 8 件は文書（ABOUT-NLS・ChangeLog.old・FAQ）と build の補助（m4・libtool の
# 補助・MSVC の wrapper、LICENSE-BUILDTOOLS はそれらの license）で、image の binary に入らない。

# GPL・LGPL の package（全体を一つの判定とする archive の上の directory、ws129-p002）。image に入らない（CI の
# config は選ばない）か、入れるならユーザーの判断。REmacs の archive は 2026-10-04 から取得しない（作者が zlib にして
# userland/base/emacs に取り込み、ime-dict-ja の辞書も userland/desktop/ime/dict に写した）。
# FFmpeg（2026-10-09 ws129-p002）: ベータ1 からユーザーの判断で image に入る（WS122）。LGPL 2.1 or later として build し
# （--disable-gpl --disable-nonfree、version3 なし。build の config.h は FFMPEG_LICENSE "LGPL version 2.1 or later"、
# CONFIG_GPL 0）、GPL だけの部分（postproc・GPL の filter など）は build されない。archive の中の GPL の文言は package の判定とする。
packages='gtk-4.18.6 glib-2.84.4 pango-1.56.4 cairo-1.18.6 fribidi-1.0.17 gdk-pixbuf-2.44.8 gperf-3.3 ffmpeg-9.0.2'

# build の時だけ使う autotools・libtool・GNU の補助の file の名前（どの archive でも）。
helpers='config.guess config.sub ltmain.sh libtool.m4 ltoptions.m4 ltsugar.m4 ltversion.m4 lt~obsolete.m4 compile
depcomp install-sh missing ar-lib test-driver configure aclocal.m4 mkinstalldirs py-compile ylwrap texinfo.tex'

# build の時だけ使う、または image に入らない directory（どの archive でも）: autoconf の m4、試験、contrib、文書、例。
dirs='m4 test tests testsuite contrib doc docs build-aux conftools po examples example fuzz fuzzing'

status=0
count=0
for archive in "$dist"/*.tar.gz "$dist"/*.tar.xz "$dist"/*.tar.bz2; do
	test -f "$archive" || continue
	tar -xf "$archive" -C "$work"
done

found=$(cd "$work" && grep -rlE \
	'GNU General Public License|GNU Lesser General Public|SPDX-License-Identifier: *L?GPL' \
	. 2>/dev/null | sed 's|^\./||' | LC_ALL=C sort)

for file in $found; do
	count=$((count + 1))
	top=${file%%/*}
	base=${file##*/}
	case "
$known
" in
	*"
$file
"*) continue ;;
	esac
	case " $(echo $packages) " in
	*" $top "*) continue ;;
	esac
	case " $(echo $helpers) " in
	*" $base "*) continue ;;
	esac
	skip=0
	for dir in $dirs; do
		case "/$file" in
		*"/$dir/"*) skip=1 ;;
		esac
	done
	test "$skip" = 1 && continue
	echo "audit-licenses: UNKNOWN GPL-bearing file: $file" >&2
	status=1
done

echo "audit-licenses: $count GPL-bearing file(s), all known: $(test "$status" = 0 && echo yes || echo NO)"
exit "$status"
