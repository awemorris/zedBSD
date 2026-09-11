WS025_PROFILE_OBJECT := $(BUILD)/tests/input-profile-kernel.o
AMD64_VMUNIX_OBJS += $(WS025_PROFILE_OBJECT)
$(WS025_PROFILE_OBJECT): plan/ws025/tests/input-profile-kernel.c $(ZEDBSD_SYSROOT_AMD64)/.zedbsd-sysroot-complete
	@mkdir -p $(dir $@)
	$(CC) $(AMD64_CPPFLAGS) $(AMD64_CFLAGS) -c $< -o $@
.PHONY: ws025-profile-relink
$(BUILD)/vmunix: $(WS025_PROFILE_OBJECT) ws025-profile-relink
$(BUILD)/vmunix: LD += --wrap=vmspace_user_input_lease_acquire --wrap=vmspace_user_lease_release --wrap=vm_kernel_map_borrow --wrap=vm_kernel_map_release --wrap=hal_space_map --wrap=hal_space_prot --wrap=hal_space_unmap
