# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The standalone package interface of the userland.
#
# A package's Makefile declares what it builds and includes this file:
#
#   PROG := cat                         one program
#   SRCS := main.c
#   include ../package.mk
#
#   PROGS := ssh sshd                   several programs; each one's
#   ssh_SRCS := ...                     variables carry its name
#   sshd_SRCS := ...
#   include ../package.mk
#
#   SHLIB := libz-compat                a shared library (SO_NAME names
#   LIB := curses                       the file), a static library
#
# The other variables of a declaration are CLASS, REQUIRE, MODE, DATA,
# HEADERS and INSTALL_DIR (name_CLASS and so on under PROGS).  SRCS are
# relative to the package's directory.
#
# The older USERLAND_PACKAGE call with its fifteen arguments makes the same
# rules, so that the packages move to the declarations one at a time.
#
# Each source is compiled to its own object, which follows its headers
# (-MMD -MP) and the compile flags (a stamp), and only what changed is
# compiled again.  V=1 shows every command.
#

# The package's own make only: the top-level build reads none of this.
ifndef TOPLEVEL_BUILD

.DEFAULT_GOAL := all

################################################
# Output
################################################

# V=1 shows every command; otherwise each step writes one short line.
ifeq ($(V),1)
Q :=
standalone_log =
else
Q := @
standalone_log = @printf '  %-7s %s\n' '$(1)' '$(2)'
endif

################################################
# Environment
################################################

BASE_PACKAGE_MK := $(lastword $(MAKEFILE_LIST))
REPO_ROOT := $(abspath $(dir $(BASE_PACKAGE_MK))/../..)
ZEDBSD_REPO_ROOT := $(REPO_ROOT)
PREFIX ?= /usr/local
DESTDIR ?=
INSTALL ?= install
STANDALONE_CONFIG ?= $(REPO_ROOT)/config.mk
comma := ,

# make's own default compiler is not the target's: the tree's clang takes its place.
ifeq ($(origin CC),default)
CC := $(REPO_ROOT)/build/llvm/bin/clang
endif

# A caller that passed an empty CC gets the tree's clang too.
ifeq ($(strip $(CC)),)
CC := $(REPO_ROOT)/build/llvm/bin/clang
endif

# make's own default nm is not the target's: the tree's llvm-nm takes its place.
ifeq ($(origin NM),default)
NM := $(REPO_ROOT)/build/llvm/bin/llvm-nm
endif

# A caller that passed an empty NM gets the tree's llvm-nm too.
ifeq ($(strip $(NM)),)
NM := $(REPO_ROOT)/build/llvm/bin/llvm-nm
endif

AR ?= $(REPO_ROOT)/build/llvm/bin/llvm-ar

# Acquisition and patch preparation must also work in a freshly cloned source
# tree, before menuconfig has produced config.mk.  Compilation and installation
# retain the existing configured-target requirement: a make without goals
# builds and needs the configuration, a make that only downloads or patches
# goes on without one, and any other goal needs it.
ifeq ($(strip $(MAKECMDGOALS)),)
include $(STANDALONE_CONFIG)
else ifeq ($(strip $(filter-out download patch,$(MAKECMDGOALS))),)
-include $(STANDALONE_CONFIG)
else
include $(STANDALONE_CONFIG)
endif

USERLAND_PACKAGE_LIFECYCLE := 1
include $(dir $(BASE_PACKAGE_MK))../download.mk

################################################
# Platform
################################################

STANDALONE_PLATFORM_DIR = $(strip \
	$(if $(filter sun4u,$(PLATFORM)),sparcv9,\
	$(PLATFORM)))
STANDALONE_TARGET_TRIPLE = $(strip \
	$(if $(filter rpi4 rpi5 arm64,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),aarch64-unknown-zedbsd,\
	$(if $(filter vf2 riscv64,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),riscv64-unknown-zedbsd,\
	$(if $(filter i386 pcat pc98 pc9821 pc9801,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),i386-unknown-zedbsd,\
	$(if $(filter sun4u sparcv9,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),sparc64-unknown-zedbsd,\
	x86_64-unknown-zedbsd)))))
