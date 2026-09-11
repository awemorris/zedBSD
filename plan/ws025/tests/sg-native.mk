# Link-only fragmented DMA probe; force an ordinary relink after this gate.
WS025_SG_KERNEL := $(BUILD)/tests/sg-kernel.o
AMD64_VMUNIX_OBJS += $(WS025_SG_KERNEL)
$(WS025_SG_KERNEL): plan/ws025/tests/sg-kernel.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) $(WS025_SG_FLAGS) -c $< -o $@
.PHONY: ws025-sg-relink
$(BUILD)/vmunix: $(WS025_SG_KERNEL) ws025-sg-relink
$(BUILD)/vmunix: LD += --wrap=drv_dma_vector_create --wrap=hal_pmem_alloc_range --wrap=drv_usb_hcd_register
