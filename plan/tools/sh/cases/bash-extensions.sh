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
diff <(echo 1) <(echo 1); echo same $?; diff <(echo 1) <(echo 2) >/dev/null; echo differ $?
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

#### let (ws065-p003)
let a=1+2 b=a*2; echo $a $b $?; let 0; echo $?; let "c = 5"; echo $c
let 2>/dev/null; echo st$?; let "1 > 2"; echo $?; (let 1+) 2>/dev/null; echo st$?

#### test and [ with == (ws065-p003)
[ a == a ] && echo eq; test a == b || echo ne; [ 1 == 01 ] || echo strcmp

#### declare and typeset (ws065-p003)
declare x=1; declare -p x; declare -x y=2; declare -p y; declare -r z=3; declare -p z
declare -i n=1+2; echo $n; n=4+5; echo $n; declare -p n
f() { declare l=loc; declare -g g=glob; typeset t=tt; echo in:$l:$t; }; f; echo out:${l-unset}:$g:${t-unset}
declare -p nonexist 2>/dev/null; echo st$?
v=$(printf 'a\nb'); declare -p v; q="it's"; declare -p q; w="a\"b\$c"; declare -p w
declare -l lo=HeLLo; echo $lo; lo=ABC; echo $lo; declare -u up=abc; declare -p up lo
declare dx; declare -p dx; declare -x ex; declare -p ex; declare +x ex; declare -p ex
declare -r ro=1; declare ro=2 2>/dev/null; echo st$?
declare -i i=5; declare +i i; i=1+1; echo $i
declare -i k=3; abc=7; k=abc; echo k=$k; k="2 * 3"; echo $k; export k; readonly k; declare -p k

#### declare -f and -F (ws065-p003)
f() { :; }; g() { echo g; }
declare -F; declare -F f; declare -F nofunc; echo st$?; declare -f nofunc; echo st$?
declare -f g >/dev/null && echo has-g

#### local with attributes (ws065-p003)
g2() { local -i m=2+2; local -r r=1; echo $m; declare -p r; }; g2
h() { local x=inner; declare -p x; }; h

#### printf -v (ws065-p003)
printf -v out '%s-%d|%5s|%c' x 5 ab q; echo "[$out]"
printf -vy '%s,' a b c; echo $y
printf -v 1bad x 2>/dev/null; echo st$?
printf -v e ''; echo "[${e-unset}]"; printf -v z -- '-%s' q; echo $z

#### builtin (ws065-p003)
echo() { printf 'fn\n'; }; builtin echo real; echo; builtin nosuch 2>/dev/null; builtin echo st$?

#### source and . with operands (ws065-p003)
printf 'echo in:$#:$1\nset -- z\n' > inc.sh; printf 'shift; echo sh:$#:$1\n' > sh1.sh
set -- a b c; . ./sh1.sh x y; echo after:$#:$1
f() { . ./inc.sh q; echo f:$#:$1; }; f m n; echo top:$#:$1
source ./inc.sh s1 s2; echo after:$#:$1

#### pushd, popd and dirs (ws065-p003)
mkdir -p d1 d2 d3; cd d1; HOME=$(dirname "$PWD")
pushd ../d2 >/dev/null; echo st$? ${PWD##*/}; pushd ../d3; dirs; dirs -v; dirs -p
pushd; echo ${PWD##*/}; pushd +1; popd; echo ${PWD##*/}; popd +1; dirs; popd 2>/dev/null; echo st$?
pushd -n ../d3; dirs; dirs -c; dirs; pushd /nonexist 2>/dev/null; echo st$?
pushd ../d2 >/dev/null; pushd ../d3 >/dev/null; dirs +1; dirs -0; pushd -1; popd -0; popd -n; dirs
pushd +5 2>/dev/null; echo st$?; dirs -l | sed "s,$HOME,H,g"

#### printf %q (ws065-p003)
printf '[%q] [%q] [%q] [%q] [%q]\n' '' '~a' '#a' 'a b' "$(printf 'a\tb')"
printf '%q ' 'x!x' 'x"x' 'x$x' "x'x" 'x(x' 'x*x' 'x,x' 'x;x' 'x<x' 'x?x' 'x[x' 'x\x' 'x^x' 'x`x' 'x{x' 'x|x' 'x~x' 'a=b' 'x#x'; echo
printf '[%8q][%-6q]\n' ab cd
printf -v x %q "it's \$HOME"; eval "y=$x"; echo "$y"

#### builtin before command and export (ws065-p003)
builtin command export bc=export; echo bc=$bc
command builtin readonly cb=readonly; echo cb=$cb
v="a b"; builtin export w=$v; echo $w
builtin() { echo redefined "$@"; }; builtin echo q