STANDALONE_ARCH_CFLAGS = $(strip \
	$(if $(filter rpi4 rpi5 arm64,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),-march=armv8-a -mno-outline-atomics -DHAL_ARCH_ARM64 -DKERN_USER_ABI_AARCH64 -DKERN_USER_ABI_LP64,\
	$(if $(filter vf2 riscv64,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),-march=rv64gc -mabi=lp64d -mno-relax -DHAL_ARCH_RISCV64 -DKERN_USER_ABI_RISCV64 -DKERN_USER_ABI_LP64,\
	$(if $(filter i386 pcat pc98 pc9821 pc9801,$(PLATFORM) $(STANDALONE_PLATFORM_DIR)),-m32 -march=i386 -DHAL_ARCH_I386 -DKERN_USER_ABI_ILP32,\
	-m64 -march=x86-64 -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64))))

# Where the platform's build puts what the packages make: the top-level
# build's directory (BUILD), or build/PLATFORM for a package made on its own.
STANDALONE_BUILD_DIR = $(if $(BUILD),$(abspath $(BUILD)),$(REPO_ROOT)/build/$(STANDALONE_PLATFORM_DIR))
STANDALONE_SYSROOT = $(STANDALONE_BUILD_DIR)/sysroot
STANDALONE_DYNAMIC_DIR = $(STANDALONE_BUILD_DIR)/dynamic
STANDALONE_LIB_DIR = $(STANDALONE_BUILD_DIR)/lib

# Where install puts a program (INSTALL_DIR under PREFIX, or at the root when PREFIX is /).
standalone_bindir = $(strip $(if $(filter /,$(PREFIX)),\
	/$(1),$(patsubst %/,%,$(PREFIX))/$(1)))
STANDALONE_INSTALL_LIBDIR = $(if $(filter /,$(PREFIX)),/lib,$(patsubst %/,%,$(PREFIX))/lib)
STANDALONE_INSTALL_INCLUDEDIR = $(if $(filter /,$(PREFIX)),/include,$(patsubst %/,%,$(PREFIX))/include)
STANDALONE_TERMINFO_DIR = $(strip $(if $(filter /,$(PREFIX)),\
	/lib/terminfo,$(patsubst %/,%,$(PREFIX))/share/terminfo))

################################################
# Compile and link flags
################################################

# A program: position-independent, freestanding, against the sysroot.
STANDALONE_PROGRAM_CFLAGS = \
	--target=$(STANDALONE_TARGET_TRIPLE) --sysroot="$(STANDALONE_SYSROOT)" \
	$(STANDALONE_ARCH_CFLAGS) \
	-nostdinc -isystem "$(STANDALONE_SYSROOT)/usr/include" \
	-I"$(REPO_ROOT)/include" -I"$(REPO_ROOT)/src" -I"$(REPO_ROOT)" -I. \
	-ffreestanding -fno-stack-protector -fPIC -Os

# A program's link: a PIE that loads /lib/libc.so through /lib/ld.so.
STANDALONE_PROGRAM_LDFLAGS = \
	--target=$(STANDALONE_TARGET_TRIPLE) --sysroot="$(STANDALONE_SYSROOT)" \
	$(STANDALONE_ARCH_CFLAGS) \
	-nostdlib -pie -Wl,--no-relax -Wl,--gc-sections \
	-Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
	-Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
	-Wl,--dynamic-linker=/lib/ld.so

# A shared library's objects.
STANDALONE_SHLIB_CFLAGS = \
	--target=$(STANDALONE_TARGET_TRIPLE) --sysroot="$(STANDALONE_SYSROOT)" \
	$(STANDALONE_ARCH_CFLAGS) \
	-fPIC \
	-nostdinc -isystem "$(STANDALONE_SYSROOT)/usr/include" \
	-I"$(REPO_ROOT)/include" -I"$(REPO_ROOT)/src" -I"$(REPO_ROOT)" -I.

# A shared library's link.
STANDALONE_SHLIB_LDFLAGS = \
	--target=$(STANDALONE_TARGET_TRIPLE) --sysroot="$(STANDALONE_SYSROOT)" \
	$(STANDALONE_ARCH_CFLAGS) \
	-fPIC -shared \
	-Wl,--hash-style=both -Bsymbolic-functions \
	-Wl,-z,now,-z,relro,-z,separate-code,-z,stack-size=0x100000

# A static library's objects take the caller's flags (the top-level build's).
STANDALONE_LIB_CFLAGS = \
	$(CPPFLAGS) $(CFLAGS) -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$(REPO_ROOT)"

################################################
# Package-specific additions
################################################

#
# These belong in the packages' own declarations and move there as the
# packages are converted: the shared sources of the basic and network
# programs, the line editor, and the shared library names that differ from
# their packages' directories.
#

