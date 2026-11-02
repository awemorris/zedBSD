# zedBSD armv7: the console root the kernel reads from its initial ramdisk.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# The root tree holds the programs of platform/armv7/userland.mk, the
# runtime, the accounts and init's configuration.  Of init's service
# definitions it carries the console's getty and the two it is ordered
# after: the system log daemon, and networking, which init passes over
# when the network daemon it requires is not in the root.  A root with the
# network daemon (a configuration that names networkd) carries its service
# and the network's configuration, which brings the loopback interface up
# and leaves a wired interface it does not name to DHCP.
ARMV7_ROOT_SERVICES := getty_console networking syslogd \
	$(filter networkd,$(ZEDBSD_USER_PROGRAMS))
ARMV7_ROOT_DATA := /etc/rc.conf=userland/base/etc/rc.conf \
	/etc/fstab=userland/base/etc/fstab \
	$(if $(filter networkd,$(ZEDBSD_USER_PROGRAMS)),\
 /etc/net.conf=userland/base/etc/net.conf) \
	$(foreach service,$(ARMV7_ROOT_SERVICES),\
 /etc/service.d/$(service)=userland/base/init/services/$(service))
# OpenSSH's programs also load libutil, zedBSD's own SSH programs
# libcrypto-compat.  The packages a configuration names
# (ZEDBSD_USER_PROGRAMS) go into the tree with their own files.
ARMV7_ROOT_RUNTIME := ld.so libc.so \
	$(if $(filter openssh,$(ZEDBSD_USER_PROGRAMS)),libutil.so) \
	$(if $(ARMV7_SSH_COMMANDS),libcrypto-compat.so)

ZEDBSD_ROOTFS_INPUTS := $(addprefix $(BUILD)/bin/,$(ARMV7_ROOT_COMMANDS)) \
	$(addprefix $(DYNAMIC_DIR)/,$(ARMV7_ROOT_RUNTIME)) \
	$(ZEDBSD_ACCOUNT_INPUTS) \
	$(foreach entry,$(ARMV7_ROOT_DATA),$(word 2,$(subst =, ,$(entry))))
ZEDBSD_ROOTFS_FILES := $(foreach command,$(ARMV7_ROOT_COMMANDS),\
 --file $(if $(filter umount,$(command)),/sbin/umount,$(call zedbsd_userland_destination,$(command)))=$(BUILD)/bin/$(command)) \
	$(foreach command,$(ARMV7_ROOT_COMMANDS),$(if $(USERLAND_$(command)_MODE),\
 --mode $(call zedbsd_userland_destination,$(command))=$(USERLAND_$(command)_MODE))) \
	$(foreach library,$(ARMV7_ROOT_RUNTIME),\
 --file /lib/$(library)=$(DYNAMIC_DIR)/$(library)) \
	$(foreach entry,$(ARMV7_ROOT_DATA),--file $(entry) \
 --mode $(word 1,$(subst =, ,$(entry)))=0644) \
	$(ZEDBSD_ACCOUNT_FILES)
$(eval $(call ZEDBSD_ROOTFS_TREE_RULE,armv7,$(ZEDBSD_ROOTFS_INPUTS),$(ZEDBSD_ROOTFS_FILES)))

.PHONY: rootfs root-image
rootfs: $(BUILD)/rootfs/.stamp

# The root disk: one MBR partition holding the UFS root made from the tree
# (platform/armv7/tools/make-armv7-root-image.py).  It is loaded over JTAG
# at about 250 KiB/s, so it is no larger than the tree and the files init,
# the log and the logins write need: 4 MiB, or 16 MiB with OpenSSH and
# OpenSSL in the tree.
# The debugging information of the programs is left out of it: the
# external packages are built with it (OpenSSH's programs are three times
# their size with it), and nothing reads it on the board.
ARMV7_ROOT_MIB ?= $(if $(filter openssh,$(ZEDBSD_USER_PROGRAMS)),16,4)
ARMV7_STRIP := $(ZEDBSD_TARGET_LLVM_BIN)/llvm-strip
$(BUILD)/root.img: $(BUILD)/rootfs/.stamp $(ARMV7_ROOT_IMAGE_TOOL) \
	tools/build/ufs_format.py tools/build/check-ufs-image.py
	find $(BUILD)/rootfs -type f -exec sh -c 'for file; do \
 magic=$$(head -c 4 "$$file" | od -An -tx1 | tr -d " "); \
 if test "$$magic" = 7f454c46; then $(ARMV7_STRIP) --strip-debug "$$file"; fi; \
 done' sh {} +
	$(PYTHON) $(ARMV7_ROOT_IMAGE_TOOL) --ufs-size-mib $(ARMV7_ROOT_MIB) \
 --tree $(BUILD)/rootfs $@
root-image: $(BUILD)/root.img

# The same tree as a raw UFS for a partition of a disk image: the root on
# the ZYBO's SD card (platform/armv7/disk-image.mk).  On the card the root
# costs no RAM, so it has room for what the system writes.
ARMV7_SD_ROOT_MIB ?= 256
$(BUILD)/root.ufs: $(BUILD)/rootfs/.stamp $(ARMV7_ROOT_IMAGE_TOOL) \
	tools/build/ufs_format.py tools/build/check-ufs-image.py
	$(PYTHON) $(ARMV7_ROOT_IMAGE_TOOL) --raw --ufs-size-mib $(ARMV7_SD_ROOT_MIB) \
 --tree $(BUILD)/rootfs $@

# The external packages of the ZYBO's network login (WS289), built on
# their own; a configuration that names them (config/ci/config-zybo.mk)
# puts them into the root.
.PHONY: armv7-packages
armv7-packages: zlib openssl openssh ca-certificates
