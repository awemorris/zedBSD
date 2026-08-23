.PHONY: toolchain
toolchain:
	@command -v $(M68K_CC) >/dev/null
	@command -v $(M68K_LD) >/dev/null
	@command -v $(M68K_OBJCOPY) >/dev/null
	@echo "X68000 toolchain: $(M68K_CC), $(M68K_LD), $(M68K_OBJCOPY)"
