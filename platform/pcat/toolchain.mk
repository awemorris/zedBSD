.PHONY: toolchain
toolchain:
	@command -v $(firstword $(CC)) >/dev/null
	@command -v $(LD) >/dev/null
	@command -v $(OBJCOPY) >/dev/null
	@echo "PC/AT toolchain: $(CC), $(LD), $(OBJCOPY)"