# The files among the arguments that exist, as paths from the top of the tree.
standalone_existing = $(patsubst $(REPO_ROOT)/%,%,$(wildcard $(addprefix $(REPO_ROOT)/,$(1))))

# A program's extra sources: (1) name, (2) class, (3) requirements.
standalone_extra_sources = $(strip \
	$(if $(filter basic,$(2)),$(call standalone_existing,userland/base/common/command.c userland/base/common/pager.c)) \
	$(if $(filter network,$(2)),$(if $(call standalone_existing,userland/base/net/netutil.c),userland/base/net/netutil.c userland/base/net/dhcp.c)) \
	$(if $(or $(filter sh,$(1)),$(filter base/libedit,$(3))),$(call standalone_existing,userland/base/libedit/readline.c)))

# A program's extra compile flags: (1) name, (3) requirements.
standalone_extra_cflags = $(if $(or $(filter sh,$(1)),$(filter base/libedit,$(3))),-I$(REPO_ROOT)/userland/base/libedit)

# The shared library a requirement links, or nothing.
standalone_required_so = $(strip \
	$(if $(filter base/libedit,$(1)),,\
	$(if $(filter desktop/xserver,$(1)),libGLX.so,\
	$(if $(findstring lib,$(1)),\
	$(if $(filter desktop/libwayland desktop/libwayland-client,$(1)),libwayland-client.so,\
	$(if $(filter desktop/libegl,$(1)),libEGL.so,\
	$(if $(filter desktop/libglesv2,$(1)),libGLESv2.so,\
	$(if $(filter desktop/libGL,$(1)),libGL.so,\
	$(notdir $(1)).so))))))))

# The package directory that makes a requirement's shared library.
standalone_required_dir = $(if $(filter desktop/xserver,$(1)),desktop/xserver/libGLX,$(1))

# The required shared libraries whose packages are in the tree: (1) requirements.
standalone_makeable_so = $(strip $(foreach req,$(1),$(if $(call standalone_required_so,$(req)),\
	$(if $(wildcard $(REPO_ROOT)/userland/$(call standalone_required_dir,$(req))/Makefile),\
	$(call standalone_required_so,$(req))))))

#
# A required shared library missing from the build is made by its package,
# once per make: (1) library, (2) package directory.  The top-level build
# normally made it first (mk/userland.mk orders the packages); this rule
# covers a package made on its own.  A requirement whose library is still
# missing at the link is left out of it, as before.
#
define STANDALONE_REQUIRED_RULE
# The first program that requires the library defines its rule.
ifndef STANDALONE_REQUIRED_RULE_$(1)
STANDALONE_REQUIRED_RULE_$(1) := 1
$$(STANDALONE_DYNAMIC_DIR)/$(1):
	$$(call standalone_log,MAKE,userland/$(2))
	$$(Q)$$(MAKE) -C "$$(REPO_ROOT)/userland/$(2)"
endif

endef

# The link options of the required libraries that exist (a command for the recipe's shell).
standalone_required_libs = $$$$(for so in $(1); do test -f "$(STANDALONE_DYNAMIC_DIR)/$$$$so" && printf ' -l:%s' "$$$$so"; done)

################################################
# The rules of one package
################################################

#
# One program, library or data set: (1) name, (2) shared library name,
# (3) class, (4) sources from the top of the tree, (5) requirements,
# (6) mode, (7) type (program, shared-library, library, data, package or
# custom), (8) data (DESTINATION=SOURCE), (9) headers, (10) install
# directory.  Everything it defines carries the name, so a Makefile may
# make several.
#
define STANDALONE_RULES
STANDALONE_WORK_DIR_$(1) = $$(STANDALONE_BUILD_DIR)/packages/$(1)
STANDALONE_OBJ_DIR_$(1) = $$(STANDALONE_WORK_DIR_$(1))/obj
STANDALONE_FLAGS_STAMP_$(1) = $$(STANDALONE_WORK_DIR_$(1))/.flags
STANDALONE_SOURCES_$(1) := $(4) $(if $(filter program,$(7)),$(call standalone_extra_sources,$(1),$(3),$(5)))
STANDALONE_OBJECTS_$(1) = $$(foreach src,$$(STANDALONE_SOURCES_$(1)),$$(STANDALONE_OBJ_DIR_$(1))/$$(if $$(filter /%,$$(src)),_abs)$$(src:.c=.o))
STANDALONE_SO_LIST_$(1) := $(foreach req,$(5),$(call standalone_required_so,$(req)))

