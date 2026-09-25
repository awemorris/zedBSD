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
