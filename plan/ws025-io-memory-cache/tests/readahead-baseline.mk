# Diagnostic fixture only; never included by the production build.
WS025_READ_BASELINE := $(BUILD)/tests/readahead-baseline.o
AMD64_VMUNIX_OBJS += $(WS025_READ_BASELINE)
$(WS025_READ_BASELINE): plan/ws025-io-memory-cache/tests/readahead-baseline.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-read-baseline-relink
$(BUILD)/vmunix: $(WS025_READ_BASELINE) ws025-read-baseline-relink
$(BUILD)/vmunix: LD += --wrap=readahead_submit
