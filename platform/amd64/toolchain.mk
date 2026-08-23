.PHONY: toolchain
toolchain:
	@command -v $(CC) >/dev/null
	@command -v $(LD) >/dev/null
	@command -v $(OBJCOPY) >/dev/null
	@command -v $(EFI_CC) >/dev/null
	@command -v $(EFI_LD) >/dev/null
	@echo "AMD64 toolchain: $(CC), $(LD), $(EFI_CC), $(EFI_LD)"
