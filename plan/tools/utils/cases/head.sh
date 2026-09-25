#### default ten lines
seq_out() { i=1; while [ $i -le 15 ]; do echo $i; i=$((i+1)); done; }; seq_out | head

#### -n
printf '1\n2\n3\n4\n' | head -n 2

#### -n with a joined argument
printf '1\n2\n3\n4\n' | head -n2

#### obsolescent -N
printf '1\n2\n3\n4\n' | head -3

#### several files have headers
printf 'a\n' > f1; printf 'b\n' > f2; head -n 1 f1 f2

#### fewer lines than asked
printf '1\n' | head -n 5

#### -n 0
printf '1\n2\n' | head -n 0; echo end

#### last line without newline
printf 'a\nb' | head -n 5; echo

#### head -c counts bytes
printf 'abcdef\nghi\n' | head -c 4; echo; printf 'abcdef\n' | head -c3; echo
