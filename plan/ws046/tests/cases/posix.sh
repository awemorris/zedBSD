# ws046: POSIX make (XCU make, 2024), run by make-diff.py.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

#### the first target is the default goal
cat > Makefile <<'EOF'
first:
	@echo first
second:
	@echo second
EOF
$MAKE

#### a named goal, and several goals in order
cat > Makefile <<'EOF'
a:
	@echo a
b:
	@echo b
c:
	@echo c
EOF
$MAKE c a b

#### commands are echoed unless @
cat > Makefile <<'EOF'
all:
	echo one
	@echo two
EOF
$MAKE

#### each command line is its own shell
cat > Makefile <<'EOF'
all:
	x=1; echo "x=$$x"
	echo "x=$$x"
EOF
$MAKE -s

#### a failing command stops make with status 2
cat > Makefile <<'EOF'
all:
	@echo before
	@false
	@echo after
EOF
$MAKE
echo status=$?

#### - ignores the failure of one command
cat > Makefile <<'EOF'
all:
	@echo before
	-@false
	@echo after
EOF
$MAKE
echo status=$?

#### -i ignores every failure
cat > Makefile <<'EOF'
all:
	@false
	@echo after
EOF
$MAKE -i
echo status=$?

#### -k goes on with other targets after a failure
cat > Makefile <<'EOF'
all: bad good
bad:
	@false
good:
	@echo good
EOF
$MAKE -k
echo status=$?

#### -n prints the commands without running them
cat > Makefile <<'EOF'
all:
	@echo quiet
	echo loud
EOF
$MAKE -n
echo status=$?

#### -n still runs a command with +
cat > Makefile <<'EOF'
all:
	+@echo plus
	@echo other
EOF
$MAKE -n

#### -s silences the echo
cat > Makefile <<'EOF'
all:
	echo silent
EOF
$MAKE -s

#### a target newer than its prerequisite is not remade
cat > Makefile <<'EOF'
out: in
	@echo making out
	@cp in out
EOF
echo x > in
$MAKE
$MAKE
touch -t 203001010000 in
$MAKE

#### a missing prerequisite with no rule is an error
cat > Makefile <<'EOF'
out: missing
	@echo never
EOF
$MAKE
echo status=$?

#### a target is made once however often it is named
cat > Makefile <<'EOF'
all: a b
a: c
	@echo a
b: c
	@echo b
c:
	@echo c
EOF
$MAKE

#### prerequisites are made left to right, depth first
cat > Makefile <<'EOF'
all: x y
x: x1 x2
	@echo x
x1:
	@echo x1
x2:
	@echo x2
y:
	@echo y
EOF
$MAKE

#### macros, and a later definition wins
cat > Makefile <<'EOF'
A = one
B = $(A) two
A = three
all:
	@echo $(B) ${A} $A
EOF
$MAKE

#### macros on the command line override the Makefile
cat > Makefile <<'EOF'
V = file
all:
	@echo $(V)
EOF
$MAKE V=command

#### -e lets the environment override the Makefile
cat > Makefile <<'EOF'
V = file
all:
	@echo $(V)
EOF
V=env $MAKE
V=env $MAKE -e

#### environment variables are macros
cat > Makefile <<'EOF'
all:
	@echo $(HOMEVAR)
EOF
HOMEVAR=from-env $MAKE

#### the substitution reference
cat > Makefile <<'EOF'
SRC = a.c b.c c.c
all:
	@echo $(SRC:.c=.o) ${SRC:c=h}
EOF
$MAKE

#### $$ is a dollar for the shell
cat > Makefile <<'EOF'
all:
	@x=5; echo $$x '$$'
EOF
$MAKE

#### automatic macros $@ $< $? $*
cat > Makefile <<'EOF'
.SUFFIXES: .in .out
all: one.out
.in.out:
	@echo "target=$@ first=$< newer=$? stem=$*"
	@cp $< $@
EOF
echo x > one.in
$MAKE

#### $? lists only the newer prerequisites
cat > Makefile <<'EOF'
out: a b c
	@echo newer: $?
	@touch out
EOF
touch -t 200001010000 a b c
touch -t 201001010000 out
touch -t 202001010000 b
$MAKE

