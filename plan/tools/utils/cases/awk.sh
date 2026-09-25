#### print fields
printf 'a b c\nd e f\n' | awk '{print $2}'

#### print whole line and NF
printf 'a b c\nd e\n' | awk '{print NF, $0}'

#### last field
printf 'a b c\n' | awk '{print $NF}'

#### BEGIN and END
printf '1\n2\n3\n' | awk 'BEGIN{s=0} {s+=$1} END{print "sum", s}'

#### BEGIN only does not read input
awk 'BEGIN{print "hi"}'

#### NR in END
printf 'a\nb\nc\n' | awk 'END{print NR}'

#### regex pattern
printf 'apple\nbanana\ncherry\n' | awk '/an/'

#### expression pattern
printf '5\n15\n25\n' | awk '$1 > 10'

#### range pattern
printf 'a\nstart\nb\nend\nc\n' | awk '/start/,/end/'

#### negated regex match
printf 'a1\nb2\n' | awk '$0 !~ /1/'

#### field separator -F
printf 'a:b:c\n' | awk -F: '{print $2}'

#### FS in BEGIN
printf 'a,b,c\n' | awk 'BEGIN{FS=","} {print $3}'

#### FS regex
printf 'a1b22c\n' | awk -F'[0-9]+' '{print $1, $2, $3}'

#### FS single space trims leading blanks
printf '   a   b  \n' | awk '{print NF ":" $1}'

#### FS tab
printf 'a b\tc\n' | awk -F'\t' '{print $2}'

#### OFS when a field changes
printf 'a b c\n' | awk 'BEGIN{OFS="-"} {$2="X"; print}'

#### $0 rebuilt with OFS after $1=$1
printf 'a   b   c\n' | awk 'BEGIN{OFS=","} {$1=$1; print}'

#### assigning past NF adds fields
printf 'a b\n' | awk '{$5="e"; print; print NF}'

#### NF assignment truncates
printf 'a b c d\n' | awk '{NF=2; print}'

#### ORS
printf 'a\nb\n' | awk 'BEGIN{ORS=";"} {print} END{printf "\n"}'

#### RS paragraph mode
printf 'a\nb\n\n\nc\nd\n' | awk 'BEGIN{RS=""} {print NR": "$1"/"$2}'

#### RS single character
printf 'a;b;c' | awk 'BEGIN{RS=";"} {print NR, $0}'

#### -v assignment
awk -v x=5 'BEGIN{print x*2}'

#### command line assignment operand
printf 'l\n' > f; awk '{print v, $0}' v=1 f v=2 f

#### FILENAME and FNR
printf 'a\nb\n' > f1; printf 'c\n' > f2; awk '{print FILENAME, FNR, NR}' f1 f2

#### -f program file
printf '{print $1 * 2}\n' > prog.awk; printf '3\n4\n' | awk -f prog.awk

#### arithmetic and precedence
awk 'BEGIN{print 2+3*4, (2+3)*4, 2^3^2, 7%3, -2^2}'

#### integer and float output
awk 'BEGIN{print 1/3; print 10/2; print 1e6; print 1e20; print 0.1+0.2}'

#### OFMT and CONVFMT
awk 'BEGIN{OFMT="%.2f"; x=3.14159; print x; CONVFMT="%.1f"; y=x ""; print y}'

#### integer values print as integers
awk 'BEGIN{x=3.0; print x; print x ""}'

#### string concatenation
awk 'BEGIN{a="foo"; b="bar"; print a b, a "-" b}'

#### comparison of numbers and strings
awk 'BEGIN{print (10<9), ("10"<"9"), (2=="2"), ("a"<"b")}'

#### fields that look numeric compare as numbers
printf '10 9\n' | awk '{print ($1 > $2)}'

#### uninitialized variables
awk 'BEGIN{print x+0, "[" x "]", length(x)}'

#### increment and compound assignment
awk 'BEGIN{i=5; i++; ++i; i+=10; i-=2; i*=2; i/=4; i%=5; print i; print i++ + ++i}'

