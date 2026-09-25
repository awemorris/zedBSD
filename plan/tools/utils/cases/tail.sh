#### default ten lines
i=1; while [ $i -le 15 ]; do echo $i; i=$((i+1)); done | tail

#### -n
printf '1\n2\n3\n4\n' | tail -n 2

#### -n +N from line N
printf '1\n2\n3\n4\n' | tail -n +3

#### -c bytes
printf 'abcdef\n' | tail -c 3

#### obsolescent -N
printf '1\n2\n3\n4\n' | tail -2

#### file operand
printf 'a\nb\nc\n' > f; tail -n 1 f

#### fewer lines than asked
printf '1\n' | tail -n 5

#### last line without newline
printf 'a\nb' | tail -n 1; echo

#### -n +1 is everything
printf 'a\nb\n' | tail -n +1

#### -c +2
printf 'abcdef' | tail -c +2; echo

#### empty input
printf '' | tail -n 3; echo end
