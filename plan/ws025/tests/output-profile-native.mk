WS025_OUTPUT_PROFILE_OBJECT := $(BUILD)/tests/output-profile-kernel.o
AMD64_VMUNIX_OBJS += $(WS025_OUTPUT_PROFILE_OBJECT)
$(WS025_OUTPUT_PROFILE_OBJECT): plan/ws025/tests/output-profile-kernel.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-output-profile-relink
$(BUILD)/vmunix: $(WS025_OUTPUT_PROFILE_OBJECT) ws025-output-profile-relink
$(BUILD)/vmunix: LD += --wrap=vmspace_user_lease_acquire --wrap=vmspace_user_lease_release --wrap=hal_space_map --wrap=hal_space_unmap --wrap=vmspace_pin_user_pages
