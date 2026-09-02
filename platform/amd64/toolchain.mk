.PHONY: toolchain
toolchain:
	@command -v $(firstword $(CC)) >/dev/null
	@command -v $(LD) >/dev/null
	@command -v $(OBJCOPY) >/dev/null
	@command -v $(firstword $(EFI_CC)) >/dev/null
	@command -v $(EFI_LD) >/dev/null
	@echo "AMD64 toolchain: $(CC), $(LD), $(EFI_CC), $(EFI_LD)"
