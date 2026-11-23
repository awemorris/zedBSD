# No firmware is needed for pc9801ns.

.PHONY: firmware-platform
firmware-platform:

.PHONY: firmware-licenses-platform
firmware-licenses-platform:
	$(MAKE) -f mk/userland.mk firmware-licenses PLATFORM=$(PLATFORM) BUILD=$(BUILD) ZEDBSD_CONFIGMK=$(ZEDBSD_CONFIGMK) TOPLEVEL_BUILD=$(TOPLEVEL_BUILD)
