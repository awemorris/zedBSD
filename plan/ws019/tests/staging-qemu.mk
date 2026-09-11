# Existing command observer and formatter probe, plus Noct acceptance scripts.
include plan/ws019/tests/formatter-qemu.mk
WS019_STAGING_UFS := $(ARCH_IMAGE_DIR)/amd64-ws019-staging.ufs
WS019_STAGING_SCRIPTS := userland/base/zedinst/commands.noct \
 plan/ws019/tests/staging-command.noct \
 plan/ws019/tests/staging-files.noct
$(eval $(call ZEDBSD_ARCH_UFS_IMAGE_RULE,$(WS019_STAGING_UFS),amd64,\
 $(AMD64_ARCH_INPUTS) $(WS019_GUEST) $(WS019_FORMAT_PROBE) $(WS019_STAGING_SCRIPTS) $(ZEDBSD_CONFIG),\
 $(AMD64_ARCH_FILES) --file /usr/bin/storage-exit=$(WS019_GUEST) \
 --file /usr/bin/formatter-probe=$(WS019_FORMAT_PROBE) \
 --file /usr/lib/zedinst/commands.noct=userland/base/zedinst/commands.noct \
 --file /usr/lib/zedinst/staging-command.noct=plan/ws019/tests/staging-command.noct \
 --file /usr/lib/zedinst/staging-files.noct=plan/ws019/tests/staging-files.noct))
.PHONY: ws019-staging-qemu-fixture
ws019-staging-qemu-fixture: $(WS019_STAGING_UFS)
