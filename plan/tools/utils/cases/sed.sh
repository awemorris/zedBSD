#### s with default output
printf 'hello world\nfoo bar\n' | sed 's/o/0/'

#### s with g flag
printf 'hello world\nfoo bar\n' | sed 's/o/0/g'

#### s with a number flag
printf 'aaaa\n' | sed 's/a/b/3'

#### s with number and g
printf 'aaaaa\n' | sed 's/a/b/2g'

#### s with p flag and -n
printf 'one\ntwo\nthree\n' | sed -n 's/t/T/p'

#### s with w flag
printf 'one\ntwo\n' | sed 's/o/0/w out'; cat out

#### ampersand in replacement
printf 'abc\n' | sed 's/b/[&]/'

#### escaped ampersand
printf 'abc\n' | sed 's/b/\&/'

#### back-references
printf 'john smith\n' | sed 's/\([a-z]*\) \([a-z]*\)/\2, \1/'

#### newline in replacement
printf 'a,b,c\n' | sed 's/,/\
/g'

#### other delimiter
printf '/usr/local/bin\n' | sed 's|/usr/local|/opt|'

#### escaped delimiter in regex
printf 'a/b/c\n' | sed 's/\//:/g'

#### empty regex reuses the last one
printf 'foo bar foo\n' | sed '/foo/s//X/g'

#### -n with p
printf 'a\nb\nc\n' | sed -n '2p'

#### line number address
printf 'a\nb\nc\nd\n' | sed '2d'

#### last line address
printf 'a\nb\nc\n' | sed '$d'

#### range of lines
printf '1\n2\n3\n4\n5\n' | sed -n '2,4p'

#### range to last line
printf '1\n2\n3\n4\n' | sed -n '3,$p'

#### regex address
printf 'apple\nbanana\ncherry\n' | sed -n '/an/p'

#### regex range
printf 'a\nstart\nb\nend\nc\n' | sed -n '/start/,/end/p'

#### range whose end is before its start
printf '1\n2\n3\n4\n' | sed -n '3,1p'

#### negated address
printf 'a\nb\nc\n' | sed -n '2!p'

#### negated range
printf '1\n2\n3\n4\n5\n' | sed '2,4!d'

#### custom regex delimiter in address
printf 'a/b\nc\n' | sed -n '\,a/b,p'

#### several -e
printf 'abc\n' | sed -e 's/a/A/' -e 's/c/C/'

#### semicolons between commands
printf 'abc\n' | sed 's/a/A/;s/b/B/'

#### -f script file
printf 's/x/y/\n2d\n' > script; printf 'x1\nx2\nx3\n' | sed -f script

#### -e and -f together
printf 's/2/two/\n' > script; printf '1\n2\n' | sed -e 's/1/one/' -f script

#### #n on the first line of the script
printf '#n\n/b/p\n' > script; printf 'a\nb\n' | sed -f script

#### comment in a script
printf 'a\nb\n' | sed '# comment
s/a/A/'

#### braces group commands
printf '1\n2\n3\n' | sed -n '2{p;p;}'

#### nested braces with addresses
printf '1\n2\n3\n4\n' | sed -n '2,4{/3/!p;}'

#### a append text
printf 'a\nb\n' | sed '1a\
appended'

#### i insert text
printf 'a\nb\n' | sed '2i\
inserted'

#### c change text
printf 'a\nb\nc\n' | sed '2c\
changed'

#### c on a range prints once at the end
printf 'a\nb\nc\nd\n' | sed '2,3c\
changed'

#### multiline a text
printf 'x\n' | sed 'a\
line1\
line2'

#### = prints the line number
printf 'a\nb\n' | sed -n '$='

#### y transliterates
printf 'hello\n' | sed 'y/abcdefghij/ABCDEFGHIJ/'

#### q quits after printing
printf '1\n2\n3\n' | sed '2q'

#### n next line
printf '1\n2\n3\n4\n' | sed -n 'n;p'

#### N appends the next line
printf '1\n2\n3\n4\n' | sed 'N;s/\n/-/'

