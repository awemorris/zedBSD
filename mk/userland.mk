# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Userland build rules
#

ifeq ($(strip $(CONFIG_USERLAND_PROGRAMS)),)
CONFIG_USERLAND_PROGRAMS := $(USERLAND_PROGRAMS)
endif

CONFIG_USERLAND_PROGRAMS := $(sort $(CONFIG_USERLAND_PROGRAMS) sh init cron getty syslogd networkd mount fsck sysctl net)

ALL_PROGRAM_MAKEFILES := $(sort \
	$(wildcard userland/base/*/Makefile) \
	$(wildcard userland/base/*/*/Makefile) \
	$(wildcard userland/desktop/*/Makefile) \
	$(wildcard userland/desktop/*/*/Makefile) \
	$(wildcard userland/packages/*/*/Makefile) \
	$(wildcard userland/firmware/*/Makefile) \
	$(wildcard userland/x11/*/Makefile) \
	$(wildcard userland/tests/*/Makefile))
ALL_PROGRAM_MAKEFILES := $(filter-out %/build-%/Makefile,$(ALL_PROGRAM_MAKEFILES))

pkg_aliases = $(strip $(1) \
	$(if $(filter ime,$(1)),keiland-ime) \
	$(if $(filter dict,$(1)),ime-dict-ja) \
	$(if $(filter skk-dict,$(1)),ime-dict-skk) \
	$(if $(filter libGL,$(1)),libgl) \
	$(if $(filter libwayland,$(1)),libwayland-client) \
	$(if $(filter preview,$(1)),keiland-preview) \
	$(if $(filter printd,$(1)),keiland-printd) \
	$(if $(filter installer,$(1)),install-admin) \
	$(1)-firmware $(1)-test)

SELECTED_PROGRAM_MAKEFILES := $(foreach mf, $(ALL_PROGRAM_MAKEFILES), \
	$(if $(filter $(call pkg_aliases,$(notdir $(patsubst %/,%,$(dir $(mf))))), $(CONFIG_USERLAND_PROGRAMS)), \
		$(mf)))

PROGRAM_DIRS := $(dir $(SELECTED_PROGRAM_MAKEFILES))
ALL_PROGRAM_DIRS := $(dir $(ALL_PROGRAM_MAKEFILES))

PROGRAM_STAMPS := $(foreach d,$(PROGRAM_DIRS),$(BUILD)/packages/$(notdir $(patsubst %/,%,$(d)))/.stamp)

define PROGRAM_RULE
_CHILD_DIRS_$(1) := $$(filter $(1)%/,$$(filter-out $(1),$$(ALL_PROGRAM_DIRS)))
_CHILD_SRCS_$(1) := $$(foreach c,$$(_CHILD_DIRS_$(1)),$$(wildcard $$(c)* $$(c)*/* $$(c)*/*/*) $$(patsubst %/,%,$$(c)))
_ALL_SRCS_$(1) := $$(wildcard $(1)* $(1)*/* $(1)*/*/*)
_DIR_NAMES_$(1) := $$(patsubst %/.,%,$$(wildcard $$(addsuffix /.,$$(_ALL_SRCS_$(1)))))
_PKG_SRCS_$(1) := $$(filter-out $$(_CHILD_SRCS_$(1)) $$(_DIR_NAMES_$(1)),$$(_ALL_SRCS_$(1)))

$$(BUILD)/packages/$(notdir $(patsubst %/,%,$(1)))/.stamp: $$(_PKG_SRCS_$(1)) userland/base/package.mk
	@case "$(1)" in \
		userland/packages/*) \
			$$(MAKE) -C $(1) ;; \
		*) \
			$$(MAKE) -C $(1) CC="$$(CC)" CPPFLAGS="$$(USER_CPPFLAGS)" CFLAGS="$$(USER_CFLAGS)" AS="$$(AS)" LD="$$(LD)" RANLIB="$$(RANLIB)" NM="$$(NM)" OBJCOPY="$$(OBJCOPY)" OBJDUMP="$$(OBJDUMP)" READELF="$$(READELF)" STRIP="$$(STRIP)" ;; \
	esac

.PHONY: $(1) $(patsubst %/,%,$(1))
$(1) $(patsubst %/,%,$(1)):
	@case "$(1)" in \
		userland/packages/*) \
			$$(MAKE) -C $(1) ;; \
		*) \
			$$(MAKE) -C $(1) CC="$$(CC)" CPPFLAGS="$$(USER_CPPFLAGS)" CFLAGS="$$(USER_CFLAGS)" AS="$$(AS)" LD="$$(LD)" RANLIB="$$(RANLIB)" NM="$$(NM)" OBJCOPY="$$(OBJCOPY)" OBJDUMP="$$(OBJDUMP)" READELF="$$(READELF)" STRIP="$$(STRIP)" ;; \
	esac
endef

$(foreach d,$(ALL_PROGRAM_DIRS),$(eval $(call PROGRAM_RULE,$(d))))

.PHONY: packages

programs: libc $(PROGRAM_STAMPS)
