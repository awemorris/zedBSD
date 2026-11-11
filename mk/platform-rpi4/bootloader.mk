RPI4_BOOT_COMMAND_LINE := login=$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),graphical,console)$(if $(or $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),$(filter n,$(CONFIG_BOOT_KERNEL_MESSAGES))), kmsg=quiet)

# The boot files follow the configuration: they are written every time and
# replaced only when their contents change.
.PHONY: FORCE_BOOT_FILES
FORCE_BOOT_FILES:

$(BUILD)/cmdline.txt: FORCE_BOOT_FILES
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RPI4_BOOT_COMMAND_LINE)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

$(BUILD)/bcm2711-rpi-4-b.dtb: $(RPI4_BOOT_FIRMWARE_DTB)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/config.txt: mk/platform-rpi4/config.txt mk/platform-rpi4/tools/make-rpi4-config.py FORCE_BOOT_FILES
	@mkdir -p $(dir $@)
	$(PYTHON) mk/platform-rpi4/tools/make-rpi4-config.py --source mk/platform-rpi4/config.txt \
		--bluetooth $(if $(filter y,$(CONFIG_DRIVER_RPI4_BT)),y,n) \
		--serial $(if $(filter y,$(CONFIG_RPI4_SERIAL_CONSOLE)),y,n) \
		--output $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

RPI4_FIRMWARE_FILES := \
	$(RPI4_BOOT_FIRMWARE_CACHE_FILES) \
	$(BUILD)/bcm2711-rpi-4-b.dtb \
	$(BUILD)/config.txt \
	$(BUILD)/cmdline.txt

.PHONY: bootloader-platform
bootloader-platform: $(RPI4_FIRMWARE_FILES)
