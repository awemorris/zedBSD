#### -l from standard input
printf 'a\nb\nc\n' | wc -l

#### -w
printf 'one two  three\nfour\n' | wc -w

#### -c
printf 'abc\n' | wc -c

#### -m
printf 'abc\n' | wc -m

#### all counts from standard input
set -- $(printf 'a b\nc\n' | wc); echo "$@"

#### a file operand
printf 'a\nb\n' > f; wc -l f

#### several files and a total
printf 'a\n' > f1; printf 'b\nc\n' > f2; wc -l f1 f2

#### no trailing newline
printf 'abc' | wc -l

#### empty input
printf '' | wc -c

#### configure style: count lines into a variable
n=$(printf 'x\ny\n' | wc -l); echo "[$n]"; [ "$n" -eq 2 ] && echo two

#### words separated by tabs and newlines
printf 'a\tb\n\n c  d\n' | wc -w

#### empty file
: > e; wc e
