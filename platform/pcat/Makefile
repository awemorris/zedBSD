# Defaults
override CONFIG_DRIVER_PCI                 := y
override CONFIG_DRIVER_USB                 := y
override CONFIG_DRIVER_PCI_NVME            := y
override CONFIG_DRIVER_PCI_AHCI            := y
override CONFIG_DRIVER_PCI_VMD             := y
override CONFIG_DRIVER_LOOP                := y
override CONFIG_DRIVER_FS_UFS              := y
override CONFIG_DRIVER_FS_FAT              := y
override CONFIG_DRIVER_FS_OVERLAYFS        := y
override CONFIG_KERN_NET                   := y
override CONFIG_KERN_NET_IPV4              := y
override CONFIG_KERN_NET_IPV6              := y
override CONFIG_KERN_NET_ETHERNET          := y
override CONFIG_KERN_NET_WIFI              := y
override CONFIG_DRIVER_USB_CDC_NCM         := y
override CONFIG_DRIVER_USB_CDC_ECM         := y
override CONFIG_DRIVER_PCI_RTL8125         := y
override CONFIG_DRIVER_USB_RTL815X         := y
override CONFIG_DRIVER_PCI_INTEL_AX211     := y
override CONFIG_DRIVER_USB_RTL8822BU       := y
override CONFIG_DRIVER_GRAPHICS            := y
override CONFIG_DRIVER_GRAPHICS_GENERIC    := y
override CONFIG_DRIVER_GPU                 := y
override CONFIG_DRIVER_PCI_I915            := y
override CONFIG_DRIVER_PCI_NVRTX           := y
override CONFIG_DRIVER_PCI_AMDRDNA	   := y
override CONFIG_DRIVER_PCI_VENUS	   := y
override CONFIG_DRIVER_AUDIO               := y
override CONFIG_DRIVER_PCI_HDA             := y
override CONFIG_DRIVER_HID                 := y
override CONFIG_DRIVER_I2C_HID             := y
override CONFIG_DRIVER_USB                 := y
override CONFIG_DRIVER_PCI_OHCI            := y
override CONFIG_DRIVER_PCI_UHCI            := y
override CONFIG_DRIVER_PCI_EHCI            := y
override CONFIG_DRIVER_PCI_XHCI            := y
override CONFIG_DRIVER_TYPEC               := y
override CONFIG_DRIVER_USB_HUB             := y
override CONFIG_DRIVER_USB_HID             := y
override CONFIG_DRIVER_USB_STORAGE         := y
override CONFIG_DRIVER_USB_UASP            := y
override CONFIG_DRIVER_USB_CCID            := y
override CONFIG_DRIVER_USB_BT              := y
override CONFIG_DRIVER_USB_UVC             := y
override CONFIG_DRIVER_BT                  := y
override CONFIG_DRIVER_INPUT_BRIDGE        := y
override CONFIG_DRIVER_CPU_PERF            := y
override CONFIG_DRIVER_DELL_SMM_BOOST      := y
override CONFIG_DRIVER_CAMERA              := y
override CONFIG_DRIVER_ACPI                := y
override CONFIG_DRIVER_I2C                 := y
override CONFIG_DRIVER_GPIO                := y
override CONFIG_DRIVER_PCI_LPSS_I2C        := y
override CONFIG_DRIVER_PCI_LPSS_GPIO       := y
override CONFIG_DRIVER_PCI_AMD_GPIO        := y

# HAL C Sources
KERN_SOURCES += \
	src/hal/x86/rtc.c \
	src/hal/x86/boot-parameters.c \
	src/hal/x86/io.c \
	src/hal/i386/smp.c \
	src/hal/i386/percpu.c \
	src/hal/i386/lib.c \
	src/hal/i386/atomic.c \
	src/hal/i386/irq.c \
	src/hal/i386/mps.c \
	src/hal/i386/acpi.c \
	src/hal/i386/lapic.c \
	src/hal/i386/ioapic.c \
	src/hal/i386/page.c \
	src/hal/i386/space.c \
	src/hal/i386/int.c \
	src/hal/i386/cmain.c \
	src/hal/i386/task.c \
	src/hal/i386/bsp-pcat/boot.c \
	src/hal/i386/bsp-pcat/cons.c \
	src/hal/i386/bsp-pcat/pic.c \
	src/hal/i386/bsp-pcat/pit.c \
	src/hal/idle-suspend-unsupported.c

# HAL Assembly Sources
KERN_ASM := \
	src/hal/i386/locore.S \
	src/hal/i386/ap-trampoline.S \
	src/hal/i386/trap.S \
	src/hal/i386/dispatch.S

# Kernel Sources
KERN_SOURCES += \
	src/kern/platform/pcat.c \
	src/drivers/platform/pcat/uefi-variables.c \
	src/drivers/platform/pcat/pcat-ide.c \
	src/drivers/platform/pcat/cpu-x86/cpux86.c \
	src/drivers/platform/pcat/cpu-x86/cpux86-cpuid.c \
	src/drivers/platform/pcat/cpu-x86/cpu-x86.c \
	src/drivers/platform/pcat/cpu-x86/cpu-x86-insn.c
	src/drivers/disklabel/mbr.c \
	src/drivers/disklabel/gpt.c \
	src/drivers/disklabel/pcat.c \
	src/drivers/pci/pci-pcat.c \
	src/drivers/pci/pci-pcat-assign.c \
	src/drivers/pci/pci-power.c \
	src/drivers/pci/pci-window.c \
	src/drivers/isa/ps2-8042.c

# vmunix target
include platform/pcat/vmunix.mk

include platform/pcat/bootloader.mk
include platform/pcat/rootfs.mk
include platform/pcat/disk-image.mk
include platform/pcat/run.mk
include platform/pcat/toolchain.mk