#### conditional expression
awk 'BEGIN{x=5; print (x>3 ? "big" : "small")}'

#### logical operators
awk 'BEGIN{print (1 && 0), (1 || 0), !0, !"", !"a"}'

#### in and delete
awk 'BEGIN{a["x"]=1; print ("x" in a), ("y" in a); delete a["x"]; print ("x" in a)}'

#### delete a whole array
awk 'BEGIN{a[1]; a[2]; delete a; n=0; for (k in a) n++; print n}'

#### multidimensional subscripts
awk 'BEGIN{a[1,2]=3; for (k in a) {split(k, p, SUBSEP); print p[1], p[2], a[k]}}'

#### for in counts
printf 'x\ny\nx\nz\nx\n' | awk '{c[$1]++} END{n=0; for (k in c) n++; print n, c["x"], c["y"], c["z"]}'

#### while and do loops
awk 'BEGIN{i=0; while (i<3) {printf "%d ", i; i++}; do {printf "d%d ", i; i--} while (i>0); print ""}'

#### for loop with break and continue
awk 'BEGIN{for (i=0; i<10; i++) {if (i==2) continue; if (i==5) break; printf "%d ", i}; print ""}'

#### next skips the rest
printf '1\n2\n3\n' | awk '$1==2{next} {print}'

#### exit with a status runs END
printf '1\n2\n' | awk '{print} $1==1{exit 3} END{print "end"}'; echo "st=$?"

#### length of a string and of $0
printf 'hello\n' | awk '{print length(), length($0), length("ab")}'

#### substr
awk 'BEGIN{s="hello"; print substr(s,2,3), substr(s,3), substr(s,0,2), substr(s,-1,3), substr(s,10)}'

#### index
awk 'BEGIN{print index("hello","ll"), index("hello","z")}'

#### split with a separator
awk 'BEGIN{n=split("a:b:c", a, ":"); print n, a[1], a[3]}'

#### split with default FS
awk 'BEGIN{n=split("  a  b ", a); print n, a[1], a[2]}'

#### split with a regex
awk 'BEGIN{n=split("a1b22c", a, /[0-9]+/); print n, a[2]}'

#### sub and gsub
awk 'BEGIN{s="aaa"; sub(/a/,"b",s); print s; n=gsub(/a/,"c",s); print n, s}'

#### gsub on $0 with ampersand
printf 'foo bar\n' | awk '{gsub(/o/,"[&]"); print}'

#### gsub with escaped ampersand
awk 'BEGIN{s="x"; gsub(/x/,"\\&",s); print s}'

#### gsub with an empty match
awk 'BEGIN{s="abc"; gsub(/x*/,"-",s); print s}'

#### match sets RSTART and RLENGTH
awk 'BEGIN{print match("foobar", /o+/), RSTART, RLENGTH; print match("x", /y/), RSTART, RLENGTH}'

#### sprintf formats
awk 'BEGIN{printf "%5s|%-5s|%05d|%x|%o|%e|%c|%c|%%\n", "ab", "cd", 42, 255, 8, 1234.5, 65, "hello"}'

#### printf with width from star
awk 'BEGIN{printf "%*d|%.*f\n", 5, 42, 2, 3.14159}'

#### toupper and tolower
awk 'BEGIN{print toupper("abC1"), tolower("ABc1")}'

#### int and math
awk 'BEGIN{print int(3.9), int(-3.9), sqrt(16), exp(0), log(1), sin(0), cos(0), atan2(0,1)}'

#### srand and rand are deterministic with a seed
awk 'BEGIN{srand(1); a=rand(); srand(1); b=rand(); print (a==b), (a>=0 && a<1)}'

#### user function with recursion
awk 'function fact(n) { return n<=1 ? 1 : n*fact(n-1) } BEGIN{print fact(10)}'

#### function locals and array by reference
awk 'function f(a, i) { a["k"]="v"; i=5 } BEGIN{i=1; f(arr); print arr["k"], i}'

