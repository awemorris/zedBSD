.PHONY: toolchain
toolchain:
	@command -v $(ARM64_CC) >/dev/null
	@command -v $(ARM64_LD) >/dev/null
	@command -v $(ARM64_OBJCOPY) >/dev/null
	@echo "ARM64 toolchain: $(ARM64_CC), $(ARM64_LD), $(ARM64_OBJCOPY)"
