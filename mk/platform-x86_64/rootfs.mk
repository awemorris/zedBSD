# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# x86_64 rootfs Makefile
#

ROOTFS_DIRECTORIES := \
	bin sbin lib etc var root home dev boot run \
	usr/bin usr/sbin usr/libexec var/empty etc/ssh

ROOTFS_STICKY_DIRECTORIES := \
	tmp shm

PACKAGE_LINKS ?= \
	/usr/bin/noct=/bin/noct \
	/usr/lib/libz.so=libz.so.1 \
	/usr/lib/libcurl.so=libcurl.so.4

ROOTFS_CONFIG_STAMP := \
	$(BUILD)/.rootfs-config

TARGET_SYSROOT ?= \
	$(BUILD)/sysroot

ROOTFS_DEVELOPMENT ?= $(if $(CONFIG_ROOTFS_DEVELOPMENT),$(CONFIG_ROOTFS_DEVELOPMENT),y)

ifeq ($(strip $(TARGET_SYSROOT)),)
override ROOTFS_DEVELOPMENT := n
endif

ifeq ($(ROOTFS_DEVELOPMENT),y)
ROOTFS_DEVELOPMENT_INPUTS := \
	$(TARGET_SYSROOT)/.zedbsd-sysroot-complete
define ROOTFS_INSTALL_DEVELOPMENT
	umask 022; mkdir -p $(1)/usr/include $(1)/usr/lib
	umask 022; cp -R $(TARGET_SYSROOT)/usr/include/. $(1)/usr/include/
	umask 022; cp -R $(TARGET_SYSROOT)/usr/lib/. $(1)/usr/lib/
	chmod -R u+w $(1)/usr/include $(1)/usr/lib
endef
else
ROOTFS_DEVELOPMENT_INPUTS :=
define ROOTFS_INSTALL_DEVELOPMENT
	rm -rf $(1)/usr/include $(1)/usr/lib/pkgconfig $(1)/usr/share/pkgconfig
	find $(1)/lib -maxdepth 1 \
		\( \( -type l -name '*.so' \) -o \( -type f -name '*.a' \) \) \
		-exec rm -f {} +
	test ! -d $(1)/usr/lib || find $(1)/usr/lib -maxdepth 1 \
		\( \( -type l -name '*.so' \) -o \( -type f -name '*.a' \) \) \
		-exec rm -f {} +
endef
endif

define ROOTFS_TREE_RULE
$(call ROOTFS_TREE_RULE_AT,$(1),$(2),$(3),$(or $(4),$(BUILD)/rootfs))
endef