# The object compile flags of the type.
ifeq ($(7),program)
STANDALONE_CFLAGS_$(1) = $$(STANDALONE_PROGRAM_CFLAGS) $(call standalone_extra_cflags,$(1),,$(5))
else ifeq ($(7),shared-library)
STANDALONE_CFLAGS_$(1) = $$(STANDALONE_SHLIB_CFLAGS)
else
STANDALONE_CFLAGS_$(1) = $$(STANDALONE_LIB_CFLAGS)
endif

# The required shared libraries whose packages are in the tree, each made once if missing.
$(foreach req,$(5),$(if $(call standalone_makeable_so,$(req)),$(call STANDALONE_REQUIRED_RULE,$(call standalone_required_so,$(req)),$(call standalone_required_dir,$(req)))))

# Each source to its own object, following its headers and the flags; the sources are prepared first.
$$(STANDALONE_OBJ_DIR_$(1))/%.o: $$(REPO_ROOT)/%.c $$(STANDALONE_FLAGS_STAMP_$(1)) | patch
	$$(call standalone_log,CC,$$*.c)
	$$(Q)mkdir -p "$$(@D)"
	$$(Q)$$(CC) $$(STANDALONE_CFLAGS_$(1)) -MMD -MP -c $$< -o $$@

# A source given by its absolute path (one made outside the tree).
$$(STANDALONE_OBJ_DIR_$(1))/_abs/%.o: /%.c $$(STANDALONE_FLAGS_STAMP_$(1)) | patch
	$$(call standalone_log,CC,/$$*.c)
	$$(Q)mkdir -p "$$(@D)"
	$$(Q)$$(CC) $$(STANDALONE_CFLAGS_$(1)) -MMD -MP -c $$< -o $$@

# The flags the objects were compiled with, replaced only when they change.
$$(STANDALONE_FLAGS_STAMP_$(1)): FORCE
	$$(Q)mkdir -p "$$(@D)"
	$$(Q)printf '%s\n' '$$(CC) $$(STANDALONE_CFLAGS_$(1))' > $$@.tmp
	$$(Q)if cmp -s $$@.tmp $$@; then rm -f $$@.tmp; else mv -f $$@.tmp $$@; fi

-include $$(STANDALONE_OBJECTS_$(1):.o=.d)

# A program: linked against libc.so, with its mode recorded for the root.
ifeq ($(7),program)
STANDALONE_OUTPUT_$(1) = $$(STANDALONE_BUILD_DIR)/$(10)/$(1)
$$(STANDALONE_OUTPUT_$(1)): $$(STANDALONE_OBJECTS_$(1)) | $$(addprefix $$(STANDALONE_DYNAMIC_DIR)/,$(call standalone_makeable_so,$(5)))
	$$(call standalone_log,LD,$(10)/$(1))
	$$(Q)mkdir -p "$$(@D)" "$$(STANDALONE_WORK_DIR_$(1))"
	$$(Q)$$(CC) $$(STANDALONE_PROGRAM_LDFLAGS) \
		"$$(STANDALONE_SYSROOT)/usr/lib/crt1.o" \
		$$(STANDALONE_OBJECTS_$(1)) \
		-L"$$(STANDALONE_DYNAMIC_DIR)" -Wl,-rpath-link,"$$(STANDALONE_DYNAMIC_DIR)" \
		$(call standalone_required_libs,$$(STANDALONE_SO_LIST_$(1))) -l:libm.so -l:libc.so \
		-o $$@
	$$(Q)printf '%s %s\n' "/$(10)/$(1)" "$(6)" > "$$(STANDALONE_WORK_DIR_$(1))/mode"
endif

# A shared library: in the dynamic directory, and a copy among the libraries.
ifeq ($(7),shared-library)
STANDALONE_OUTPUT_$(1) = $$(STANDALONE_DYNAMIC_DIR)/$(2).so
$$(STANDALONE_OUTPUT_$(1)): $$(STANDALONE_OBJECTS_$(1)) $$(wildcard exports.map) | $$(addprefix $$(STANDALONE_DYNAMIC_DIR)/,$(call standalone_makeable_so,$(5)))
	$$(call standalone_log,SHLIB,$(2).so)
	$$(Q)mkdir -p "$$(@D)" "$$(STANDALONE_LIB_DIR)"
	$$(Q)$$(CC) $$(STANDALONE_SHLIB_LDFLAGS) \
		$$(STANDALONE_OBJECTS_$(1)) \
		-Wl,-soname,"$(2).so" \
		$$(if $$(wildcard exports.map),-Wl$$(comma)--version-script=exports.map) \
		-L"$$(STANDALONE_DYNAMIC_DIR)" -L"$$(STANDALONE_SYSROOT)/usr/lib" \
		$(call standalone_required_libs,$$(STANDALONE_SO_LIST_$(1))) -l:libc.so -l:libm.so \
		-o $$@
	$$(Q)cp -f $$@ "$$(STANDALONE_LIB_DIR)/$(2).so"
