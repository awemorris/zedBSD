#### translate characters
printf 'hello\n' | tr 'el' 'ip'

#### ranges
printf 'Hello World\n' | tr 'a-z' 'A-Z'

#### classes
printf 'Hello World\n' | tr '[:upper:]' '[:lower:]'

#### -d deletes
printf 'a1b2c3\n' | tr -d '0-9'

#### -s squeezes
printf 'aaabbbccc\n' | tr -s 'ab'

#### -d with a class
printf 'a b\tc\n' | tr -d '[:space:]'; echo

#### -c complement with -d
printf 'abc123\n' | tr -cd '0-9'; echo

#### escapes
printf 'a b c\n' | tr ' ' '\n'

#### octal escape
printf 'a:b\n' | tr ':' '\011'

#### shorter second string repeats its last character
printf 'abcd\n' | tr 'abcd' 'xy'

#### -s with translation
printf 'aabbcc\n' | tr -s 'abc' 'xyz'

#### repeat notation
printf 'abc\n' | tr 'abc' '[x*]'

#### configure style: make an identifier
echo 'my-lib.h' | tr 'a-z./-' 'A-Z___'

#### -ds deletes then squeezes
printf 'aabbccdd\n' | tr -ds 'a' 'c'

#### fill in the middle of string2
printf 'abcdef\n' | tr 'a-f' 'x[y*]z'

#### -s alone on a class
printf 'a  b\t\tc\n' | tr -s '[:blank:]'

#### upper to lower
printf 'Hello World 123\n' | tr '[:upper:]' '[:lower:]'

#### -c with translation
printf 'ab12\n' | tr -c 'a-z\n' '_'

#### backslash escapes of special characters
printf 'a-b\n' | tr '\-' '_'