define ROOTFS_TREE_RULE_AT
$(4)/.stamp: $(ROOTFS_CONFIG_STAMP) $(2) \
	$(PACKAGE_INPUTS) $(ROOTFS_DEVELOPMENT_INPUTS)
	@rm -rf $(4)
	@umask 022; mkdir -p $(4) $(addprefix $(4)/,\
 $(ROOTFS_DIRECTORIES) $(ROOTFS_STICKY_DIRECTORIES))
	@chmod 0755 $(4) $(addprefix $(4)/,\
 $(ROOTFS_DIRECTORIES))
	@chmod 1777 $(addprefix $(4)/,\
 $(ROOTFS_STICKY_DIRECTORIES))
	@ln -s ../run $(4)/var/run
	@umask 022; printf '%s\n' '$(1)' > $(4)/lib/arch.id
	@umask 022; printf 'zedBSD ufs root v1\n' > $(4)/etc/zedbsd-root
	@set -e; umask 022; \
		cfg_items=""; \
		if [ -f "$$(ROOTFS_CONFIG_STAMP)" ]; then \
			cfg_items="$$$$($$(PYTHON) -c 'with open("$$(ROOTFS_CONFIG_STAMP)") as f: d=dict(p.split("=",1) for p in f.read().strip().split(";") if "=" in p); print(d.get("data","") + " " + d.get("files","") + " " + d.get("platform_files",""))')"; \
		fi; \
		for pass in file mode; do \
			set -- $$$$cfg_items $(3) $$(PACKAGE_FILES); while test $$$$# -gt 0; do \
				option=$$$$1; specification=$$$$2; shift 2; \
				path=$$$${specification%%=*}; value=$$$${specification#*=}; \
				path=$$$${path#/}; \
				test -n "$$$$path" || continue; \
				destination=$(4)/$$$$path; \
				case $$$$option in \
				--file) test "$$$$pass" = file || continue; \
					mkdir -p "$$$${destination%/*}"; cp -f "$$$$value" "$$$$destination" ;; \
				--subtree) test "$$$$pass" = file || continue; \
					test -d "$$$$value" || { \
						echo "rootfs: --subtree names no directory: $$$$value" >&2; \
						exit 2; }; \
					mkdir -p "$$$$destination"; cp -RP "$$$$value/." "$$$$destination/" ;; \
				--mode) test "$$$$pass" = mode || continue; \
					test -e "$$$$destination" || { \
						echo "rootfs: --mode names a file nothing installed: /$$$$path" >&2; \
						exit 2; }; \
					chmod "$$$$value" "$$$$destination" ;; \
				*) echo "unknown rootfs option: $$$$option" >&2; exit 2 ;; \
				esac; \
			done; \
		done
	@set -e; umask 022; \
		links=""; \
		if [ -f "$$(ROOTFS_CONFIG_STAMP)" ]; then \
			links="$$$$($$(PYTHON) -c 'with open("$$(ROOTFS_CONFIG_STAMP)") as f: d=dict(p.split("=",1) for p in f.read().strip().split(";") if "=" in p); print(d.get("links",""))')"; \
		fi; \
		links="$$$$links $$(PACKAGE_LINKS)"; \
		for specification in $$$$links; do \
			path=$$$${specification%%=*}; target=$$$${specification#*=}; \
			path=$$$${path#/}; \
			test -n "$$$$path" || continue; \
			destination=$(4)/$$$$path; \
			mkdir -p "$$$${destination%/*}"; ln -sfn "$$$$target" "$$$$destination"; \
		done
	@$$(call ROOTFS_INSTALL_DEVELOPMENT,$(4))
	@find $(4) \( -path $(4)/tmp -o -path $(4)/shm \) \
		-prune -o -type d -exec chmod 0755 {} +
	@umask 022; touch $$@
endef

.PHONY: FORCE_ROOTFS_CONFIG
FORCE_ROOTFS_CONFIG:

$(ROOTFS_CONFIG_STAMP): FORCE_ROOTFS_CONFIG
	@mkdir -p $(dir $@)
	@platform='$(PLATFORM)'; \
	architecture='$(ARCHITECTURE)'; \
	board='$(BOARD)'; \
	programs='$(strip $(USERLAND_PROGRAMS))'; \
	value="platform=$$platform;architecture=$$architecture;board=$$board;programs=$$programs"; \
	if ! test -f $@ || ! grep -Fqx -- "$$value" $@; then \
		printf '%s\n' "$$value" > $@.tmp; \
		mv $@.tmp $@; \
	fi

# Account and base system configuration files
ZEDBSD_ACCOUNT_INPUTS := \
	userland/base/etc/passwd \
	userland/base/etc/group \
	userland/base/etc/shadow \
	userland/base/etc/passkey \
	userland/base/etc/services \
	userland/base/etc/hosts \
	userland/base/etc/fstab \
	userland/base/etc/rc.conf

ZEDBSD_ACCOUNT_FILES := \
	--file /etc/passwd=userland/base/etc/passwd \
	--file /etc/group=userland/base/etc/group \
	--file /etc/shadow=userland/base/etc/shadow \
	--file /etc/passkey=userland/base/etc/passkey \
	--file /etc/services=userland/base/etc/services \
	--file /etc/hosts=userland/base/etc/hosts \
	--file /etc/fstab=userland/base/etc/fstab \
	--file /etc/rc.conf=userland/base/etc/rc.conf \
	--mode /etc/passwd=0644 --mode /etc/group=0644 \
	--mode /etc/shadow=0400 --mode /etc/passkey=0600 --mode /etc/services=0644 \
	--mode /etc/hosts=0644 --mode /etc/fstab=0644 --mode /etc/rc.conf=0644

ZEDBSD_DYNAMIC_INPUTS := \
	$(BUILD)/dynamic/libc.so \
	$(BUILD)/dynamic/libm.so

ZEDBSD_DYNAMIC_FILES := \
	--file /lib/libc.so=$(BUILD)/dynamic/libc.so \
	--file /lib/libm.so=$(BUILD)/dynamic/libm.so \
	--mode /lib/libc.so=0755 \
	--mode /lib/libm.so=0755

ROOTFS_BIN_FILES := $(sort $(BUILD)/bin/sh $(wildcard $(BUILD)/bin/*))
ROOTFS_BIN_INPUTS := $(ROOTFS_BIN_FILES)
ROOTFS_BIN_ENTRIES := $(foreach b,$(ROOTFS_BIN_FILES),--file /bin/$(notdir $(b))=$(b) --mode /bin/$(notdir $(b))=0755)

ROOTFS_INPUTS ?= $(ZEDBSD_ACCOUNT_INPUTS) $(ZEDBSD_DYNAMIC_INPUTS) $(ROOTFS_BIN_INPUTS)
ROOTFS_FILES  ?= $(ZEDBSD_ACCOUNT_FILES) $(ZEDBSD_DYNAMIC_FILES) $(ROOTFS_BIN_ENTRIES)

X86_64_ARCH_INPUTS ?= $(ROOTFS_INPUTS)
X86_64_ARCH_FILES  ?= $(ROOTFS_FILES)
AMD64_ARCH_INPUTS  ?= $(X86_64_ARCH_INPUTS)
AMD64_ARCH_FILES   ?= $(X86_64_ARCH_FILES)

$(eval $(call ROOTFS_TREE_RULE,x86_64,$(X86_64_ARCH_INPUTS),$(X86_64_ARCH_FILES)))

.PHONY:rootfs-x86_64
rootfs-x86_64: $(BUILD)/rootfs/.stamp