endif

# A static library, when it has sources.
ifeq ($(7)$(if $(strip $(4)),y),libraryy)
STANDALONE_OUTPUT_$(1) = $$(STANDALONE_LIB_DIR)/lib$(1).a
$$(STANDALONE_OUTPUT_$(1)): $$(STANDALONE_OBJECTS_$(1))
	$$(call standalone_log,AR,lib$(1).a)
	$$(Q)mkdir -p "$$(@D)"
	$$(Q)rm -f $$@
	$$(Q)$$(AR) rcs $$@ $$(STANDALONE_OBJECTS_$(1))
endif

# The data files, each copied when its source changes.
STANDALONE_DATA_OUTPUTS_$(1) :=
$(foreach entry,$(8),$(call STANDALONE_DATA_RULE,$(1),$(patsubst /lib/terminfo/%,/share/terminfo/%,$(firstword $(subst =, ,$(entry)))),$(word 2,$(subst =, ,$(entry)))))

# The package is built: the stamps the top-level build reads.
.PHONY: standalone-build-$(1)
standalone-build-$(1): $$(STANDALONE_OUTPUT_$(1)) $$(STANDALONE_DATA_OUTPUTS_$(1))
	$$(Q)mkdir -p "$$(STANDALONE_WORK_DIR_$(1))"
	$$(Q)touch "$$(STANDALONE_WORK_DIR_$(1))/.stamp"
	$$(Q)if test -n "$$(notdir $$(CURDIR))" && test "$$(notdir $$(CURDIR))" != "$(1)"; then \
		mkdir -p "$$(STANDALONE_BUILD_DIR)/packages/$$(notdir $$(CURDIR))"; \
		touch "$$(STANDALONE_BUILD_DIR)/packages/$$(notdir $$(CURDIR))/.stamp"; \
	fi
build: standalone-build-$(1)

# A package or custom type's own target, which its Makefile defines, is built too.
ifneq ($(filter package custom,$(7)),)
build: $(1)
endif

# Installation under DESTDIR and PREFIX.
.PHONY: standalone-install-$(1)
standalone-install-$(1): standalone-build-$(1)

# A program goes to its directory with its mode.
ifeq ($(7),program)
	$$(Q)$$(INSTALL) -d "$$(DESTDIR)$$(call standalone_bindir,$(10))"
	$$(Q)$$(INSTALL) -m "$(6)" "$$(STANDALONE_OUTPUT_$(1))" "$$(DESTDIR)$$(call standalone_bindir,$(10))/$(1)"
endif

# A shared library goes among the libraries.
ifeq ($(7),shared-library)
	$$(Q)$$(INSTALL) -d "$$(DESTDIR)$$(STANDALONE_INSTALL_LIBDIR)"
	$$(Q)$$(INSTALL) -m 0644 "$$(STANDALONE_OUTPUT_$(1))" "$$(DESTDIR)$$(STANDALONE_INSTALL_LIBDIR)/$(2).so"
endif

# A static library with sources goes among the libraries.
ifeq ($(7)$(if $(strip $(4)),y),libraryy)
	$$(Q)$$(INSTALL) -d "$$(DESTDIR)$$(STANDALONE_INSTALL_LIBDIR)"
	$$(Q)$$(INSTALL) -m 0644 "$$(STANDALONE_OUTPUT_$(1))" "$$(DESTDIR)$$(STANDALONE_INSTALL_LIBDIR)/lib$(1).a"
endif

