#### arithmetic
expr 2 + 3; expr 10 - 4; expr 3 \* 4; expr 17 / 5; expr 17 % 5

#### comparison
expr 3 \< 5; expr abc = abc; expr 2 \> 10; expr 2 != 3

#### string comparison when not numbers
expr b \> a; expr 10 \< 9

#### status 1 for null or zero
expr 0; echo "st=$?"; expr '' ; echo "st=$?"

#### or and and
expr 0 \| 5; expr 3 \& 0; expr '' \| ''; echo "st=$?"

#### match with colon
expr abcdef : 'abc'; expr abcdef : 'x'

#### match with a group
expr 'foo.c' : '\(.*\)\.c'

#### match anchored at the start
expr 'xabc' : 'abc'

#### parentheses
expr \( 2 + 3 \) \* 4

#### negative numbers
expr -5 + 2

#### length of a match with a class
expr 'abc123' : '[a-z]*'

#### syntax error is status 2
expr 1 + 2>/dev/null; echo "st=$?"; expr 1 2 2>/dev/null; echo "st=$?"

#### division by zero
expr 1 / 0 2>/dev/null; echo "st=$?"

#### configure style: basename by expr
expr "X/usr/lib/libz.so" : '.*/\([^/][^/]*\)/*$'

#### configure style: test for a prefix
expr "x--prefix=/usr" : 'x--prefix=\(.*\)'

#### configure style: length
expr "hello" : '.*'
