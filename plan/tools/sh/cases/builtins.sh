#### export -p lists exported variables as commands
export ZZA=1 ZZB='a b' ZZC="it's"
export ZZD
export -p | grep ZZ

#### readonly -p
readonly ZZR=x
readonly -p | grep ZZR

#### unset of a function and a variable of the same name
f=1; f() { echo func; }
unset f; echo "[$f]"; f
unset -f f; f 2>/dev/null; echo st=$?

#### read with IFS and backslashes
printf 'a b\\ c d\n' | { read x y; echo "[$x][$y]"; }
printf 'a b\\ c d\n' | { read -r x y; echo "[$x][$y]"; }
printf '  lead  trail  \n' | { read x; echo "[$x]"; }
printf 'a:b::c\n' | { IFS=: read x y z w; echo "[$x][$y][$z][$w]"; }
printf 'line1\\\nline2\n' | { read x; echo "[$x]"; }

#### read at end of input without newline
printf 'partial' | { read x; echo "st=$? [$x]"; }

#### read into several names from a short line
echo one | { read a b c; echo "[$a][$b][$c]"; }

#### shift and set --
set -- a b c d
shift 2; echo "$# $*"
set -- ; echo "$#"
set -- "x y" z; for i; do echo "<$i>"; done

#### set -u errors on unset variables
( set -u; echo ${UNSETVAR_ZZ-default}; echo $UNSETVAR_ZZ; echo notreached ) 2>/dev/null; echo st=$?

#### set -f disables globbing
touch g1 g2; set -f; echo g*; set +f; echo g*

#### set -C noclobber
echo a > nc; set -C; ( echo b > nc ) 2>/dev/null; echo st=$?; echo c >| nc; cat nc; set +C

#### set -a allexport
set -a; AEX=1; set +a; env | grep '^AEX='

#### set -n parses without running
sh_n=$(echo 'echo ran' | $SH -n; echo st=$?); echo "$sh_n"

#### eval with multiple arguments
eval 'x=1;' 'y=2'; echo $x$y
eval "echo \$x"

#### dot script sees positional parameters of the caller
echo 'echo "dot: $# $1"' > dotfile
set -- p1 p2; . ./dotfile

#### dot script with return
printf 'echo before\nreturn 3\necho after\n' > dotret
. ./dotret; echo st=$?

#### command -v for different kinds
f() { :; }; alias al='echo'
command -v f; command -v cd; command -v al; command -v if; command -v nonexistent_zz; echo st=$?

#### type for different kinds
f() { :; }
type f | sed 's/function.*/function/'; type cd; type if; type :

#### hash -r and hash
hash -r; hash sed 2>/dev/null; echo st=$?

#### times prints two lines
times | wc -l | tr -d ' '

#### umask printing
umask 022; umask; umask -S; umask 0077; umask -S

#### wait for a specific pid and status
(exit 7) & p=$!; wait $p; echo st=$?
(exit 3) & (exit 4) & wait; echo st=$?

#### trap on EXIT in a subshell
( trap 'echo sub-exit' EXIT; echo in-sub ); echo after

#### trap listing
trap 'echo x' INT; trap 'echo y' EXIT; trap; trap - INT EXIT; trap

#### trap in function, signal to self
trap 'echo got USR1' USR1; kill -USR1 $$; echo after-kill

#### kill -l names
kill -l | head -n 3
kill -l 15

#### getopts in a function with OPTIND local
f() { local OPTIND=1 o; while getopts ab: o; do echo "$o:${OPTARG-}"; done; shift $((OPTIND-1)); echo "rest=$*"; }
f -a -b val x y
f -ba val z

#### printf reuse and conversions
printf '%s-%d\n' a 1 b 2 c
printf '%5.2f|%-4s|%04d|%x|%o|%c\n' 3.14159 ab 7 255 8 xyz
printf '%b\n' 'a\tb\\n' 'c\0101'
printf 'no newline'; echo

#### test file operators
touch tf; mkdir -p td; ln -sf tf tl
[ -f tf ] && [ -d td ] && [ -L tl ] && [ -e tl ] && [ ! -e nothere ] && echo ok
[ tf -ef tl ] && echo same
[ -s tf ] || echo empty

#### test string and integer comparisons
[ abc \< abd ] && echo lt; [ 10 -gt 9 ] && echo gt; [ " 5 " -eq 5 ] && echo spaces
[ -n "" ] || echo nempty; [ -z "" ] && echo zempty

#### cd and pwd
mkdir -p d1/d2; cd d1/d2; pwd | sed "s|$TMP||"; cd ..; pwd | sed "s|$TMP||"; cd - | sed "s|$TMP||"; cd "$TMP"

#### alias listing
alias a1='echo one' a2="echo 'two'"; alias a1; alias | grep '^a[12]'

#### exec redirection persists
exec 3>efile; echo to3 >&3; exec 3>&-; cat efile

#### return value of a pipeline and !
false | true; echo $?; true | false; echo $?; ! false; echo $?

#### local variables are restored
x=global; f() { local x=inner; echo $x; }; f; echo $x

#### break and continue with levels
for i in 1 2 3; do for j in a b c; do [ $j = b ] && continue 2; echo $i$j; done; done
for i in 1 2 3; do while :; do break 2; done; echo no; done; echo after

#### case with patterns
for w in abc x.c '[x]' ''; do case $w in a*) echo A;; *.c) echo C;; \[*) echo B;; '') echo E;; esac; done

#### arithmetic
echo $(( 7/2 )) $(( -7/2 )) $(( 7%3 )) $(( 2**3 )) 2>/dev/null
i=5; echo $(( i++ + ++i )) $i 2>/dev/null; echo $(( 1 ? 2 : 3 )) $(( 0x10 )) $(( 010 ))

#### here-document varieties
cat <<EOF
a $((1+1)) \$x `echo b`
EOF
cat <<'EOF'
a $((1+1)) \$x
EOF
cat <<-EOF
	tabbed
	EOF

#### command substitution nesting and quoting
echo "$(echo "$(echo "a b")")"; echo `echo \`echo nested\``

#### IFS splitting
IFS=:; x='a::b:'; set -- $x; echo $#; IFS=' '; y='  a  b  '; set -- $y; echo $#

#### positional parameters past 9
set -- 1 2 3 4 5 6 7 8 9 10 11; echo ${10} ${11} $10
