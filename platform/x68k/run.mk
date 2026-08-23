X68K_EMULATOR ?= mame
X68K_MACHINE ?= x68000

.PHONY: run
run: disk-image
	@command -v $(X68K_EMULATOR) >/dev/null || { \
		echo "X68000 is not supported by QEMU; install MAME or override X68K_EMULATOR." >&2; \
		exit 2; \
	}
	$(X68K_EMULATOR) $(X68K_MACHINE) -hard1 "$(abspath $(DISK_IMAGE_ARTIFACT))"
