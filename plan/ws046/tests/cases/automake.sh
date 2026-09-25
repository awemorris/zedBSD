# ws046: the idioms of Makefiles that automake 1.16/1.17 generates (seen in
# expat 2.8.5 and coreutils 9.12), run by make-diff.py.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

#### nested variable references (silent rules), V unset
cat > Makefile <<'EOF'
AM_DEFAULT_VERBOSITY = 0
AM_V_CC = $(am__v_CC_$(V))
am__v_CC_ = $(am__v_CC_$(AM_DEFAULT_VERBOSITY))
am__v_CC_0 = @echo "  CC      " $@;
am__v_CC_1 =
all: x.o
x.o:
	$(AM_V_CC)echo compiling
EOF
$MAKE

#### nested variable references (silent rules), V=1
cat > Makefile <<'EOF'
AM_DEFAULT_VERBOSITY = 0
AM_V_CC = $(am__v_CC_$(V))
am__v_CC_ = $(am__v_CC_$(AM_DEFAULT_VERBOSITY))
am__v_CC_0 = @echo "  CC      " $@;
am__v_CC_1 =
all: x.o
x.o:
	$(AM_V_CC)echo compiling
EOF
$MAKE V=1

#### configure's probe: does make set $(MAKE)
cat > Makefile <<'EOF'
all:
	@echo '@@@%%%=$(MAKE)=@@@%%%'
EOF
$MAKE | grep -c '@@@%%%=.*=@@@%%%'

#### configure's probe: nested variables
cat > Makefile <<'EOF'
BAR0=false
BAR1=true
V=1
am__doit:
	@$(TRUE)
.PHONY: am__doit
EOF
$MAKE -s BAR0=false BAR1=true V=1 TRUE='$(BAR$(V))' am__doit
echo status=$?
$MAKE -s BAR0=false BAR1=true V=0 TRUE='$(BAR$(V))' am__doit
echo status=$?

#### configure's probe: include (GNU style)
cat > confinc.mk <<'EOF'
am__doit:
	@echo this is the am__doit target >confinc.out
.PHONY: am__doit
EOF
echo 'include confinc.mk # ignored' > confmf.GNU
$MAKE -f confmf.GNU
cat confinc.out

#### dependency files included from a subdirectory
mkdir .deps
echo '# dummy' > .deps/x.Po
cat > Makefile <<'EOF'
DEPDIR = .deps
all:
	@echo built
include ./$(DEPDIR)/x.Po # am--include-marker
EOF
$MAKE

#### am--depfiles makes the dependency files it names
cat > Makefile <<'EOF'
DEPDIR = .deps
MKDIR_P = mkdir -p
am__depfiles_remade = ./$(DEPDIR)/a.Po ./$(DEPDIR)/b.Po
$(am__depfiles_remade):
	@$(MKDIR_P) $(@D)
	@echo '# dummy' >$@-t && mv $@-t $@
am--depfiles: $(am__depfiles_remade)
all: am--depfiles
	@ls .deps
EOF
$MAKE
ls .deps
rm -rf .deps
$MAKE all

#### recursive targets with cd and $(MAKE) $(AM_MAKEFLAGS)
mkdir lib tests
printf 'all:\n\t@echo lib all\ninstall:\n\t@echo lib install\n' > lib/Makefile
printf 'all:\n\t@echo tests all\ninstall:\n\t@echo tests install\n' > tests/Makefile
cat > Makefile <<'EOF'
SUBDIRS = lib tests
AM_MAKEFLAGS = -s
all install:
	@target=`echo $@ | sed s/-recursive//`; \
	list='$(SUBDIRS)'; for subdir in $$list; do \
	  ($(am__cd) $$subdir && $(MAKE) $(AM_MAKEFLAGS) $$target) || exit 1; \
	done
am__cd = CDPATH="$${ZSH_VERSION+.}$(PATH_SEPARATOR)" && cd
.MAKE: all install
.PHONY: all install
EOF
$MAKE -s
$MAKE -s install

