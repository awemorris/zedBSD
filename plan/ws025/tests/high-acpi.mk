# Synthetic relocation of an actual firmware RSDP; retained high allocation owns it.
WS025_ACPI_OBJECT := $(BUILD)/tests/high-acpi-kernel.o
AMD64_VMUNIX_OBJS += $(WS025_ACPI_OBJECT)
$(WS025_ACPI_OBJECT): plan/ws025/tests/high-acpi-kernel.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-acpi-relink
$(BUILD)/vmunix: $(WS025_ACPI_OBJECT) ws025-acpi-relink
$(BUILD)/vmunix: LD += --wrap=bsp_acpi_rsdp --wrap=bsp_physical_range_mappable
