.PHONY: toolchain
toolchain:
	@command -v $(firstword $(CC)) >/dev/null
	@command -v $(LD) >/dev/null
	@command -v $(OBJCOPY) >/dev/null
	@echo "PC-98 toolchain: $(CC), $(LD), $(OBJCOPY)"
