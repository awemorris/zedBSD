#### paste two files
printf 'a\nb\n' > f1; printf '1\n2\n' > f2; paste f1 f2

#### paste -d
printf 'a\nb\n' > f1; printf '1\n2\n' > f2; paste -d, f1 f2

#### paste -s
printf 'a\nb\nc\n' | paste -s -d, -

#### join on the first field
printf 'a 1\nb 2\n' > f1; printf 'a x\nb y\n' > f2; join f1 f2

#### comm
printf 'a\nb\nc\n' > f1; printf 'b\nc\nd\n' > f2; comm f1 f2

#### comm -12
printf 'a\nb\nc\n' > f1; printf 'b\nc\nd\n' > f2; comm -12 f1 f2

#### fold -w
printf 'abcdefghij\n' | fold -w 4

#### nl
printf 'a\n\nb\n' | nl

#### basename and dirname
basename /usr/lib/libz.so; basename /usr/lib/libz.so .so; dirname /usr/lib/libz.so; dirname libz.so; basename /; dirname /

#### basename with trailing slashes
basename /usr/lib/; dirname /usr/lib/

#### cat with several files and -
printf 'a\n' > f1; printf 'b\n' | cat f1 - f1

#### cat -u
printf 'x\n' | cat -u

#### tsort
printf 'a b\nb c\n' | tsort

#### split -l
printf '1\n2\n3\n' > in; split -l 2 in part; cat partaa; echo --; cat partab

#### paste with a file shorter than the others
printf 'a\nb\nc\n' > f1; printf '1\n' > f2; paste f1 f2 f1

#### paste -d list goes round
printf 'a\nb\n' > f1; printf '1\n2\n' > f2; paste -d ':,' f1 f2 f1

#### paste - twice reads standard input in turn
printf 'x\ny\nz\n' | paste - -

#### join -a1
printf 'a 1\nb 2\nc 3\n' > j1; printf 'a x\nc y\nd z\n' > j2; join -a1 j1 j2

#### join -v2
printf 'a 1\nb 2\n' > j1; printf 'a x\nd z\n' > j2; join -v2 j1 j2

#### join -o with -e and -a on both
printf 'a 1\nb 2\nc 3\n' > j1; printf 'a x\nc y\nd z\n' > j2; join -a1 -a2 -e NONE -o 0,1.2,2.2 j1 j2

#### join many to many
printf 'a 1\na 2\n' > j1; printf 'a x\na y\n' > j2; join j1 j2

#### join -t and other fields
printf 'k:1:2\nm:5\n' > j1; printf 'k:3\nm:6:7\n' > j2; join -t: j1 j2

#### join -1 and -2
printf '1 a\n2 b\n' > j1; printf 'x a\ny b\n' > j2; join -1 2 -2 2 j1 j2
