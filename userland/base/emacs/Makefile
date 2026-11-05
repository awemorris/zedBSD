# zedBSD userland package: REmacs (userland/base/emacs, ws129, 2026-10-04).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# REmacs (github.com/awemorris/remacs at 1a72439) is part of this tree, under
# the zlib license of its author.  It is built from here: the editor's Noct
# sources are compiled by the host's Noct into one Noct application, which is
# installed as the command /bin/emacs (2026-10-04 user: "remacs のコマンド名は
# /bin/emacs でお願いします").  The application starts with the line
# "#!/usr/bin/noct", which Noct writes and strips itself, so the kernel runs
# it with the interpreter: /usr/bin/noct is a link to the target's /bin/noct.
# The bundled SKK dictionaries go to /usr/share/remacs: the compact
# skkjisyo.dic and the fuller skkjisyo-x.dic (dict/SKK-JISYO.X), which the
# editor looks in after the compact one (BUG-302).  Both are REmacs's own
# clean-room dictionaries under the same zlib license.  REmacs's
# own Makefile is Makefile.remacs; it runs in a copy under BUILD, so the tree
# stays clean.
ZEDBSD_REMACS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(ZEDBSD_REMACS_DIR)../../../build-jobs.mk
include $(ZEDBSD_REMACS_DIR)../package.mk
include $(ZEDBSD_REMACS_DIR)../noct/version.mk

REQUIRE = base/noct
# Emacs (REmacs) is written in Noct, so choosing it chooses the interpreter
# that runs it.  The menu files it with the base programs.
$(eval $(call USERLAND_PACKAGE,emacs,emacs,*,n,package,,y,\
	base,base/emacs,$(REQUIRE)))
REQUIRE =

REMACS_SOURCE_DIR := userland/base/emacs
REMACS_SOURCES := $(wildcard $(REMACS_SOURCE_DIR)/editor/*.noct) \
	$(REMACS_SOURCE_DIR)/src/napi.def $(REMACS_SOURCE_DIR)/tools/gen-napi.py \
	$(REMACS_SOURCE_DIR)/tools/build-nap.sh $(REMACS_SOURCE_DIR)/Makefile.remacs
REMACS_WORK_DIR := $(BUILD)/packages/base/emacs/work
# The host's Noct is noct's Makefile's (userland/base/noct), which is read after
# this one (the packages are read in name order), so its stamp is named here from
# the same version and patch level; the recipe checks the two agree.
REMACS_HOST_NOCT_STAMP := build/host-noct-state/built-$(ZEDBSD_NOCT_VERSION)-$(ZEDBSD_NOCT_PATCH_LEVEL)-process
REMACS_NAP := $(BUILD)/packages/base/emacs/remacs.nap
REMACS_DICTIONARY := $(REMACS_SOURCE_DIR)/dict/SKK-JISYO.remacs
REMACS_DICTIONARY_X := $(REMACS_SOURCE_DIR)/dict/SKK-JISYO.X

# These recursive additions are evaluated after config.mk dependencies have
# been expanded.
ZEDBSD_PACKAGE_INPUTS += $(if $(filter emacs,$(ZEDBSD_USER_PROGRAMS)),\
	$(REMACS_NAP) $(REMACS_DICTIONARY) $(REMACS_DICTIONARY_X))
ZEDBSD_PACKAGE_FILES += $(if $(filter emacs,$(ZEDBSD_USER_PROGRAMS)),\
	--file /bin/emacs=$(REMACS_NAP) \
	--mode /bin/emacs=0755 \
	--file /usr/share/remacs/skkjisyo.dic=$(REMACS_DICTIONARY) \
	--mode /usr/share/remacs/skkjisyo.dic=0644 \
	--file /usr/share/remacs/skkjisyo-x.dic=$(REMACS_DICTIONARY_X) \
	--mode /usr/share/remacs/skkjisyo-x.dic=0644)
ZEDBSD_PACKAGE_LINKS += $(if $(filter emacs,$(ZEDBSD_USER_PROGRAMS)),\
	/usr/bin/noct=/bin/noct)

# The bytecode, built in a fresh copy of the sources with the host's Noct.
$(REMACS_NAP): $(REMACS_SOURCES) $(REMACS_HOST_NOCT_STAMP)
	@test '$(REMACS_HOST_NOCT_STAMP)' = '$(NOCT_HOST_BUILD_STAMP)' || \
		{ echo 'remacs: the host Noct stamp moved to $(NOCT_HOST_BUILD_STAMP)' >&2; exit 1; }
	@rm -rf $(REMACS_WORK_DIR)
	@mkdir -p $(REMACS_WORK_DIR)
	cp -R $(REMACS_SOURCE_DIR)/editor $(REMACS_SOURCE_DIR)/src $(REMACS_SOURCE_DIR)/tools \
		$(REMACS_SOURCE_DIR)/Makefile.remacs $(REMACS_WORK_DIR)/
	$(MAKE) -C $(REMACS_WORK_DIR) -f Makefile.remacs NOCT=$(abspath $(NOCT_HOST_ARTIFACT)) build/remacs.nap
	@mkdir -p $(dir $@)
	cp -f $(REMACS_WORK_DIR)/build/remacs.nap $@
	@head -c 16 $@ | grep -qx '#!/usr/bin/noct' || \
		{ echo 'remacs: $@ does not start with #!/usr/bin/noct' >&2; rm -f $@; exit 1; }

.PHONY: emacs
emacs: $(REMACS_NAP)
