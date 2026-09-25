# ws065: bash extensions that POSIX leaves unspecified (or calls syntax
# errors), which /bin/sh gives bash's meaning.  sh-diff.py scores this file
# against bash --posix, not dash.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

#### $'...' escapes
printf '%s\n' $'a\tb' | od -c | head -1
echo $'x\x41\101é\cA\e' | od -c | head -1
x=$'it'\''s'; echo "$x"
echo "$'not special in double quotes'"
echo $'q\'q' $'a\0b'x

#### [[ ]] strings, patterns and regular expressions
[[ abc == a* ]]; echo $?
[[ abc == "a*" ]]; echo $?
[[ abc != b* ]]; echo $?
[[ ab =~ ^a(b|c)$ ]]; echo $?
[[ a.c =~ "a.c" ]]; echo $?
[[ abc =~ "a.c" ]]; echo $?
r='^[0-9]+$'; [[ 123 =~ $r ]]; echo $?
[[ b > a && a < b ]]; echo $?

#### [[ ]] words are not split or globbed
x="a b"; [[ $x == "a b" ]] && echo nosplit
y='*'; [[ $y == '*' ]] && echo noglob
[[ -z "" && -n x ]] && echo zn

#### [[ ]] file tests, -v, -o and grouping
[[ -f /etc/passwd && -d /etc ]] && echo files
[[ ! -e /nonexistent ]] && echo missing
v=1; [[ -v v ]] && echo set; [[ -v nov ]] || echo unset
[[ -o errexit ]]; echo $?
[[ ( a == a || b == c ) && ! -z x ]]; echo $?
[[ -f -f ]]; echo $?
[[ -f == ]]; echo $?

#### [[ ]] integer comparisons evaluate arithmetic
[[ 1+1 -eq 2 ]] && echo arith
x=3; [[ x*2 -gt 5 ]] && echo gt

#### [[ ]] across lines, and errexit
[[ a == a &&
   b == b ]] && echo multiline
set -e
[[ 1 == 2 ]]
echo not-reached

#### (( )) status and assignment
((0)); echo $?
((2+3)); echo $?
((x=5)); echo $x
((x++)); echo $x
(( y = x * 2, y += 1 )); echo $y
((1/0)); echo st=$?

#### ++ and -- step a variable, and are signs before a number
x=3
echo $((1--1)) $((--5)) $((x+++1)) $x $((--x)) $((x---1)) $x

#### for (( ; ; ))
for ((i=0; i<3; i++)); do echo $i; done
for (( ; ; )); do echo once; break; done
for ((i=0; i<2; i++)) { echo b$i; }

#### function keyword
function f { echo F; }; f
function g() { echo G; }; g
function h
{
  echo H
}
h

#### (( that is nested subshells
( (echo nested) )
((echo nested2) )

#### |& sends standard error into the pipe
{ echo out; echo err >&2; } |& cat
{ echo out; echo err >&2; } |& tr a-z A-Z

#### <<< here-string
cat <<< "a b"
x=5; cat <<< $((x+1))
cat <<<word | wc -c

#### >& file sends both outputs to the file
{ echo o; echo e >&2; } >& both.txt; cat both.txt
echo a 2>&f4; echo st=$?

#### n>&m- moves a descriptor
exec 3>&1
echo moved 1>&3-
echo st=$?

#### <( ) and >( )
cat <(echo ps)
while read l; do echo "got $l"; done < <(printf 'a\nb\n')
cat <(echo one) <(echo two)
echo into > >(cat); sleep 1

#### ${name:offset:length} (ws065-p002)
v=abcdef
echo ${v:1:2} ${v:3} ${v: -2} ${v: -3:2} ${v:1:-2} "[${v:10}]" ${v:(-2)} ${v:1+1:1}
i=1; echo ${v:i:1} ${v:i+1} "${v:0:1}" "${v: 1}"
set -- a b c d
echo ${@:2}; echo ${@:2:2}; echo ${@: -1}; echo "${*:2:2}"
unset u; echo "[${u:1}]" ${u:-d} ${v:+p} ${w:=set} $w

#### ${name/pattern/string} and its forms (ws065-p002)
v=abc; echo "${v/b/[&]}" "${v/b/[\&]}" "${v/b/"&"}"
v=aXbXc; echo ${v//X/-} ${v/#a/S} ${v/%c/E} ${v/#/P} ${v/%/Q} ${v/} ${v//} ${v/X}
v=aaa; echo ${v//a*/X} ${v/a*/X} ${v//?/&&}
v=a/b/c; echo ${v//\//_} ${v//"/"/:}
v='x*y'; echo "${v/\*/S}" "${v/'*'/T}" "${v/*/U}"
p=b; v=abcb; echo ${v//$p/Z} ${v/"$p"/W} ${v//[ac]/.} ${v/[!a]/_}
set -- ab cb; echo ${@/b/X}; for w in "${@/b/X Y}"; do echo "<$w>"; done

#### ${name^} ${name^^} ${name,} ${name,,} (ws065-p002)
v=hello; echo ${v^} ${v^^} ${v,} ${v,,} ${v^^[el]}
V=HELLO; echo ${V,} ${V,,}
set -- ab cb; echo "${@^^}"

#### ${!name} (ws065-p002)
x=y; y=z; echo ${!x}
set -- p q; n=2; echo ${!n}
x=v; v=abc; echo ${!x:-d} ${!x/b/B}; x=u; echo "[${!x:-dflt}]"
(x=1bad; echo ${!x}) 2>/dev/null || echo bad
true & p=$!; [ "${!}" = "$p" ] && echo same; echo "[${!-x}]" | tr -d 0-9; wait

#### set -u and the new expansions (ws065-p002)
set -u; v=a; echo ${v:0} ${v/a/b}
(echo ${u:1}) 2>/dev/null || echo e1
(echo ${u/a/b}) 2>/dev/null || echo e2
(echo ${u^}) 2>/dev/null || echo e3
(x=u; echo ${!x}) 2>/dev/null || echo e4