#### am__make_running_with_option reads -n from MAKEFLAGS
cat > Makefile <<'EOF'
am__make_running_with_option = \
  case $${target_option-} in \
      ?) ;; \
      *) echo "am__make_running_with_option: internal error: invalid" \
              "target option '$${target_option-}' specified" >&2; \
         exit 1;; \
  esac; \
  has_opt=no; \
  sane_makeflags=$$MAKEFLAGS; \
  if $(am__is_gnu_make); then \
    sane_makeflags=$$MFLAGS; \
  else \
    case $$MAKEFLAGS in \
      *\\[\ \	]*) \
        bs=\\; \
        sane_makeflags=`printf '%s\n' "$$MAKEFLAGS" \
          | sed "s/$$bs$$bs[$$bs $$bs	]*//g"`;; \
    esac; \
  fi; \
  skip_next=no; \
  strip_trailopt () \
  { \
    flg=`printf '%s\n' "$$flg" | sed "s/$$1.*$$//"`; \
  }; \
  for flg in $$sane_makeflags; do \
    test $$skip_next = yes && { skip_next=no; continue; }; \
    case $$flg in \
      *=*|--*) continue;; \
        -*I) strip_trailopt 'I'; skip_next=yes;; \
      -*I?*) strip_trailopt 'I';; \
        -*O) strip_trailopt 'O'; skip_next=yes;; \
      -*O?*) strip_trailopt 'O';; \
        -*l) strip_trailopt 'l'; skip_next=yes;; \
      -*l?*) strip_trailopt 'l';; \
      -[dEDm]) skip_next=yes;; \
      -[JT]) skip_next=yes;; \
    esac; \
    case $$flg in \
      *$$target_option*) has_opt=yes; break;; \
    esac; \
  done; \
  test $$has_opt = yes
am__is_gnu_make = { \
  if test -z '$(MAKELEVEL)'; then \
    false; \
  elif test -n '$(MAKE_HOST)'; then \
    true; \
  elif test -n '$(MAKE_VERSION)' && test -n '$(CURDIR)'; then \
    true; \
  else \
    false; \
  fi; \
}
am__make_dryrun = (target_option=n; $(am__make_running_with_option))
am__make_keepgoing = (target_option=k; $(am__make_running_with_option))
all:
	+@if $(am__make_dryrun); then echo dry; else echo wet; fi
	+@if $(am__make_keepgoing); then echo keep; else echo stop; fi
EOF
$MAKE
$MAKE -n | grep -v '^if'
$MAKE -k

#### .NOEXPORT, .MAKE and .PRECIOUS are accepted
cat > Makefile <<'EOF'
all:
	@echo ok
.MAKE: all
.PRECIOUS: Makefile
.NOEXPORT:
EOF
$MAKE

#### %:: rules that cancel the built-in RCS and SCCS rules
cat > Makefile <<'EOF'
all: file
	@echo done
file:
	@echo made file
%:: %,v
%:: RCS/%,v
%:: RCS/%
%:: s.%
EOF
$MAKE

#### suffix rules for the test harness (.log.trs, .test.log)
cat > Makefile <<'EOF'
.SUFFIXES:
.SUFFIXES: .log .test .trs
TESTS = one.test two.test
TEST_LOGS = $(TESTS:.test=.log)
.test.log:
	@echo "run $< > $@"
	@sh $< > $@
check: $(TEST_LOGS)
	@cat $(TEST_LOGS)
EOF
echo 'echo one ran' > one.test
echo 'echo two ran' > two.test
$MAKE check

#### the Makefile's own rule does not run when it is up to date
cat > Makefile.in <<'EOF'
in
EOF
cat > Makefile <<'EOF'
all:
	@echo all
Makefile: Makefile.in
	@echo remaking Makefile
EOF
touch -t 200001010000 Makefile.in
$MAKE

#### install with DESTDIR and a shell loop over a list
cat > Makefile <<'EOF'
bindir = /usr/bin
bin_PROGRAMS = a b
INSTALL = cp
install-binPROGRAMS:
	@list='$(bin_PROGRAMS)'; test -n "$(bindir)" || list=; \
	for p in $$list; do echo " $(INSTALL) $$p '$(DESTDIR)$(bindir)/$$p'"; done
EOF
$MAKE DESTDIR=/stage install-binPROGRAMS

#### MAKEFLAGS in a sub-make carries command-line macros
mkdir sub
printf 'all:\n\t@echo sub: $(prefix)\n' > sub/Makefile
cat > Makefile <<'EOF'
all:
	@cd sub && $(MAKE)
EOF
$MAKE -s prefix=/opt
