#### -d and -f
printf 'a:b:c\n' | cut -d: -f2

#### several fields
printf 'a:b:c:d\n' | cut -d: -f1,3

#### field range
printf 'a:b:c:d\n' | cut -d: -f2-3

#### open ranges
printf 'a:b:c:d\n' | cut -d: -f-2; printf 'a:b:c:d\n' | cut -d: -f3-

#### default delimiter is tab
printf 'a\tb\tc\n' | cut -f2

#### line without the delimiter is printed whole
printf 'nodelim\na:b\n' | cut -d: -f2

#### -s suppresses lines without the delimiter
printf 'nodelim\na:b\n' | cut -s -d: -f2

#### -c characters
printf 'abcdef\n' | cut -c2-4

#### -c list
printf 'abcdef\n' | cut -c1,3,5

#### -b bytes
printf 'abcdef\n' | cut -b-3

#### field past the end
printf 'a:b\n' | cut -d: -f5

#### option argument joined
printf 'a b c\n' | cut -d ' ' -f 2

#### files as operands
printf 'x:y\n' > f; cut -d: -f1 f

#### overlapping and out of order lists
printf 'abcdef\n' | cut -c5,1-2,2

#### configure style: version field
echo 'Linux 6.1.0 x86_64' | cut -d' ' -f2 | cut -d. -f1

#### empty lines and -f
printf 'a:b\n\nc\n' | cut -d: -f2

#### empty lines with -s
printf 'a:b\n\nc\n' | cut -s -d: -f1

#### delimiter at the end of a line
printf 'a:b:\n' | cut -d: -f2-
