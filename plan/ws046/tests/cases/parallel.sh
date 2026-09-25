# ws064: make -j (several recipes at once, the jobserver that recursive
# makes share, .WAIT, .NOTPARALLEL, failures with and without -k), run by
# make-diff.py.  The recipes write files and a later make or command reads
# them, so that the output does not depend on which recipe ends first.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

#### -j4 makes independent targets and then what needs them
cat > Makefile <<'EOF'
all: out
out: a b c d
	@cat a b c d > out; cat out
a b c d:
	@echo $@ > $@
EOF
$MAKE -j4

#### -j keeps the order a chain of prerequisites sets
cat > Makefile <<'EOF'
all: c
a:
	@echo a > a
b: a
	@test -f a && echo b > b
c: b
	@test -f b && cat a b
EOF
$MAKE -j3

#### -j without a count, and --jobs=N
cat > Makefile <<'EOF'
all: x y
	@cat x y
x y:
	@echo $@ > $@
EOF
$MAKE -j
rm -f x y
$MAKE --jobs=2

#### a recursive make shares the jobs
mkdir sub
cat > sub/Makefile <<'EOF'
all: p q r
p q r:
	@echo $@ > $@
EOF
cat > Makefile <<'EOF'
all:
	@$(MAKE) -s -C sub
	@cat sub/p sub/q sub/r
EOF
$MAKE -j3 --no-print-directory

#### .WAIT makes the prerequisites before it first
cat > Makefile <<'EOF'
all: first .WAIT second
first:
	@sleep 1; echo first > first
second:
	@test -f first && echo second-after-first
EOF
$MAKE -j4

#### .NOTPARALLEL makes the recipes run one at a time
cat > Makefile <<'EOF'
.NOTPARALLEL:
all: one two
one:
	@sleep 1; echo one > one
two:
	@test -f one && echo two-after-one
EOF
$MAKE -j4

#### a failure stops -j after the running recipes end
cat > Makefile <<'EOF'
all: good bad slow
good:
	@echo good > good
bad:
	@false
slow:
	@sleep 1; echo slow > slow
EOF
$MAKE -j4
echo status $?
cat good slow

#### -k with -j makes what it can
cat > Makefile <<'EOF'
all: a b c
	@echo not reached
a:
	@false
b c:
	@echo $@ > $@
EOF
$MAKE -k -j4
echo status $?
cat b c

#### -j1 is serial
cat > Makefile <<'EOF'
all: a b c
a b c:
	@echo $@
EOF
$MAKE -j1
