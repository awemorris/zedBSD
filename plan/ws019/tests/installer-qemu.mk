# Read-only command/Noct integration before the installer transaction is enabled.
include plan/ws019/tests/formatter-qemu.mk
WS019_INSTALL_UFS := $(ARCH_IMAGE_DIR)/amd64-ws019-installer.ufs
WS019_INSTALL_SCRIPTS := userland/base/zedinst/selection.noct plan/ws019/tests/installer-selection.noct plan/ws019/tests/installer-scratch.noct \
 userland/base/zedinst/commands.noct userland/base/zedinst/identity.noct \
 plan/ws019/tests/installer-identity.noct plan/ws019/tests/installer-command.noct
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS019_INSTALL_UFS),amd64,\
 $(AMD64_ARCH_INPUTS) $(WS019_GUEST) $(WS019_FORMAT_PROBE) $(WS019_INSTALL_SCRIPTS) $(ZEDBSD_CONFIG),\
 $(AMD64_ARCH_FILES) --file /usr/bin/storage-exit=$(WS019_GUEST) \
 --file /usr/bin/formatter-probe=$(WS019_FORMAT_PROBE) \
 --file /usr/lib/zedinst/commands.noct=userland/base/zedinst/commands.noct \
 --file /usr/lib/zedinst/selection.noct=userland/base/zedinst/selection.noct \
 --file /usr/lib/zedinst/installer-selection.noct=plan/ws019/tests/installer-selection.noct \
 --file /usr/lib/zedinst/installer-scratch.noct=plan/ws019/tests/installer-scratch.noct \
 --file /usr/lib/zedinst/identity.noct=userland/base/zedinst/identity.noct \
 --file /usr/lib/zedinst/installer-identity.noct=plan/ws019/tests/installer-identity.noct \
 --file /usr/lib/zedinst/installer-command.noct=plan/ws019/tests/installer-command.noct))
.PHONY: ws019-installer-qemu-fixture
ws019-installer-qemu-fixture: $(WS019_INSTALL_UFS)
