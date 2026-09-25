#### default
printf 'a\na\nb\na\n' | uniq

#### -c counts
printf 'a\na\nb\n' | uniq -c

#### -d only repeated
printf 'a\na\nb\nc\nc\n' | uniq -d

#### -u only unique
printf 'a\na\nb\nc\nc\n' | uniq -u

#### -f skips fields
printf 'x a\ny a\nz b\n' | uniq -f 1

#### -s skips characters
printf 'xa\nya\nzb\n' | uniq -s 1

#### input and output files
printf 'a\na\n' > in; uniq in out; cat out

#### -c with -d
printf 'a\na\nb\nc\nc\nc\n' | uniq -cd

#### -f and -s together
printf 'x  ab\ny ac\nz ad\n' | uniq -f 1 -s 1

#### empty lines are a group
printf '\n\na\n' | uniq -c

#### last line without a newline
printf 'a\na' | uniq
