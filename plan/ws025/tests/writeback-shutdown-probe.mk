WS025_SHUTDOWN_PROBE := $(BUILD)/tests/writeback-shutdown-probe.o
AMD64_VMUNIX_OBJS += $(WS025_SHUTDOWN_PROBE)
$(WS025_SHUTDOWN_PROBE): plan/ws025/tests/writeback-shutdown-probe.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-shutdown-probe-relink
$(BUILD)/vmunix: $(WS025_SHUTDOWN_PROBE) ws025-shutdown-probe-relink
$(BUILD)/vmunix: LD += --wrap=drv_usb_shutdown --wrap=kern_platform_halt