#### getline from a file
printf 'x\ny\n' > f; awk 'BEGIN{while ((getline line < "f") > 0) print "got", line}'

#### getline from a command
awk 'BEGIN{"echo hi" | getline v; print v}'

#### plain getline reads the next record
printf '1\n2\n3\n' | awk '{getline; print}'

#### getline var updates NR
printf '1\n2\n3\n' | awk 'NR==1{getline x; print x, NR}'

#### print to a file and close
awk 'BEGIN{print "a" > "out"; print "b" > "out"; close("out"); while ((getline l < "out") > 0) print l}'

#### append with >>
printf 'first\n' > out; awk 'BEGIN{print "second" >> "out"}'; cat out

#### print to a pipe
awk 'BEGIN{print "b\na" | "cat"; close("cat"); print "done"}'

#### system
awk 'BEGIN{r=system("exit 3"); print r}'

#### ENVIRON
X=hello awk 'BEGIN{print ENVIRON["X"]}'

#### ARGV and ARGC
awk 'BEGIN{print ARGC; for (i=0; i<ARGC; i++) if (i) print ARGV[i]}' a b

#### regex dynamic from a string
awk 'BEGIN{re="^a.c$"; print ("abc" ~ re), ("abcd" ~ re)}'

#### regex special characters
awk 'BEGIN{print ("a+b" ~ /a\+b/), ("a.b" ~ /a\.b/), ("ab" ~ /^(a|b)+$/)}'

#### string escapes
awk 'BEGIN{print "tab\there", "nl\\n", "quote\"", length("\101")}'

#### semicolons and newlines as separators
awk 'BEGIN { x = 1
y = 2 ; print x + y
}'

#### comments in programs
awk '# comment
BEGIN { print "ok" } # trailing'

#### getline var < file returns 0 at end and -1 for missing
awk 'BEGIN{print (getline x < "nosuchfile")}'

#### printf without newline
awk 'BEGIN{printf "a"; printf "b\n"}'

#### print with parentheses and comma
awk 'BEGIN{print("a","b")}'

#### number to string of large integers
awk 'BEGIN{print 2^31, 2^53, 100000 * 100000}'

#### string to number conversions
awk 'BEGIN{print "3x"+0, " 12 "+0, "0x10"+0, ".5"+0, "1e2"+0}'

#### configure style: extract from a table
printf 'NAME=zed\nVERSION=1.2\n' | awk -F= '$1=="VERSION"{print $2}'

#### configure style: join lines
printf 'a\nb\nc\n' | awk '{s = s (NR>1 ? " " : "") $0} END{print s}'

#### configure style: config.status substitution
printf 'x @A@ y @B@\n' | awk 'BEGIN{v["A"]="1"; v["B"]="two"} {line=$0; while (match(line, /@[A-Z]+@/)) {k=substr(line, RSTART+1, RLENGTH-2); line=substr(line,1,RSTART-1) v[k] substr(line,RSTART+RLENGTH)}; print line}'

#### precedence: unary minus, power and concatenation
awk 'BEGIN{x=2; print -x^2, 2^-1, 1 " " -1, 2 3 * 4, 10 - 2 - 3, 2^3^2, !0 + 1}'

#### assignment is right associative and has a value
awk 'BEGIN{a = b = c = 4; print a, b, c; print (d = 5) + 1, d}'

#### field expressions: $NF-1, $(NF-1), $i++ and $++i
printf '1 2 3\n' | awk '{i=1; print $NF-1, $(NF-1); x = $i++; print x, i; print $++i}'

#### length without parentheses in a pattern
printf 'ab\nabcd\n' | awk 'length > 3'

#### numeric strings: constants are strings, fields are numbers
printf '3.0 3\n' | awk '{x = "3.0"; print (x == 3), ($1 == 3), ($1 == $2), ($1 == "3")}'

#### uninitialized compares as both
awk 'BEGIN{print (x == 0), (x == ""), (x < 1), (x < "a")}'

