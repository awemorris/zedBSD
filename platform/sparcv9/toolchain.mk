.PHONY: toolchain
toolchain:
	@test -x "$(SPARCV9_CC)" || { \
		echo "SPARC V9 compiler not found: $(SPARCV9_CC)" >&2; \
		echo "Set SPARCV9_PREFIX or SPARCV9_CC to an installed cross toolchain." >&2; \
		exit 2; \
	}
	@test -x "$(SPARCV9_LD)"
	@test -x "$(SPARCV9_OBJCOPY)"
	@echo "SPARC V9 toolchain: $(SPARCV9_PREFIX)"
