#
# Additional kernel test features and drivers for the kernel build.
#

# [PC/AT serial mirror of the console]
ifneq ($(filter y,$(CONFIG_PCAT_SERIAL_MIRROR)),)
KERN_CPPFLAGS += -DPCAT_SERIAL_MIRROR
endif

# [RPi4 / RPi5 serial console] (the PL011 / the debug UART carries the console)
ifneq ($(filter rpi4y rpi5y,$(PLATFORM)$(CONFIG_RPI4_SERIAL_CONSOLE) $(PLATFORM)$(CONFIG_RPI5_SERIAL_CONSOLE)),)
KERN_CPPFLAGS += -DARM64_SERIAL_CONSOLE
endif

# [Kernel Test] checkpoints
ifneq ($(filter y,$(CONFIG_KERNEL_TEST_CHECKPOINTS)),)
KERN_CPPFLAGS += -DKERN_TEST_CHECKPOINTS
endif

# [Kernel Test] input injection
ifneq ($(filter y,$(CONFIG_INPUT_TEST_INJECT)),)
KERN_CPPFLAGS += -DINPUT_TEST_INJECT
KERN_SOURCES += \
	src/drivers/generic/input-inject.c
endif

# [Kernel Test] security key loopback
ifneq ($(filter y,$(CONFIG_SECURITY_KEY_TEST_LOOPBACK)),)
KERN_CPPFLAGS += -DSECURITY_KEY_TEST_LOOPBACK
endif

# [Kernel Test] Bluetooth loopback
ifneq ($(filter y,$(CONFIG_BT_TEST_LOOPBACK)),)
KERN_CPPFLAGS += -DBT_TEST_LOOPBACK
endif

# [Kernel Test] USB HID checkpoint
ifneq ($(filter y,$(CONFIG_KERNEL_USB_HID_CHECKPOINT)),)
KERN_CPPFLAGS += -DCONFIG_KERNEL_USB_HID_CHECKPOINT=1
endif

# [Kernel Test] desktop thread balancer stress (needs the balancer)
ifeq ($(CONFIG_KERN_DESKTOP_BALANCER)$(CONFIG_KERN_DESKTOP_BALANCER_TEST),yy)
KERN_CPPFLAGS += -DCONFIG_KERN_DESKTOP_BALANCER_TEST=1
KERN_SOURCES += \
	src/kern/sched-hint-test.c
endif
