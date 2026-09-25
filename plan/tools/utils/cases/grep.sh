#### simple match
printf 'apple\nbanana\ncherry\n' | grep an

#### no match is status 1
printf 'apple\n' | grep zzz; echo "st=$?"

#### match is status 0
printf 'apple\n' | grep -q app; echo "st=$?"

#### missing file is status 2
grep x nosuchfile 2>/dev/null; echo "st=$?"

#### -s silences a missing file
grep -s x nosuchfile; echo "st=$?"

#### -v inverts
printf 'a\nb\nc\n' | grep -v b

#### -c counts
printf 'a\nb\na\n' | grep -c a

#### -c with -v
printf 'a\nb\na\n' | grep -vc a

#### -n numbers lines
printf 'x\ny\nx\n' | grep -n x

#### -i ignores case
printf 'Hello\nhello\nHELLO\nbye\n' | grep -i hello

#### -x matches whole lines
printf 'ab\nabc\n' | grep -x ab

#### -l lists files
printf 'a\n' > f1; printf 'b\n' > f2; printf 'a\n' > f3; grep -l a f1 f2 f3

#### several files prefix names
printf 'a1\n' > f1; printf 'a2\n' > f2; grep a f1 f2

#### -c with several files
printf 'a\na\n' > f1; printf 'b\n' > f2; grep -c a f1 f2

#### -n with several files
printf 'q\na\n' > f1; printf 'a\n' > f2; grep -n a f1 f2

#### -e several patterns
printf 'a\nb\nc\n' | grep -e a -e c

#### newline separates patterns
printf 'a\nb\nc\n' | grep 'a
c'

#### -f pattern file
printf 'b\nc\n' > pats; printf 'a\nb\nc\n' | grep -f pats

#### -F fixed strings
printf 'a.c\nabc\n' | grep -F 'a.c'

#### -F with several strings
printf 'x*y\nxy\nz\n' | grep -F -e 'x*y' -e z

#### BRE dot and star
printf 'ac\nabc\nabbc\n' | grep 'ab*c'

#### BRE anchors
printf 'start\nrestart\n' | grep '^start'

#### BRE end anchor
printf 'end\nending\n' | grep 'end$'

#### BRE interval
printf 'ab\naab\naaab\n' | grep 'a\{2\}b'

#### BRE group and back-reference
printf 'abab\nabcd\n' | grep '\(ab\)\1'

#### BRE literal plus and question mark
printf 'a+b\nab\n' | grep 'a+b'

#### ERE plus
printf 'ab\naab\nb\n' | grep -E 'a+b'

#### ERE alternation and groups
printf 'abc\naxc\nadc\n' | grep -E 'a(b|x)c'

#### ERE question mark
printf 'color\ncolour\n' | grep -E 'colou?r'

#### ERE interval
printf 'x\nxx\nxxx\n' | grep -Ex 'x{2,3}'

#### bracket classes
printf 'a1\nbb\n2c\n' | grep '[[:digit:]]'

#### negated bracket
printf 'abc\n123\n' | grep '^[^0-9]*$'

#### bracket with a right bracket first
printf 'a]b\nab\n' | grep '[]]'

#### empty pattern matches every line
printf 'a\n\nb\n' | grep -c ''

#### -- ends options
printf -- '-x\ny\n' | grep -- -x

#### -q stops with status 0 even with errors after
printf 'a\n' > f1; grep -q a f1 nosuchfile 2>/dev/null; echo "st=$?"

#### -l with standard input
printf 'a\n' | grep -l a

#### -h is not POSIX but -c with one file has no name
printf 'a\n' > f1; grep -c a f1

#### configure style: check for a define
printf '#define HAVE_X 1\n/* #undef HAVE_Y */\n' | grep '^#define HAVE_'

#### configure style: grep -v comment lines
printf '# c\nx=1\n  # d\ny=2\n' | grep -v '^[ 	]*#'

#### configure style: word boundary by hand
printf 'gcc\ngcc-12\nxgcc\n' | grep '^gcc$'

#### -f with an empty file matches nothing
: > empty; printf 'a\nb\n' | grep -f empty; echo "st=$?"

#### -e with an empty pattern matches everything
printf 'a\n\nb\n' | grep -c -e ''

#### -x -F -i
printf 'Abc\nabcd\nABC\n' | grep -xFi abc

#### -v with -c, -l and -n
printf 'a\nb\na\n' > f; grep -vc a f; grep -vl a f; grep -vn a f

#### BRE star at the start is literal
printf '*a\nba\n' | grep '*a'

#### escaped dot
printf 'a.b\naxb\n' | grep 'a\.b'

#### -i with a bracket expression
printf 'X1\nx2\ny3\n' | grep -i '^[x]'

#### -x with several patterns
printf 'ab\nabc\nc\n' | grep -x -e ab -e c

#### -q with several files and a match in the second
printf 'a\n' > f1; printf 'b\n' > f2; grep -q b f1 f2; echo "st=$?"

#### last line without a newline
printf 'a\nbz' | grep z

#### -n with several files
printf 'x\ny\n' > f1; printf 'y\n' > f2; grep -n y f1 f2
