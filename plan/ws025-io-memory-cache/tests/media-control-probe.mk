WS025_MEDIA_PROBE := $(BUILD)/tests/media-control-probe.o
AMD64_VMUNIX_OBJS += $(WS025_MEDIA_PROBE)
$(WS025_MEDIA_PROBE): plan/ws025-io-memory-cache/tests/media-control-probe.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-media-probe-relink
$(BUILD)/vmunix: $(WS025_MEDIA_PROBE) ws025-media-probe-relink
$(BUILD)/vmunix: LD += --wrap=drv_usb_urb_wait_reusable --wrap=disk_media_retire
