# The zedBSD firmware repository, awemorris/zedBSD-firmware
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# Every firmware file the build fetches (the packages below this directory,
# the Raspberry Pi 4 and 5 boot files, platform/arm64/firmware.mk and
# rpi5-firmware.mk, and the VisionFive 2 device trees, platform/riscv64/
# visionfive2/dtb.mk) is also kept, byte for byte, in one GitHub repository,
# one directory per package, together with its license texts
# (plan/ws233/phase001/phase.md).  A package
# fetches FILE from
#
#	$(ZEDBSD_FIRMWARE_REPOSITORY_URL)/PACKAGE/FILE
#
# first and from its original upstream only when that fetch fails.  Either
# way the size and SHA-256 every package declares are checked, so the repository
# changes where the bytes come from, never which bytes are accepted.
#
# The revision is a full commit SHA of the repository.  While it is empty
# no package consults the mirror and each one fetches from its upstream
# alone, as before the repository existed.
override ZEDBSD_FIRMWARE_REPOSITORY_REVISION := 43662bd72419deb927af6d533d90fe72aa158c74
override ZEDBSD_FIRMWARE_REPOSITORY_BASE_URL := https://raw.githubusercontent.com/awemorris/zedBSD-firmware

ifneq ($(ZEDBSD_FIRMWARE_REPOSITORY_REVISION),)
ifneq ($(shell printf '%s' '$(ZEDBSD_FIRMWARE_REPOSITORY_REVISION)' | grep -Ex '[0-9a-f]{40}'),$(ZEDBSD_FIRMWARE_REPOSITORY_REVISION))
$(error ZEDBSD_FIRMWARE_REPOSITORY_REVISION must be a full 40-digit commit SHA)
endif
endif

override ZEDBSD_FIRMWARE_REPOSITORY_URL := $(if $(ZEDBSD_FIRMWARE_REPOSITORY_REVISION),$(ZEDBSD_FIRMWARE_REPOSITORY_BASE_URL)/$(ZEDBSD_FIRMWARE_REPOSITORY_REVISION))

# License acceptance (plan/ws233/phase001/phase.md, section 4c).  The first
# time a package's firmware is fetched, license-agree.sh shows what it is,
# its copyright, the whole license text, and asks yes/no; an accepted text is
# recorded in CACHE-ROOT/license-accepted and not asked about again.
#
# $(call ZEDBSD_FIRMWARE_LICENSE_COMMAND,PACKAGE,CACHE-ROOT,NOTICE,LICENSE-NAME,SIZE,SHA-256,REPOSITORY-URL,UPSTREAM-URL,FETCH)
override ZEDBSD_FIRMWARE_TREE := $(if $(ZEDBSD_TOPLEVEL_BUILD),userland/firmware,$(ZEDBSD_REPO_ROOT)/userland/firmware)
ZEDBSD_FIRMWARE_LICENSE_COMMAND = sh '$(ZEDBSD_FIRMWARE_TREE)/license-agree.sh' '$(1)' '$(2)/license-accepted' '$(3)' '$(4)' '$(5)' '$(6)' '$(7)' '$(8)' $(9)

# GNU make gives the standard input to only one of the jobs it runs in
# parallel, so a recipe cannot rely on reading the answer under make -j.
# Make itself still reads its standard input while it parses the makefiles,
# one $(shell) at a time.  So a goal that needs the firmware asks there, in
# the top-level make (not a sub-make, not -n), for a package whose cache is
# missing: a build goal (none, which is disk-image, or one listed below) for
# every selected package, and PACKAGE or PACKAGE-license for that package.
# Other goals (clean, menuconfig, list-*, host tests, release-info, ...) never
# ask.  The fetch recipe asks too, for a goal not covered here.
#
# $(call ZEDBSD_FIRMWARE_ASK_AT_PARSE,PACKAGE,SELECTED,CACHE-DIR), SELECTED
# not empty when the configuration selects PACKAGE.
override ZEDBSD_FIRMWARE_BUILD_GOALS := all disk-image world bootloader rootfs \
	rootfs-bin rootfs-usr run check-disk-image download firmware-licenses %.img
ZEDBSD_FIRMWARE_ASK_AT_PARSE = $(and $(ZEDBSD_TOPLEVEL_BUILD),$(filter 0,$(MAKELEVEL)),$(if $(findstring n,$(firstword -$(MAKEFLAGS))),,y),$(if $(MAKECMDGOALS),$(or $(and $(2),$(filter $(ZEDBSD_FIRMWARE_BUILD_GOALS),$(MAKECMDGOALS))),$(filter $(1) $(1)-license,$(MAKECMDGOALS))),$(2)),$(if $(wildcard $(3)),,y))

# One field of the entry NAME in a CACHE-NAME|...|... list:
# $(call ZEDBSD_FIRMWARE_FIELD,NAME,FIELD-NUMBER,LIST)
ZEDBSD_FIRMWARE_FIELD = $(word $(2),$(subst |, ,$(filter $(1)|%,$(3))))

# "yes | make firmware-licenses" settles every selected package's license
# before a build (CI and the test image scripts).  Each package adds its own
# PACKAGE-license target here when it is selected.
.PHONY: firmware-licenses
firmware-licenses:

# "make firmware-licenses-pending" asks nothing: for each selected package
# whose license the first fetch would ask about, it fetches and checks the
# license text and prints one tab-separated line (license-agree.sh, mode
# pending) for a front end that asks in its own way (tools/menuconfig.py).
.PHONY: firmware-licenses-pending
firmware-licenses-pending:
