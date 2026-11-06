ROOTFS_DIRECTORIES := \
	bin sbin lib etc var root home dev boot run \
	usr/bin usr/sbin usr/libexec var/empty etc/ssh etc/service.d

ROOTFS_STICKY_DIRECTORIES := \
	tmp shm

ROOTFS_ACCOUNT_INPUTS := \
	userland/base/etc/passwd \
	userland/base/etc/group \
	userland/base/etc/shadow \
	userland/base/etc/passkey \
	userland/base/etc/services \
	userland/base/etc/hosts \
	userland/base/etc/fstab \
	userland/base/etc/rc.conf \
	userland/base/etc/net.conf

ROOTFS_ACCOUNT_FILES := \
	--file /etc/passwd=userland/base/etc/passwd \
	--file /etc/group=userland/base/etc/group \
	--file /etc/shadow=userland/base/etc/shadow \
	--file /etc/passkey=userland/base/etc/passkey \
	--file /etc/services=userland/base/etc/services \
	--file /etc/hosts=userland/base/etc/hosts \
	--file /etc/fstab=userland/base/etc/fstab \
	--file /etc/rc.conf=userland/base/etc/rc.conf \
	--file /etc/net.conf=userland/base/etc/net.conf \
	--mode /etc/passwd=0644 --mode /etc/group=0644 \
	--mode /etc/shadow=0400 --mode /etc/passkey=0600 --mode /etc/services=0644 \
	--mode /etc/hosts=0644 --mode /etc/fstab=0644 --mode /etc/rc.conf=0644 \
	--mode /etc/net.conf=0644

ROOTFS_SERVICE_INPUTS := \
	$(wildcard userland/base/init/services/*) \
	userland/base/init/make-homes \
	userland/base/bootlog/service \
	userland/base/netclock/service \
	userland/base/audiod/audiod.service \
	userland/base/volumed/volumed.service \
	userland/base/bluetoothd/bluetoothd.service \
	userland/desktop/sessiond/greeter.service

ROOTFS_SERVICE_FILES := \
	$(foreach s,$(wildcard userland/base/init/services/*),--file /etc/service.d/$(notdir $(s))=$(s) --mode /etc/service.d/$(notdir $(s))=0644) \
	--file /usr/libexec/make-homes=userland/base/init/make-homes --mode /usr/libexec/make-homes=0755 \
	--file /etc/service.d/bootlog=userland/base/bootlog/service --mode /etc/service.d/bootlog=0644 \
	--file /etc/service.d/netclock=userland/base/netclock/service --mode /etc/service.d/netclock=0644 \
	--file /etc/service.d/audiod=userland/base/audiod/audiod.service --mode /etc/service.d/audiod=0644 \
	--file /etc/service.d/volumed=userland/base/volumed/volumed.service --mode /etc/service.d/volumed=0644 \
	--file /etc/service.d/bluetoothd=userland/base/bluetoothd/bluetoothd.service --mode /etc/service.d/bluetoothd=0644 \
	$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),--file /etc/service.d/greeter=userland/desktop/sessiond/greeter.service --mode /etc/service.d/greeter=0644) \
	$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),--file /etc/keiland/session=userland/desktop/sessiond/session.sh --mode /etc/keiland/session=0755) \
	$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),--file /etc/keiland/autologin=userland/desktop/sessiond/autologin --mode /etc/keiland/autologin=0644)

ROOTFS_DYNAMIC_INPUTS := \
	$(BUILD)/dynamic/libc.so \
	$(BUILD)/dynamic/libm.so \
	$(BUILD)/dynamic/libutil.so

ROOTFS_DYNAMIC_FILES := \
	--file /lib/libc.so=$(BUILD)/dynamic/libc.so \
	--file /lib/libm.so=$(BUILD)/dynamic/libm.so \
	--file /lib/libutil.so=$(BUILD)/dynamic/libutil.so \
	--mode /lib/libc.so=0755 \
	--mode /lib/libm.so=0755 \
	--mode /lib/libutil.so=0755

ROOTFS_PACKAGE_LINKS ?= \
	/usr/lib/libz.so=libz.so.1 \
	/usr/lib/libcurl.so=libcurl.so.4

ROOTFS_CONFIG_STAMP := \
	$(BUILD)/.rootfs-config

ROOTFS_TARGET_SYSROOT ?= \
	$(BUILD)/sysroot

ROOTFS_DEVELOPMENT ?= $(if $(CONFIG_ROOTFS_DEVELOPMENT),$(CONFIG_ROOTFS_DEVELOPMENT),y)
ifeq ($(strip $(ROOTFS_TARGET_SYSROOT)),)
override ROOTFS_DEVELOPMENT := n
endif

ifeq ($(ROOTFS_DEVELOPMENT),y)
ROOTFS_DEVELOPMENT_INPUTS := \
	$(ROOTFS_TARGET_SYSROOT)/.zedbsd-sysroot-complete
define ROOTFS_INSTALL_DEVELOPMENT
	umask 022; mkdir -p $(1)/usr/include $(1)/usr/lib
	umask 022; cp -R $(ROOTFS_TARGET_SYSROOT)/usr/include/. $(1)/usr/include/
	umask 022; cp -R $(ROOTFS_TARGET_SYSROOT)/usr/lib/. $(1)/usr/lib/
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
		links="$$$$links $$(ROOTFS_PACKAGE_LINKS)"; \
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


ROOTFS_BIN_FILES := $(sort $(BUILD)/bin/sh $(wildcard $(BUILD)/bin/*))
ROOTFS_BIN_INPUTS := $(ROOTFS_BIN_FILES)
ROOTFS_BIN_ENTRIES := $(foreach b,$(ROOTFS_BIN_FILES),--file /bin/$(notdir $(b))=$(b) --mode /bin/$(notdir $(b))=0755)

ROOTFS_SBIN_FILES := $(wildcard $(BUILD)/sbin/*)
ROOTFS_SBIN_INPUTS := $(ROOTFS_SBIN_FILES)
ROOTFS_SBIN_ENTRIES := $(foreach b,$(ROOTFS_SBIN_FILES),--file /sbin/$(notdir $(b))=$(b) --mode /sbin/$(notdir $(b))=0755)

ROOTFS_LIBEXEC_FILES := $(wildcard $(BUILD)/usr/libexec/*)
ROOTFS_LIBEXEC_INPUTS := $(ROOTFS_LIBEXEC_FILES)
ROOTFS_LIBEXEC_ENTRIES := $(foreach b,$(ROOTFS_LIBEXEC_FILES),--file /usr/libexec/$(notdir $(b))=$(b) --mode /usr/libexec/$(notdir $(b))=0755)

ROOTFS_INPUTS ?= $(ROOTFS_ACCOUNT_INPUTS) $(ROOTFS_SERVICE_INPUTS) $(ROOTFS_DYNAMIC_INPUTS) $(ROOTFS_BIN_INPUTS) $(ROOTFS_SBIN_INPUTS) $(ROOTFS_LIBEXEC_INPUTS)
ROOTFS_FILES  ?= $(ROOTFS_ACCOUNT_FILES) $(ROOTFS_SERVICE_FILES) $(ROOTFS_DYNAMIC_FILES) $(ROOTFS_BIN_ENTRIES) $(ROOTFS_SBIN_ENTRIES) $(ROOTFS_LIBEXEC_ENTRIES)
