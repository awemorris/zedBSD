#### default sort
printf 'c\na\nb\n' | sort

#### -r
printf 'c\na\nb\n' | sort -r

#### -n numeric
printf '10\n9\n100\n-1\n' | sort -n

#### -u unique
printf 'b\na\nb\na\n' | sort -u

#### -k field
printf 'x 3\ny 1\nz 2\n' | sort -k2

#### -k with -n
printf 'a 10\nb 9\nc 100\n' | sort -k2 -n

#### -k with end field and n modifier
printf 'a 10 x\nb 9 y\n' | sort -k2,2n

#### -t separator
printf 'a:3\nb:1\nc:2\n' | sort -t: -k2

#### -f folds case
printf 'b\nA\na\nB\n' | sort -f

#### -b ignores leading blanks
printf '  b\na\n c\n' | sort -b

#### -c checks order
printf 'a\nb\n' | sort -c; echo "st=$?"

#### -c on unsorted input
printf 'b\na\n' | sort -c 2>/dev/null; echo "st=$?"

#### -o output file
printf 'b\na\n' > in; sort -o in in; cat in

#### -m merges sorted files
printf 'a\nc\n' > f1; printf 'b\nd\n' > f2; sort -m f1 f2

#### several files
printf 'b\n' > f1; printf 'a\n' > f2; sort f1 f2

#### stable ties are broken by the whole line
printf 'a 1\nb 1\na 0\n' | sort -k2,2

#### -n with non-numbers
printf 'x\n2\n1\n' | sort -n

#### -r with -n
printf '1\n3\n2\n' | sort -rn

#### -k with character positions
printf 'xab\nyaa\n' | sort -k1.2

#### empty lines
printf 'b\n\na\n' | sort

#### -u with a key keeps the first of equal keys
printf 'b 1\na 1\nc 2\n' | sort -u -k2,2

#### -n with decimals, negatives and zeros
printf '1.5\n-0\n0\n-2\n1.25\n.5\n10\n' | sort -n

#### several keys
printf 'b 2\na 2\nc 1\n' | sort -k2,2n -k1,1r

#### -r on one key only
printf 'a 1\nb 2\nc 3\n' | sort -k2,2nr

#### -d dictionary order
printf 'a-c\nab\na c\n' | sort -d

#### -t with an end character
printf 'x:abc\ny:abd\nz:aba\n' | sort -t: -k2.1,2.2 -k2.3,2.3r

#### -o onto one of the inputs with several files
printf 'c\n' > f1; printf 'a\nb\n' > f2; sort -o f1 f1 f2; cat f1

#### -c status with -u
printf 'a\na\n' | sort -cu 2>/dev/null; echo "st=$?"
