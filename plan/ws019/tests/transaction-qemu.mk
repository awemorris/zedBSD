# Private Noct managed-file transaction image; not a public installer launcher.
include plan/ws019/tests/formatter-qemu.mk
WS019_TRANSACTION_UFS := $(ARCH_IMAGE_DIR)/amd64-ws019-transaction.ufs
WS019_NESTED_MOUNT := $(BUILD)/tests/nested-mount
WS019_NESTED_OBJECT := $(BUILD)/user64/plan/ws019/tests/nested-mount-guest.o
$(WS019_NESTED_MOUNT): $(AMD64_USER_LIBC_OBJS) $(WS019_NESTED_OBJECT) $(AMD64_PLATFORM)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 --gc-sections -nostdlib -static -z max-page-size=4096 -z stack-size=0x100000 \
	 -T $(AMD64_PLATFORM)/user.ld $(AMD64_USER_LIBC_OBJS) $(WS019_NESTED_OBJECT) -o $@
	$(NOCT) --path=tools/build $(AMD64_USER_ELF_CHECK) --machine amd64 $@
WS019_TRANSACTION_SCRIPTS := userland/base/zedinst/commands.noct \
 userland/base/zedinst/install.noct userland/base/zedinst/main.noct \
 userland/base/zedinst/destination.noct plan/ws019/tests/installer-destination.noct \
 userland/base/zedinst/preflight.noct plan/ws019/tests/installer-preflight.noct \
 userland/base/zedinst/workspace.noct plan/ws019/tests/installer-workspace.noct \
 userland/base/zedinst/confirmation.noct plan/ws019/tests/installer-confirmation.noct \
 userland/base/zedinst/admission.noct \
 userland/base/zedinst/metadata.noct plan/ws019/tests/installer-metadata.noct \
 userland/base/zedinst/discovery.noct plan/ws019/tests/installer-discovery.noct \
 userland/base/zedinst/source.noct userland/base/zedinst/selection.noct \
 plan/ws019/tests/installer-source.noct \
 plan/ws019/tests/noct-large-seek.noct \
 userland/base/zedinst/identity.noct userland/base/zedinst/transaction.noct \
 userland/base/zedinst/files.noct plan/ws019/tests/installer-files.noct \
 plan/ws019/tests/installer-admission.noct \
 plan/ws019/tests/installer-transaction.noct
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS019_TRANSACTION_UFS),amd64,\
 $(AMD64_ARCH_INPUTS) $(WS019_GUEST) $(WS019_FORMAT_PROBE) $(WS019_NESTED_MOUNT) $(WS019_TRANSACTION_SCRIPTS) $(ZEDBSD_CONFIG),\
 $(AMD64_ARCH_FILES) --file /usr/bin/storage-exit=$(WS019_GUEST) \
 --file /usr/bin/formatter-probe=$(WS019_FORMAT_PROBE) \
 --file /usr/bin/nested-mount=$(WS019_NESTED_MOUNT) \
 --file /usr/lib/zedinst/commands.noct=userland/base/zedinst/commands.noct \
 --file /usr/lib/zedinst/install.noct=userland/base/zedinst/install.noct \
 --file /usr/lib/zedinst/main.noct=userland/base/zedinst/main.noct \
 --file /usr/lib/zedinst/destination.noct=userland/base/zedinst/destination.noct \
 --file /usr/lib/zedinst/installer-destination.noct=plan/ws019/tests/installer-destination.noct \
 --file /usr/lib/zedinst/preflight.noct=userland/base/zedinst/preflight.noct \
 --file /usr/lib/zedinst/installer-preflight.noct=plan/ws019/tests/installer-preflight.noct \
 --file /usr/lib/zedinst/workspace.noct=userland/base/zedinst/workspace.noct \
 --file /usr/lib/zedinst/installer-workspace.noct=plan/ws019/tests/installer-workspace.noct \
 --file /usr/lib/zedinst/confirmation.noct=userland/base/zedinst/confirmation.noct \
 --file /usr/lib/zedinst/installer-confirmation.noct=plan/ws019/tests/installer-confirmation.noct \
 --file /usr/lib/zedinst/admission.noct=userland/base/zedinst/admission.noct \
 --file /usr/lib/zedinst/metadata.noct=userland/base/zedinst/metadata.noct \
 --file /usr/lib/zedinst/discovery.noct=userland/base/zedinst/discovery.noct \
 --file /usr/lib/zedinst/installer-discovery.noct=plan/ws019/tests/installer-discovery.noct \
 --file /usr/lib/zedinst/installer-metadata.noct=plan/ws019/tests/installer-metadata.noct \
 --file /usr/lib/zedinst/source.noct=userland/base/zedinst/source.noct \
 --file /usr/lib/zedinst/selection.noct=userland/base/zedinst/selection.noct \
 --file /usr/lib/zedinst/installer-source.noct=plan/ws019/tests/installer-source.noct \
 --file /usr/lib/zedinst/noct-large-seek.noct=plan/ws019/tests/noct-large-seek.noct \
 --file /usr/lib/zedinst/identity.noct=userland/base/zedinst/identity.noct \
 --file /usr/lib/zedinst/transaction.noct=userland/base/zedinst/transaction.noct \
 --file /usr/lib/zedinst/files.noct=userland/base/zedinst/files.noct \
 --file /usr/lib/zedinst/installer-admission.noct=plan/ws019/tests/installer-admission.noct \
 --file /usr/lib/zedinst/installer-files.noct=plan/ws019/tests/installer-files.noct \
 --file /usr/lib/zedinst/installer-transaction.noct=plan/ws019/tests/installer-transaction.noct))
.PHONY: ws019-transaction-qemu-fixture
ws019-transaction-qemu-fixture: $(WS019_TRANSACTION_UFS)
