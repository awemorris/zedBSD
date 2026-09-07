# Link-only vmap probe, restored to ordinary amd64 after acceptance.
WS025_VMAP_KERNEL := $(BUILD)/tests/vmap-kernel.o
AMD64_VMUNIX_OBJS += $(WS025_VMAP_KERNEL)
$(WS025_VMAP_KERNEL): plan/ws025-io-memory-cache/tests/vmap-kernel.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-vmap-relink
$(BUILD)/vmunix: $(WS025_VMAP_KERNEL) ws025-vmap-relink
$(BUILD)/vmunix: LD += --wrap=io_pool_init --wrap=kernel_cpu_notify_handler --wrap=hal_pmem_alloc_range --wrap=hal_pmem_alloc --wrap=io_scratch_alloc --wrap=hal_malloc
