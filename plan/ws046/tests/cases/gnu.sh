# ws046: the GNU make features that WS046 takes (the WS's range: := += ?=,
# conditionals, pattern rules, the common functions, include, target
# variables), run by make-diff.py.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

#### := expands at once, = when used
cat > Makefile <<'EOF'
A = one
B := $(A)
C = $(A)
A = two
all:
	@echo $(B) $(C)
EOF
$MAKE

#### += keeps the flavor of the variable
cat > Makefile <<'EOF'
X = a
S := b
X += $(Y)
S += $(Y)
Y = late
all:
	@echo "$(X)|$(S)|"
EOF
$MAKE

#### ?= sets only an unset variable
cat > Makefile <<'EOF'
A ?= first
A ?= second
B =
B ?= not
all:
	@echo "$(A)|$(B)|"
EOF
$MAKE

#### ifeq, ifneq, else, nested
cat > Makefile <<'EOF'
MODE = fast
ifeq ($(MODE),fast)
  SPEED = high
  ifneq ($(EXTRA),)
    SPEED += extra
  endif
else
  SPEED = low
endif
ifeq "$(MODE)" "slow"
  NOTE = slow
else ifeq '$(MODE)' 'fast'
  NOTE = quick
endif
all:
	@echo $(SPEED) $(NOTE)
EOF
$MAKE
$MAKE EXTRA=1 MODE=slow

#### ifdef and ifndef
cat > Makefile <<'EOF'
DEFINED = x
EMPTY =
ifdef DEFINED
A = yes
endif
ifdef EMPTY
B = yes
else
B = no
endif
ifndef NOTHING
C = unset
endif
all:
	@echo $(A) $(B) $(C)
EOF
$MAKE

#### a pattern rule
cat > Makefile <<'EOF'
all: a.out b.out
%.out: %.in
	@echo "$< -> $@ ($*)"
EOF
touch a.in b.in
$MAKE

#### a pattern rule with a directory part
cat > Makefile <<'EOF'
all: build/x.o
build/%.o: src/%.c
	@echo "$< -> $@ stem $*"
EOF
mkdir src build
touch src/x.c
$MAKE

#### $^ and $+ and $(@D) $(@F) $(<D) $(<F)
cat > Makefile <<'EOF'
dir/out: a b a
	@echo "^=$^ +=$+ D=$(@D) F=$(@F) <D=$(<D) <F=$(<F)"
EOF
mkdir dir
touch a b
$MAKE

#### subst, patsubst, strip, findstring, filter, filter-out, sort
cat > Makefile <<'EOF'
L = c.c a.o b.c  a.o d.h
all:
	@echo "[$(subst .c,.C,$(L))]"
	@echo "[$(patsubst %.c,%.o,$(L))]"
	@echo "[$(strip   a   b  )]"
	@echo "[$(findstring b.c,$(L))][$(findstring z,$(L))]"
	@echo "[$(filter %.c %.h,$(L))]"
	@echo "[$(filter-out %.o,$(L))]"
	@echo "[$(sort $(L))]"
EOF
$MAKE

#### word, wordlist, words, firstword, lastword
cat > Makefile <<'EOF'
L = one two three four
all:
	@echo $(word 2,$(L)) / $(wordlist 2,3,$(L)) / $(words $(L)) / $(firstword $(L)) / $(lastword $(L))
EOF
$MAKE

#### dir, notdir, suffix, basename, addsuffix, addprefix, join
cat > Makefile <<'EOF'
F = src/a.c b.h /x/y
all:
	@echo "$(dir $(F))|$(notdir $(F))|$(suffix $(F))|$(basename $(F))"
	@echo "$(addsuffix .o,a b)|$(addprefix p/,a b)|$(join a b,1 2 3)"
EOF
$MAKE

#### wildcard
cat > Makefile <<'EOF'
all:
	@echo $(sort $(wildcard *.c)) [$(wildcard none*)]
EOF
touch b.c a.c x.h
$MAKE

#### shell and !=
cat > Makefile <<'EOF'
A := $(shell echo one; echo two)
all:
	@echo "$(A)"
EOF
$MAKE

