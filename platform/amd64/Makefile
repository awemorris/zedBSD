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
	src/hal/x86_64/asm.c \
	src/hal/x86_64/lib.c \
	src/hal/x86_64/page.c \
	src/hal/x86_64/pmem-range.c \
	src/hal/x86_64/ram-map.c \
	src/hal/x86_64/framebuffer-map.c \
	src/hal/x86_64/space.c \
	src/hal/x86_64/image.c \
	src/hal/x86_64/acpi-window.c \
	src/hal/x86_64/cmain.c \
	src/hal/x86_64/descriptor.c \
	src/hal/x86_64/int.c \
	src/hal/x86_64/irq.c \
	src/hal/x86_64/msi-source.c \
	src/hal/x86_64/task.c \
	src/hal/x86_64/percpu.c \
	src/hal/x86_64/smp.c \
	src/hal/x86_64/bsp-pcat/boot.c \
	src/hal/x86_64/bsp-pcat/handoff-validation.c \
	src/hal/x86_64/bsp-pcat/cons.c \
	src/hal/x86_64/bsp-pcat/pic.c \
	src/hal/x86_64/bsp-pcat/clock.c \
	src/hal/x86_64/bsp-pcat/acpi.c \
	src/hal/x86_64/bsp-pcat/lapic.c \
	src/hal/x86_64/bsp-pcat/idle-suspend.c \
	src/hal/x86_64/bsp-pcat/early-init-policy.c \
	src/hal/x86_64/bsp-pcat/timecounter-policy.c \
	src/hal/x86_64/bsp-pcat/timecounter.c \
	src/hal/x86_64/bsp-pcat/mcfg.c \
	src/hal/x86_64/bsp-pcat/ioapic.c

# HAL Assembly Sources
KERN_ASM += \
	src/hal/x86_64/locore.S \
	src/hal/x86_64/trap.S \
	src/hal/x86_64/dispatch.S \
	src/hal/x86_64/ap-trampoline.S

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
include platform/amd64/vmunix.mk


include platform/amd64/bootloader.mk
include platform/amd64/rootfs.mk
include platform/amd64/disk-image.mk
include platform/amd64/run.mk
include platform/amd64/toolchain.mk
