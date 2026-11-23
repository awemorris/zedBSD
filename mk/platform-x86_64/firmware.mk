# No firmware is needed for x86_64, so we just define the variables to empty values

firmware-platform:

firmware-licenses-platform:
	$(MAKE) -f mk/userland.mk firmware-licenses PLATFORM=$(PLATFORM) BUILD=$(BUILD) ZEDBSD_CONFIGMK=$(ZEDBSD_CONFIGMK) TOPLEVEL_BUILD=$(TOPLEVEL_BUILD)
