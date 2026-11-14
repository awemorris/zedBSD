#
# Add kernel test configuration options here. These options are not
# part of the kernel configuration, but are used to enable/disable
# kernel test features in the kernel source code.
#
# See also test-kernel.mk for how these options are used in the kernel
# build.
#

# [Kernel Test: Checkpoints]
CONFIG_KERNEL_TEST_CHECKPOINTS    ?= n

# [Kernel Test: Input Injection]
CONFIG_INPUT_TEST_INJECT          ?= n

# [Kernel Test: Security Key Loopback]
CONFIG_SECURITY_KEY_TEST_LOOPBACK ?= n

# [Kernel Test:: Bluetooth Loopback]
CONFIG_BT_TEST_LOOPBACK           ?= n

# [Kernel Test: USB HID Checkpoint]
CONFIG_KERNEL_USB_HID_CHECKPOINT  ?= n

# [Kernel Test: Thermal Injection] (/dev/thermal-inject)
CONFIG_THERMAL_TEST_INJECT        ?= n
ifneq ($(filter y,$(CONFIG_THERMAL_TEST_INJECT)),)
CONFIG_CPPFLAGS += -DCONFIG_THERMAL_TEST_INJECT=1
endif

# [Kernel Test: Desktop Thread Balancer Stress] (desktop_balancer.classes=, desktop_balancer.stress=1)
#  - Depends: CONFIG_KERN_DESKTOP_BALANCER
CONFIG_KERN_DESKTOP_BALANCER_TEST ?= n