#### foreach, if, or, and
cat > Makefile <<'EOF'
L = a b c
all:
	@echo '$(foreach x,$(L),<$(x)>)'
	@echo $(if $(L),yes,no) $(if ,yes,no) [$(if ,yes)]
	@echo [$(or ,,z,w)] [$(and a,b,c)] [$(and a,,c)]
EOF
$MAKE

#### call and a user function
cat > Makefile <<'EOF'
reverse = $(2) $(1)
pair = <$(1)|$(2)>
all:
	@echo '$(call reverse,a,b) $(call pair,x,$(call reverse,1,2))'
EOF
$MAKE

#### define and endef, and a multi-line recipe variable
cat > Makefile <<'EOF'
define TWO_LINES
@echo line one
@echo line two
endef
all:
	$(TWO_LINES)
EOF
$MAKE

#### eval defines rules
cat > Makefile <<'EOF'
define RULE
$(1):
	@echo rule for $(1)
endef
$(foreach t,x y,$(eval $(call RULE,$(t))))
all: x y
EOF
$MAKE all

#### origin and flavor, value
cat > Makefile <<'EOF'
A = $(B)
B := b
all:
	@echo $(origin A) $(origin HOME) $(origin CLI) $(origin NONE) $(flavor A) $(flavor B) '$(value A)'
EOF
$MAKE CLI=1

#### info, warning to stderr, error stops
cat > Makefile <<'EOF'
$(info at parse time)
all:
	@echo recipe
EOF
$MAKE
printf '$(error stop here)\nall:\n\t@echo never\n' > Makefile
$MAKE
echo status=$?

#### target-specific variables
cat > Makefile <<'EOF'
CFLAGS = -O
all: a b
a: CFLAGS += -g
a:
	@echo a $(CFLAGS)
b:
	@echo b $(CFLAGS)
EOF
$MAKE

#### target-specific variables reach the prerequisites
cat > Makefile <<'EOF'
all: X = from-all
all: dep
	@echo all $(X)
dep:
	@echo dep $(X)
EOF
$MAKE

#### export and unexport
cat > Makefile <<'EOF'
export A = exported
B = kept
export B
C = hidden
all:
	@echo "$$A $$B [$$C]"
EOF
$MAKE

#### override beats the command line
cat > Makefile <<'EOF'
override V = file
all:
	@echo $(V)
EOF
$MAKE V=command

#### VPATH and vpath find prerequisites elsewhere
cat > Makefile <<'EOF'
VPATH = srcdir
vpath %.h include
all: a.c b.h
	@echo $^
EOF
mkdir srcdir include
touch srcdir/a.c include/b.h
$MAKE

#### order-only prerequisites
cat > Makefile <<'EOF'
out/file: in | out
	@echo made $@
	@touch $@
out:
	@mkdir out
	@echo made dir
EOF
touch in
$MAKE
touch out
$MAKE

#### MAKECMDGOALS and .DEFAULT_GOAL
cat > Makefile <<'EOF'
.DEFAULT_GOAL := second
first:
	@echo first [$(MAKECMDGOALS)]
second:
	@echo second [$(MAKECMDGOALS)]
EOF
$MAKE
$MAKE first

#### MAKELEVEL in a sub-make
mkdir sub
printf 'all:\n\t@echo level $(MAKELEVEL)\n' > sub/Makefile
cat > Makefile <<'EOF'
all:
	@echo level $(MAKELEVEL)
	@$(MAKE) -C sub
EOF
$MAKE

#### .SECONDARY, .INTERMEDIATE and .DELETE_ON_ERROR are accepted
cat > Makefile <<'EOF'
.DELETE_ON_ERROR:
.SECONDARY:
.INTERMEDIATE: mid
all: mid
	@echo all
mid:
	@echo mid
EOF
$MAKE

#### a missing included file that a rule makes is made and read
cat > Makefile <<'EOF'
include gen.mk
all:
	@echo $(GEN)
gen.mk:
	@echo 'GEN = generated' > gen.mk
EOF
$MAKE

#### conditional directives inside a recipe are directives
cat > Makefile <<'EOF'
X = 1
all:
	@echo start
ifeq ($(X),1)
	@echo one
else
	@echo other
endif
	@echo end
EOF
$MAKE

#### long options that autotools and packages use
cat > Makefile <<'EOF'
all:
	@echo hi
EOF
$MAKE --no-print-directory
$MAKE --silent
$MAKE --file=Makefile --directory=.
