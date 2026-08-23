.PHONY: toolchain
toolchain:
	@command -v $(CC) >/dev/null
	@command -v $(LD) >/dev/null
	@command -v $(OBJCOPY) >/dev/null
	@echo "PC/AT toolchain: $(CC), $(LD), $(OBJCOPY)"
