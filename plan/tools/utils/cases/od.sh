#### default octal words
printf 'ab' | od

#### -c characters
printf 'a\tb\n' | od -c

#### -x hex
printf 'abcd' | od -x

#### -t x1
printf 'AB' | od -t x1

#### -A n with -t x1
printf 'AB' | od -A n -t x1

#### -A d addresses
printf 'abcdefghijklmnopq' | od -A d -c

#### -t d1
printf '\377\001' | od -t d1

#### -N count
printf 'abcdef' | od -N 2 -c

#### -j skip
printf 'abcdef' | od -j 4 -c

#### -b
printf 'A' | od -b

#### repeated lines are starred
printf '%064d' 0 | od -c
