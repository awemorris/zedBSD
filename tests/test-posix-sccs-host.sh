#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-phase8.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/sccs/main.c" \
	"$repo/userland/base/common/sccs.c" -o "$work/sccs"
for command in admin delta get prs rmdel sact unget val what; do
	ln -s sccs "$work/$command"
done

cd "$work"
printf 'version %%I%% %%W%%\n' > initial
./admin -i initial -y created s.demo
./val s.demo
./get -p s.demo > revision1
grep -q 'version 1.1 @(#)demo' revision1
./get -e s.demo
grep -q '^1.1 1.2 ' p.demo
printf 'second %%I%% %%W%%\n' > demo
./delta -y second s.demo
./val s.demo
./prs -d ':I: :C:' s.demo > prs.out
grep -qx '1.2 second' prs.out
./get -p -r 1.1 s.demo > old.out
grep -q 'version 1.1' old.out
./get -p s.demo > new.out
grep -q 'second 1.2' new.out
./what new.out > what.out
grep -q 'demo.*1.2' what.out
./get -e s.demo
./sact s.demo > pending.out
grep -q '^1.2 1.3 ' pending.out
./unget s.demo
test ! -e p.demo
./sccs val s.demo
if ./rmdel -r 1.1 s.demo 2>/dev/null; then
	exit 1
fi
cp s.demo corrupt
chmod u+w corrupt
printf X >> corrupt
if ./val corrupt 2>/dev/null; then
	exit 1
fi

printf '%s\n' 'zedBSD POSIX Phase 8 SCCS host test: PASS'