#### a suffix rule makes a file from its source
cat > Makefile <<'EOF'
.SUFFIXES:
.SUFFIXES: .x .y
.x.y:
	@echo "$< -> $@"
	@cp $< $@
all: f.y
EOF
echo data > f.x
$MAKE
cat f.y

#### a single-suffix rule
cat > Makefile <<'EOF'
.SUFFIXES:
.SUFFIXES: .sh
.sh:
	@echo "script $< -> $@"
	@cp $< $@
EOF
echo 'echo hi' > prog.sh
$MAKE prog

#### .PHONY targets run even when the file exists
cat > Makefile <<'EOF'
.PHONY: clean
clean:
	@echo cleaning
EOF
touch clean
$MAKE clean

#### a target with no commands and no file is made
cat > Makefile <<'EOF'
all: stamp
stamp:
EOF
$MAKE
echo status=$?

#### .DEFAULT gives the commands for a target without a rule
cat > Makefile <<'EOF'
all: thing
.DEFAULT:
	@echo default for $@
EOF
$MAKE

#### .IGNORE ignores every error
cat > Makefile <<'EOF'
.IGNORE:
all:
	@false
	@echo went on
EOF
$MAKE
echo status=$?

#### .SILENT silences every echo
cat > Makefile <<'EOF'
.SILENT:
all:
	echo quiet
EOF
$MAKE

#### -f names the Makefile, and - is standard input
cat > other.mk <<'EOF'
all:
	@echo other
EOF
$MAKE -f other.mk
printf 'all:\n\t@echo stdin\n' | $MAKE -f -

#### makefile is read before Makefile
printf 'all:\n\t@echo lower\n' > makefile
printf 'all:\n\t@echo upper\n' > Makefile
$MAKE

#### comments and continuation lines
cat > Makefile <<'EOF'
# a comment
LIST = a \
	b \
	c   # trailing comment
all:
	@echo $(LIST) \
	and more
EOF
$MAKE

#### include reads another file
cat > inc.mk <<'EOF'
FROM = included
EOF
cat > Makefile <<'EOF'
include inc.mk
all:
	@echo $(FROM)
EOF
$MAKE

#### -include does not mind a missing file
cat > Makefile <<'EOF'
-include missing.mk
all:
	@echo fine
EOF
$MAKE

#### ::= and += and ?= and != (POSIX 2024)
cat > Makefile <<'EOF'
A = a
B ::= $(A)
A = changed
C = one
C += two
D ?= set
D ?= not
E != echo shell
all:
	@echo $(B) $(C) $(D) $(E)
EOF
$MAKE

#### -q reports whether the target is up to date
cat > Makefile <<'EOF'
out: in
	@cp in out
EOF
echo x > in
$MAKE -q
echo status=$?
$MAKE -s
$MAKE -q
echo status=$?

#### -t touches instead of making
cat > Makefile <<'EOF'
out: in
	@echo should not run
EOF
echo x > in
$MAKE -t -s
ls out

#### -C changes directory first
mkdir sub
printf 'all:\n\t@echo in sub\n' > sub/Makefile
$MAKE -C sub

#### the MAKE macro, and recursion with MAKEFLAGS
mkdir sub
printf 'all:\n\t@echo sub sees $(V)\n' > sub/Makefile
cat > Makefile <<'EOF'
all:
	@cd sub && $(MAKE)
EOF
$MAKE -s V=passed

#### MAKEFLAGS carries -k and -n to a sub-make
mkdir sub
printf 'all:\n\techo in sub\n' > sub/Makefile
cat > Makefile <<'EOF'
all:
	cd sub && $(MAKE)
EOF
$MAKE -n

#### the built-in .c.o rule and CC
cat > Makefile <<'EOF'
CC = echo compile
all: x.o
EOF
touch x.c
$MAKE -s

#### a double-colon rule runs each of its recipes
cat > Makefile <<'EOF'
all:: ; @echo one
all:: ; @echo two
EOF
$MAKE

#### a recipe on the rule line after ;
cat > Makefile <<'EOF'
all: ; @echo inline
EOF
$MAKE

#### a target that is also a directory prerequisite
mkdir -p d
cat > Makefile <<'EOF'
d/file: d
	@echo made $@
	@touch $@
EOF
$MAKE