# The headers, then the data files (terminfo under PREFIX).
	$$(Q)set -e; for header in $(9); do \
		$$(INSTALL) -d "$$(DESTDIR)$$(STANDALONE_INSTALL_INCLUDEDIR)"; \
		$$(INSTALL) -m 0644 "$$(REPO_ROOT)/$$$$header" "$$(DESTDIR)$$(STANDALONE_INSTALL_INCLUDEDIR)/$$$${header##*/}"; \
	done
	$$(Q)set -e; for entry in $(8); do \
		destination=$$$${entry%%=*}; source=$$$${entry#*=}; \
		case "$$$$destination" in \
		/lib/terminfo/*) destination="$$(STANDALONE_TERMINFO_DIR)/$$$${destination##*/}" ;; \
		esac; \
		case "$$$$source" in \
		/*) ;; \
		*) source="$$(REPO_ROOT)/$$$$source" ;; \
		esac; \
		$$(INSTALL) -d "$$(DESTDIR)$$$${destination%/*}"; \
		$$(INSTALL) -m 0644 "$$$$source" "$$(DESTDIR)$$$$destination"; \
	done
install: standalone-install-$(1)

# Removal of what the package made.
.PHONY: standalone-clean-$(1)
standalone-clean-$(1):
	$$(Q)rm -rf "$$(STANDALONE_OBJ_DIR_$(1))" "$$(STANDALONE_FLAGS_STAMP_$(1))" "$$(STANDALONE_WORK_DIR_$(1))/.stamp" \
		"$$(STANDALONE_BUILD_DIR)/packages/$$(notdir $$(CURDIR))/.stamp"
clean: standalone-clean-$(1)
endef

#
# One data file: (1) package, (2) destination, (3) source from the top of
# the tree or absolute.  It is copied into the build when its source
# changes, readable by everyone as install makes it (a fetched source
# keeps the fetcher's private mode), and the root takes it from there.
#
define STANDALONE_DATA_RULE
STANDALONE_DATA_OUTPUTS_$(1) += $$(STANDALONE_BUILD_DIR)$(2)
$$(STANDALONE_BUILD_DIR)$(2): $(if $(filter /%,$(3)),$(3),$$(REPO_ROOT)/$(3))
	$$(call standalone_log,DATA,$(2))
	$$(Q)mkdir -p "$$(@D)"
	$$(Q)cp -f $$< $$@
	$$(Q)chmod 0644 $$@

endef

################################################
# The declarations
################################################

# The type a USERLAND_PACKAGE call names, or the one its class implies.
standalone_type = $(strip $(if $(strip $(2)),$(2),$(if $(filter library,$(1)),library,$(if $(filter package,$(1)),package,program))))

#
# The older call: (1) name, (2) shared library name, (5) class, (6) sources,
# (10) requirements, (11) mode, (12) type, (13) data, (14) headers, (15)
# install directory.  The other arguments are not read.
#
define USERLAND_PACKAGE
$(call STANDALONE_RULES,$(strip $(1)),$(strip $(if $(filter shared-library,$(strip $(12))),$(if $(strip $(2)),$(2),$(1)),$(1))),$(strip $(5)),$(strip $(6)),$(strip $(10)),$(strip $(if $(strip $(11)),$(11),0755)),$(call standalone_type,$(5),$(12)),$(strip $(13)),$(strip $(14)),$(strip $(if $(strip $(15)),$(15),bin)))
endef

# A declared source, relative to the package's directory, as a path from the top of the tree.
standalone_declared_sources = $(patsubst $(REPO_ROOT)/%,%,$(abspath $(1)))

# PROG, SHLIB or LIB: one declaration.
ifneq ($(strip $(PROG)$(SHLIB)$(LIB)),)
STANDALONE_DECLARED := $(firstword $(PROG) $(SHLIB) $(LIB))
STANDALONE_DECLARED_TYPE := $(if $(PROG),program,$(if $(SHLIB),shared-library,library))
$(eval $(call STANDALONE_RULES,$(STANDALONE_DECLARED),$(if $(SO_NAME),$(SO_NAME),$(STANDALONE_DECLARED)),$(CLASS),$(call standalone_declared_sources,$(SRCS)),$(REQUIRE),$(if $(MODE),$(MODE),0755),$(STANDALONE_DECLARED_TYPE),$(DATA),$(HEADERS),$(if $(INSTALL_DIR),$(INSTALL_DIR),bin)))
endif

# PROGS: several programs, each one's variables prefixed with its name.
$(foreach prog,$(PROGS),$(eval $(call STANDALONE_RULES,$(prog),$(prog),$($(prog)_CLASS),$(call standalone_declared_sources,$($(prog)_SRCS)),$($(prog)_REQUIRE),$(if $($(prog)_MODE),$($(prog)_MODE),0755),program,$($(prog)_DATA),$($(prog)_HEADERS),$(if $($(prog)_INSTALL_DIR),$($(prog)_INSTALL_DIR),bin))))

################################################
# The goals
################################################

.PHONY: all build install clean FORCE
FORCE:

all: build

# The sources are prepared before anything a declaration adds to build.
build: patch

endif
