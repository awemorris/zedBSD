# ws333 p027: the WS291 p022 measurement image under the platform build system: the x86_64 defaults (graphical login,
# the session of "user" logged in at boot) with mview and its qs40 model and sysctl, root reached over SSH with the
# guest harness's key (plan/tmp/guest, plan/tools/guest/guest.py keys) and fixed host keys, as
# plan/ws291/tests/config-5330.mk had them; without the firmware packages (their licenses are the user's to accept)
# and without the applications the measurement does not start.  The nested makes of the packages read this file from
# their own directories, so every path is absolute; the platform defaults are read here first so that the program
# list can be narrowed (override keeps the defaults' second reading from adding them back).
#   make PLATFORM=x86_64 CONFIGMK=plan/ws333/tests/hw5330/config-5330-qs40.mk CONFIG_KERN_THERMAL=y \
#	CONFIG_DRIVER_X86_POWER_CONTROL=y disk
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
P027_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(P027_ROOT)/config/config.x86_64.mk
include $(P027_ROOT)/mk/platform-x86_64/defaults.mk
ZEDBSD_TEST_SCREEN_CAPTURE := y
override CONFIG_USERLAND_PROGRAMS := $(filter-out %-firmware browser libbrowser emacs mailer pdfviewer libpdf \
	videoplayer music photos phone calendar notes textedit imageview mediastorage zgears,$(CONFIG_USERLAND_PROGRAMS)) \
	mview sysctl keiland-shot

# Root's SSH key and fixed host keys, added to the platform's rootfs list.
P027_GUEST := $(P027_ROOT)/plan/tmp/guest
P027_FILES := /root/.ssh/authorized_keys=$(P027_GUEST)/id_ed25519.pub \
	$(foreach key,rsa ecdsa ed25519,/etc/ssh/ssh_host_$(key)_key=$(P027_GUEST)/ssh_host_$(key)_key \
	/etc/ssh/ssh_host_$(key)_key.pub=$(P027_GUEST)/ssh_host_$(key)_key.pub)
X86_64_ARCH_INPUTS = $(ROOTFS_INPUTS) $(foreach item,$(P027_FILES),$(lastword $(subst =, ,$(item))))
X86_64_ARCH_FILES = $(ROOTFS_FILES) $(foreach item,$(P027_FILES),--file $(item)) \
	--mode /root/.ssh=0700 --mode /root/.ssh/authorized_keys=0600 \
	$(foreach key,rsa ecdsa ed25519,--mode /etc/ssh/ssh_host_$(key)_key=0600)