#### subscripts: numbers by their integer form, strings as they are
awk 'BEGIN{a[1] = "one"; print a[1.0], a["1"]; a["01"] = "zero-one"; print a[1], a["01"]; CONVFMT = "%.2f"; b[0.123] = 1; for (k in b) print k}'

#### multiple subscripts and in with a list
awk 'BEGIN{a[1, "x"] = 5; print ((1, "x") in a), ((2, "x") in a); SUBSEP = ":"; a[3, 4] = 1; print ((3, 4) in a), ("3:4" in a)}'

#### delete inside for in
awk 'BEGIN{for (i = 1; i <= 5; i++) a[i] = i; for (k in a) delete a[k]; n = 0; for (k in a) n++; print n}'

#### exit in BEGIN still runs END, and exit in END keeps the status
awk 'BEGIN{print "b"; exit 4; print "no"} END{print "e"; exit}'; echo "st=$?"

#### next inside a function
printf '1\n2\n3\n' | awk 'function skip() { next } $1 == 2 { skip() } { print }'

#### return value and uninitialized return
awk 'function f(x) { if (x) return x * 2 } BEGIN{print f(3), "[" f(0) "]"}'

#### recursion with local arrays
awk 'function fill(n,   a, i, s) { for (i = 1; i <= n; i++) a[i] = i; for (i in a) s += a[i]; return s } BEGIN{print fill(3), fill(10)}'

#### passing an array element and a field by value
printf 'x y\n' | awk 'function f(v) { v = "changed"; return v } {a[1] = "keep"; f(a[1]); f($1); print a[1], $1}'

#### printf conversions
awk 'BEGIN{printf "%5.2f|%-6s|%+d|% d|%x|%o|%e|%G|%i\n", 3.14159, "ab", 3, 3, -1, 8, 12345.678, 0.00001, "0x1A"}'

#### printf %c with numbers, numeric strings and strings
printf '65\n' | awk '{printf "%c|%c|%c|%3c|%-3c|\n", 66, $1, "xyz", "a", "b"}'

#### printf %s with precision and width, and %%
awk 'BEGIN{printf "%.2s|%5.1s|%-5s|%%|%s\n", "abcdef", "xyz", "q", 1/4}'

#### printf with too few arguments is an error
## skip-status
awk 'BEGIN{printf "%s %s\n", "a"}' 2>/dev/null; echo "failed=$?"

#### sprintf with integers out of range
awk 'BEGIN{print sprintf("%d", 2^64), sprintf("%d", -3.9), sprintf("%x", 255)}'

#### substr with fractions and out-of-range positions
awk 'BEGIN{s = "hello"; print substr(s, 1.5, 2) "|" substr(s, 2.7, 2) "|" substr(s, -1.5, 4) "|" substr(s, 1, 0.6) "|" substr(s, 5, 10) "|" substr(s, 6) "|"}'

#### index and length of numbers
awk 'BEGIN{print index(12345, 34), length(12345), length(1/3), index("abc", "")}'

#### split with a single space and with one character
awk 'BEGIN{n = split(" a  b ", p, " "); print n, p[1], p[2]; n = split("a..b", q, "."); print n, q[1], "[" q[2] "]", q[3]}'

#### split elements are numeric strings
awk 'BEGIN{split("10 9", p); print (p[1] > p[2])}'

#### sub and gsub replacement backslashes
awk 'BEGIN{s = "x"; gsub(/x/, "[\\\\&]", s); print s; s = "x"; gsub(/x/, "[\\\\\\&]", s); print s; s = "x"; gsub(/x/, "[\\\\]", s); print s; s = "x"; gsub(/x/, "[\\q]", s); print s}'

#### gsub with anchors
awk 'BEGIN{s = "abc"; gsub(/^/, ">", s); gsub(/$/, "<", s); print s; t = "aaa"; print gsub(/a*/, "-", t), t; u = "baaac"; gsub(/a*/, "-", u); print u}'

#### gsub on a field rebuilds the record
printf 'a-b c-d\n' | awk 'BEGIN{OFS=":"} {gsub(/-/, "+", $2); print; print NF}'

#### sub returns 0 and leaves the target alone when nothing matches
printf 'a   b\n' | awk '{n = sub(/x/, "y"); print n, $0}'

#### match with a dynamic regex and RSTART
awk 'BEGIN{re = "b+"; print match("abbbc", re), RSTART, RLENGTH; print match("", /x*/), RSTART, RLENGTH}'

#### regex literals: slash, brackets and classes
awk 'BEGIN{print ("a/b" ~ /a\/b/), ("a/b" ~ /a[/]b/), ("a]b" ~ /a[]]b/), ("x1" ~ /^[[:alpha:]][[:digit:]]$/), ("a.b" ~ "a\\.b"), ("axb" ~ "a\\.b")}'

#### regex escapes: tab and octal
printf 'a\tb\n' | awk '/a\tb/ {print "tab"} /a\011b/ {print "octal"}'

#### dynamic regex from a field
printf 'a+ aaa\n' | awk '{print ($2 ~ $1), ("a+" ~ $1)}'

#### FS regex with leading separator, and FS a single character
printf ':a::b:\n' | awk -F: '{print NF, "[" $1 "]", $3}'; printf '  a b\n' | awk -F' +' '{print NF, "[" $1 "]"}'

#### FS changed in a rule applies to the next record
printf 'a:b c\nd:e f\n' | awk '{FS = ":"; print $1}'

#### paragraph mode with a character FS splits at newlines too
printf 'a:b\nc:d\n\ne\n' | awk 'BEGIN{RS = ""; FS = ":"} {print NR, NF, $3}'

#### NF set to 0, and $0 in END
printf 'a b\nc d\n' | awk '{last = $0} END{print $0, NF; NF = 0; print "[" $0 "]"}'

#### assigning $0 splits again
awk 'BEGIN{$0 = "x y z"; print NF, $2; $3 = ""; print NF "[" $0 "]"}'

#### -v with escapes and a numeric string
awk -v 's=a\tb' -v n=010 'BEGIN{print s; print (n == 10), n}'

#### ARGV can be changed in BEGIN
printf 'one\n' > f1; printf 'two\n' > f2; awk 'BEGIN{ARGV[1] = "f2"} {print}' f1

#### an assignment operand after the last file happens before END
printf 'l\n' > f; awk 'END{print v}' f v=9

#### a missing file is reported and the rest are read
## skip-status
printf 'ok\n' > f; awk '{print}' nosuch f 2>/dev/null; echo "st=$?"

#### empty pattern-action forms and semicolons
printf 'a\n' | awk '
/a/
;
{ print "x" } ; END { print "e" }'

#### if else chains and dangling else
awk 'BEGIN{for (i = 0; i < 4; i++) { if (i == 0) print "zero"; else if (i == 1) print "one"; else print "many" } if (1) if (0) print "no"; else print "inner else"}'

#### getline from the main input in END returns 0
printf 'a\n' | awk 'END{r = getline; print r, $0}'

#### getline var at the end of input leaves the variable
printf 'a\n' | awk '{x = "keep"; r = getline x; print r, x}'

#### output of numbers: OFMT for print, CONVFMT for concatenation, integers exact
awk 'BEGIN{OFMT = "%.2f"; CONVFMT = "%.3f"; x = 3.14159; print x, x ""; print 17, 17 "", 1e15, 1e16 + 1; print 0.1 * 3, -0.0 ""}'

#### string to number with hex, inf and exponents
awk 'BEGIN{print "0x1A" + 0, "1e3x" + 0, ".5" + 0, "+7" + 0, "-" + 0, "inf" + 0, "-nan" + 0}'

#### comparisons of numbers with strings of the input
printf '10 9 abc 1e1\n' | awk '{print ($1 > $2), ($1 < $3), ($1 == $4), ($3 > 5)}'

#### increment of a field and of an element
printf '5\n' | awk '{$1++; a["k"]++; a["k"] += 2; print $0, a["k"]}'

