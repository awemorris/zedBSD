# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# crunch is the single program: the programs of the packages the
# configuration names in CONFIG_USERLAND_SINGLEBINARY, linked into one
# file, and each command's name a link to it (main.c runs the program the
# name chooses).  The commands share one text in memory and one file on the
# disk.
#
# Each of those packages' makes left an archive of each of its programs'
# objects, with their globals renamed to carry the program's name and main
# renamed to the entry the table calls, and a record of them (package.mk).
# This make reads the records, writes the table of the programs, links the
# archives with the table and main.c, and writes the links the root makes
# (mk/rootfs.mk).  The top-level build runs it after those packages' makes.
#

PROG := crunch
SRCS = main.c $(CRUNCH_TABLE)
REQUIRE = $(sort $(foreach entry,$(CRUNCH_ENTRIES),$(wordlist 3,999,$(subst :, ,$(entry)))))
INSTALL_DIR := bin

# The records of the packages, a word for each program (name:directory:requirements).
CRUNCH_ENTRIES = $(foreach package,$(STANDALONE_SINGLEBINARY),\
	$(if $(wildcard $(STANDALONE_BUILD_DIR)/packages/$(package)/single-programs),\
	$(file <$(STANDALONE_BUILD_DIR)/packages/$(package)/single-programs)))
crunch_name = $(word 1,$(subst :, ,$(1)))
crunch_directory = $(word 2,$(subst :, ,$(1)))
CRUNCH_NAMES = $(sort $(foreach entry,$(CRUNCH_ENTRIES),$(call crunch_name,$(entry))))

# The programs' archives, which the link takes besides main.c and the table.
STANDALONE_LINK_OBJECTS_crunch = $(foreach name,$(CRUNCH_NAMES),$(STANDALONE_BUILD_DIR)/packages/$(name)/single.a)

# The link folds the functions that several programs compiled alike into one copy.
STANDALONE_LINK_FLAGS_crunch := -Wl,--icf=safe

# The table and the links' record, which this make writes.
CRUNCH_TABLE = $(STANDALONE_BUILD_DIR)/packages/crunch/table.c
CRUNCH_LINKS = $(STANDALONE_BUILD_DIR)/packages/crunch/rootfs-links

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The relative target of a command's link: (1) the command's directory.
crunch_nothing :=
crunch_space := $(crunch_nothing) $(crunch_nothing)
crunch_link_target = $(if $(filter bin,$(1)),crunch,$(subst $(crunch_space),,$(foreach part,$(subst /, ,$(1)),../))bin/crunch)

#
# The table: each program's renamed main declared, and the programs in the
# byte order of their names (make's sort), which main.c searches by
# halving.  It is written every time and replaced only when it changes.
#
$(CRUNCH_TABLE): FORCE
	$(Q)mkdir -p "$(@D)"
	$(Q){ \
		printf '%s\n' '/* The programs of the single program; the build writes this file. */' '' '#include "crunch.h"' ''; \
		printf 'int %s(int, char **, char **);\n' $(foreach name,$(CRUNCH_NAMES),$(call standalone_single_entry,$(name))); \
		printf '\n%s\n' 'const struct crunch_program crunch_programs[] = {'; \
		$(foreach name,$(CRUNCH_NAMES),printf '\t{ "%s", %s },\n' '$(name)' '$(call standalone_single_entry,$(name))';) \
		printf '%s\n\n' '};'; \
		printf 'const unsigned int crunch_program_count = %s;\n' '$(words $(CRUNCH_NAMES))'; \
	} > $@.tmp
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

# The links' record: DESTINATION=TARGET for each command, which the root reads.
$(CRUNCH_LINKS): FORCE
	$(Q)mkdir -p "$(@D)"
	$(Q)printf '%s\n' $(foreach entry,$(CRUNCH_ENTRIES),'/$(call crunch_directory,$(entry))/$(call crunch_name,$(entry))=$(call crunch_link_target,$(call crunch_directory,$(entry)))') > $@.tmp
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

build: $(CRUNCH_LINKS)

# Installation puts each command's link beside the single program.
.PHONY: crunch-install-links
crunch-install-links: standalone-install-crunch
	$(Q)set -e; for link in $(foreach entry,$(CRUNCH_ENTRIES),$(call crunch_directory,$(entry))/$(call crunch_name,$(entry))=$(call crunch_link_target,$(call crunch_directory,$(entry)))); do \
		path=$${link%%=*}; target=$${link#*=}; \
		$(INSTALL) -d "$(DESTDIR)$(call standalone_bindir,$${path%/*})"; \
		ln -sfn "$$target" "$(DESTDIR)$(call standalone_bindir,$${path%/*})/$${path##*/}"; \
	done
install: crunch-install-links