#### N on the last line prints the pattern space
printf '1\n2\n3\n' | sed 'N;s/\n/-/'

#### D deletes the first line and restarts
printf '1\n2\n3\n' | sed -n 'N;P;D'

#### P prints the first line
printf 'a\nb\n' | sed -n 'N;P'

#### h and g
printf 'first\nsecond\n' | sed -n '1h;2{g;p;}'

#### H and x
printf 'a\nb\nc\n' | sed -n 'H;${x;s/\n/,/g;p;}'

#### G appends the hold space
printf 'a\nb\n' | sed 'G'

#### reverse lines with the hold space
printf '1\n2\n3\n' | sed -n '1!G;h;$p'

#### b branches to a label
printf 'a\nb\n' | sed ':top
s/^a/x/
/^x/b end
s/$/!/
:end'

#### b with no label ends the cycle
printf 'a\nb\n' | sed '/a/b
s/$/!/'

#### t branches after a substitution
printf 'aaa\nbbb\n' | sed ':l
s/a/x/
t l'

#### t resets on a new cycle
printf 'ab\ncd\n' | sed -n 's/a/A/;t yes
p;b
:yes
s/$/ (changed)/p'

#### r reads a file
printf 'inserted\n' > f; printf 'a\nb\n' | sed '1r f'

#### r with a missing file
printf 'a\n' | sed 'r nosuchfile'

#### w writes lines to a file
printf 'a\nb\nc\n' | sed -n '/b/w out'; cat out

#### l shows unprintable characters
printf 'a\tb\\c\001\n' | sed -n 'l'

#### BRE star and anchors
printf 'aaab\nb\nxab\n' | sed -n '/^a*b$/p'

#### BRE interval
printf 'ab\naab\naaab\n' | sed -n '/^a\{2,3\}b/p'

#### bracket expression and classes
printf 'a1\nb2\nc-\n' | sed 's/[[:digit:]]/#/'

#### negated bracket expression
printf 'abc123\n' | sed 's/[^0-9]//g'

#### back-reference in the regex
printf 'abab\nabcd\n' | sed -n '/\(ab\)\1/p'

#### dot matches any character
printf 'abc\n' | sed 's/a.c/X/'

#### ERE with -E
printf 'aaa bbb\n' | sed -E 's/(a+) (b+)/\2 \1/'

#### ERE alternation
printf 'cat\ndog\nbird\n' | sed -E -n '/cat|dog/p'

#### several files
printf '1\n2\n' > f1; printf '3\n4\n' > f2; sed -n '$p' f1 f2

#### line numbers continue across files
printf 'a\n' > f1; printf 'b\n' > f2; sed -n '2p' f1 f2

#### - is standard input
printf 'in\n' | sed 's/in/out/' -

#### missing input file
## skip-status
sed p nosuchfile; echo "status nonzero: $([ $? -ne 0 ] && echo yes)"

#### last line without a newline
printf 'a\nb' | sed 's/b/B/'

#### empty input
printf '' | sed 's/a/b/'; echo end

#### s on an empty match
printf 'abc\n' | sed 's/x*/-/g'

#### s with the i flag is not POSIX but anchors are
printf 'abc\nabc\n' | sed '2s/^/> /'

#### address 0 is not valid
## skip-status
printf 'a\n' | sed -n '0p' 2>/dev/null; echo "st=$([ $? -ne 0 ] && echo bad)"

#### configure style: substitute several variables
printf '@prefix@/lib @VERSION@\n' | sed -e 's,@prefix@,/usr,g' -e 's,@VERSION@,1.0,g'

#### configure style: quote special characters
printf "it's a \$var\n" | sed "s/'/'\\\\''/g"

#### configure style: print lines between markers
printf 'x\n#START\nkeep\n#END\ny\n' | sed -n '/^#START/,/^#END/{/^#/!p;}'

#### configure style: strip trailing slash
echo /usr/local/ | sed 's,/*$,,'

#### configure style: extract a version
echo 'gcc (GCC) 12.2.0' | sed -n 's/^.* \([0-9][0-9]*\.[0-9.]*\).*$/\1/p'