#### while with a getline loop over the main input
printf '1\n2\n3\n4\n' | awk 'NR == 1 {while ((getline line) > 0) s = s line; print s, NR}'

#### compound assignment operators
awk 'BEGIN{x = 7; x %= 4; y = 2; y ^= 3; z = 9; z /= 2; print x, y, z}'

#### division by zero is an error
## skip-status
awk 'BEGIN{x = 0; print 1/x}' 2>/dev/null; echo "failed=$?"

#### toupper keeps other bytes
awk 'BEGIN{print toupper("a1-z_é"), tolower(123)}'

#### int and exp and log of edge values
awk 'BEGIN{print int("3.9x"), int(-0.5), exp(1), log(10), sqrt(2), atan2(1, 1) * 4}'

#### rand stays in range and srand returns the last seed
awk 'BEGIN{srand(5); s = srand(7); ok = 1; for (i = 0; i < 1000; i++) { r = rand(); if (r < 0 || r >= 1) ok = 0 } print s, ok}'

#### printf with a variable format and dollar-free fields
printf 'a 1\nb 22\n' | awk '{fmt = "%-3s[%3d]\n"; printf fmt, $1, $2}'

#### configure style: a heredoc substitution script
printf 'prefix=@prefix@ libdir=@libdir@\n' | awk '
BEGIN {
  S["prefix"] = "/usr/local"
  S["libdir"] = "${prefix}/lib"
}
{
  line = $0
  nfields = split(line, field, "@")
  subst = field[1]
  for (i = 2; i < nfields; i++) {
    key = field[i]
    if (key in S) { subst = subst S[key]; i++ }
    else subst = subst "@" key
    subst = subst field[i]
  }
  if (i == nfields) subst = subst field[i]
  print subst
}'

#### > keeps one stream until close, and >> on the same name shares it
awk 'BEGIN{print "1" > "o"; print "2" >> "o"; printf "%s\n", "3" > "o"; close("o"); print "4" >> "o"}'; cat o

#### output split into files by a key
printf 'a 1\nb 2\na 3\n' | awk '{print $2 > ($1 ".txt")} END{close("a.txt"); close("b.txt")}'; cat a.txt b.txt

#### standard output and error by name
awk 'BEGIN{print "out" > "/dev/stdout"}'; awk 'BEGIN{print "err" > "/dev/stderr"}' 2>e; cat e; awk 'BEGIN{print "dash" > "-"}'; ls

#### a pipe left open is closed at the end, after what was written before
awk 'BEGIN{print "x"; print "c\nb\na" | "sort"; print "z"}'

#### output before system comes first
awk 'BEGIN{print "before"; r = system("echo mid"); print "after", r}'

#### system status of a signal and of success
awk 'BEGIN{print system("exit 0"), system("kill -9 $$")}'

#### command | getline reads every line, and NR stays
awk 'BEGIN{while (("printf \"a b\\nc d e\\n\"" | getline) > 0) print NF, $2, NR}'

#### getline < file leaves NR and FNR, sets NF
printf 'p q r\n' > f; printf 'main\n' | awk '{getline < "f"; print NR, FNR, NF, $3}'

#### close and read a file again
printf 'one\ntwo\n' > f; awk 'BEGIN{getline a < "f"; close("f"); getline b < "f"; getline c < "f"; print a, b, c}'

#### close of a name not open, and of a file
awk 'BEGIN{print close("none"); print "x" > "o"; print close("o")}'

#### fflush of everything and of an unknown name
awk 'BEGIN{printf "a" > "o"; print fflush(), fflush("unknown"), fflush("o")}' 2>/dev/null; cat o; echo

#### getline var < missing file leaves the variable
awk 'BEGIN{v = "keep"; r = (getline v < "missing"); print r, v}'

#### printf to a pipe
awk 'BEGIN{printf "%s\n", "3" | "cat"; printf "%s\n", "1" | "cat"; close("cat"); print "end"}'
