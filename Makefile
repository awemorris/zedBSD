#
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Project Toplevel Makefile
#

# C/C++ compilers to build clang and noct
HOSTCC       := cc
HOSTCXX      := c++

# Python3
PYTHON       := python3

# Configuration File
CONFIGMK     := config.mk

# Base Configuration
PLATFORM     := x86_64
ARCHITECTURE := x86_64
BOARD        := pcat
VARIANT      := native

# Version String
VERSION      := 1.0.0

# LTO setting. (full, thin, none)
KERNEL_LTO   := full

# "zedimage-host" is a host-side tool to create UFS and disk images.
IMAGE_HOST   := build/zedimage-host

# Indicates that the build is done by the toplevel Makefile
TOPLEVEL_BUILD := 1

# The default goal for the toplevel Makefile is "disk-image"
all: vmunix

########################################################################
# Cross Toolchain
########################################################################

include toolchain/llvm/llvm.mk

#######################################################################
# List of Configurations
#######################################################################

################################################
# Boot
################################################

# /usr/include
CONFIG_ROOTFS_DEVELOPMENT         ?= n

# Show boot splash
CONFIG_BOOT_SPLASH                ?= n

# Start sessiond automatically
CONFIG_GRAPHICAL_LOGIN            ?= n

################################################
# 0. Bus
################################################

########################
# Framework
########################

# [PCI Core]
CONFIG_DRIVER_PCI                 ?= n

# [USB Core]
CONFIG_DRIVER_USB                 ?= n

################################################
# 1. Storage
################################################

########################
# Drivers
########################

# [NVMe driver]
CONFIG_DRIVER_PCI_NVME            ?= n

# [SATA AHCI driver]
CONFIG_DRIVER_PCI_AHCI            ?= n

# [Intel VMD driver]
CONFIG_DRIVER_PCI_VMD             ?= n

# [Loopback Disk driver]
CONFIG_DRIVER_LOOP                ?= n

# [RAM Disk driver]
CONFIG_DRIVER_RAMDISK             ?= n

########################
# File Systems
########################

# [UFS]
CONFIG_DRIVER_FS_UFS              ?= n

# [FAT]
CONFIG_DRIVER_FS_FAT              ?= n

# [OverlayFS] 
# - OverlayFS mounts a read-onlye rootfs and a writable data images that exists on a boot FAT partition.
CONFIG_DRIVER_FS_OVERLAYFS        ?= n

########################
# Options
########################

# [Kernel Buffer Cache Size]
CONFIG_BUF_CACHE_KIB              ?= 0

################################################
# 2. Network
################################################

########################
# Netowrk Stack
########################

# [Network Core]
CONFIG_KERN_NET                   ?= n

# [IPv4 Stack]
CONFIG_KERN_NET_IPV4              ?= n

# [IPv6 Stack]
#  - Depends on CONFIG_KERN_NET_IPV4
CONFIG_KERN_NET_IPV6              ?= n

# [Ethernet Stack]
CONFIG_KERN_NET_ETHERNET          ?= n

# [WiFi Stack]
CONFIG_KERN_NET_WIFI              ?= n

########################
# Ethernet Drivers
########################

# [USB CDC driver]
CONFIG_DRIVER_USB_CDC             ?= n

# [USB CDC NCM Ethernet driver]
CONFIG_DRIVER_USB_CDC_NCM         ?= n

# [USB CDC NCM Ethernet driver]
CONFIG_DRIVER_USB_CDC_ECM         ?= n

# [Realtek RTL8125 PCIe Ethernet driver]
CONFIG_DRIVER_PCI_RTL8125         ?= n
CONFIG_DRIVER_PCI_RTL8125_PROFILE ?= 0

# [Realtek RTL8156/8157 USB Ethernet vendor mode driver]
CONFIG_DRIVER_USB_RTL815X         ?= n

# [RPi4 Ethernet driver]
CONFIG_DRIVER_RPI4_ETHERNET       ?= n

# [RPi5 Ethernet driver]
CONFIG_DRIVER_RPI5_ETHERNET       ?= n

# [Vision Five 2 Ethernet driver]
CONFIG_DRIVER_RPI5_ETHERNET       ?= n

# [PC/AT NE2000-compatible ISA Ethernet driver]
CONFIG_DRIVER_NE2000              ?= n

# [PC-98 LGY98 C-BUS Ethernet driver]
CONFIG_DRIVER_LGY98               ?= n

########################
# WiFi Drivers
########################

# [Intel AX211 PCIe WiFi driver]
CONFIG_DRIVER_PCI_INTEL_AX211     ?= n

# [Realtek RTL8822BU USB WiFi driver]
CONFIG_DRIVER_USB_RTL8822BU       ?= n

# [RPi4 WiFi driver]
CONFIG_DRIVER_RPI4_WIFI           ?= n

# [RPi5 WiFi driver]
CONFIG_DRIVER_RPI5_WIFI           ?= n

################################################
# 3. Graphics
################################################

########################
# Framework
########################

# [GPU Framework]
CONFIG_DRIVER_GPU                 ?= n

# [Legacy 2D Graphics Framework]
CONFIG_DRIVER_GRAPHICS            ?= n

# [GPU Job Timeouts]
CONFIG_GPU_JOB_RESERVATION_MS     ?= 10000
CONFIG_GPU_JOB_EXECUTION_MS       ?= 60000
CONFIG_GPU_JOB_STOP_MS            ?= 10000
CONFIG_GPU_CONTROL_MS             ?= 10000

########################
# GPU Drivers (/dev/gpuN)
########################

# [Intel i915 GPU Driver]
#  - Skylake to Raptor Lake Refresh
CONFIG_DRIVER_PCI_I915            ?= n

# [NVIDIA RTX GPU Driver]
#  - Turing and later
CONFIG_DRIVER_PCI_NVRTX           ?= n

# [AMD RDNA GPU driver]
#  - RDNA and later
CONFIG_DRIVER_PCI_AMDRDNA         ?= n

# [RPi4 VideoCore VI GPU driver]
CONFIG_DRIVER_RPI4_VIDEOCORE_VI   ?= n

# [RPi5 VideoCore VII GPU driver]
CONFIG_DRIVER_RPI5_VIDEOCORE_VII  ?= n

# [Vision Five 2 Imagination PowerVR GPU and WAVE511 Video Codex driver]
CONFIG_DRIVER_VF2_GPU_VPU         ?= n

# [GHCI GPU driver]
#  - Deliverable of the Amaterrace GPU Project
CONFIG_DRIVER_PCI_GHCI            ?= n

# [Venus GPU Driver]
#  - QEMU Venus Virtual GPU
CONFIG_DRIVER_PCI_VENUS           ?= n

########################
# Legacy 2D Drivers
########################

# [Boot Linear Framebuffer driver]
CONFIG_DRIVER_GRAPHICS_BOOTLFB    ?= n

# [PC/AT VGA driver]
CONFIG_DRIVER_GRAPHICS_PCAT       ?= n

# [PC-98 GDC driver]
CONFIG_DRIVER_GRAPHICS_PC98       ?= n

################################################
# 4. Audio
################################################

########################
# Framework 
########################

# [Audio Core]
CONFIG_DRIVER_AUDIO               ?= n

########################
# Drivers
########################

# [Intel HD Audio driver]
CONFIG_DRIVER_PCI_HDA             ?= n

# [RPi4 PWM Audio driver]
CONFIG_DRIVER_RPI4_AUDIO          ?= n

# [RPi5 RP1 Audio driver]
CONFIG_DRIVER_RPI5_AUDIO          ?= n

# [Vision Five 2 Audio driver]
CONFIG_DRIVER_VF2_AUDIO           ?= n

########################################################################
# 5. Input Device
########################################################################

########################
# Framework
########################

# [Generic HID driver]
CONFIG_DRIVER_HID                 ?= n

########################
# Drivers
########################

# None

################################################
# 6. USB
################################################

########################
# Framework
########################

# [USB Framework]
CONFIG_DRIVER_USB                 ?= n

########################
# Controller Drivers
########################

# [USB 1.1 OHCI driver]
#  - Not implemented yet.
CONFIG_DRIVER_PCI_OHCI            ?= n

# [USB 1.1 UHCI driver]
CONFIG_DRIVER_PCI_UHCI            ?= n

# [USB 2.0 EHCI driver]
CONFIG_DRIVER_PCI_EHCI            ?= n

# [USB 3.0 XHCI driver]
CONFIG_DRIVER_PCI_XHCI            ?= n

# [USB-C USCI driver]
CONFIG_DRIVER_TYPEC               ?= n

########################
# Class Drivers
########################

# [USB Hub driver]
CONFIG_DRIVER_USB_HUB             ?= n

# [USB Human Interface Device (HID) driver]
CONFIG_DRIVER_USB_HID             ?= n

# [USB Mass Storage Device (MSD) driver]
CONFIG_DRIVER_USB_STORAGE         ?= n

# [USB Attached SCSI Protocol (UASP) driver]
CONFIG_DRIVER_USB_UASP            ?= n

# [USB Chip Card Interface Device (CCID) driver]
CONFIG_DRIVER_USB_CCID            ?= n

# [USB Bluetooth driver]
CONFIG_DRIVER_USB_BT              ?= n

# [USB Video Class (UVC) driver]
CONFIG_DRIVER_USB_UVC             ?= n

################################################
# 7. Bluetooth
################################################

########################
# Framework
########################

# [Generic Bluetooth HCI]
CONFIG_DRIVER_BT                  ?= n

# [Bluetooth Input Bridge] (/dev/input/bridge)
#  - Bluetooth daemon sends HID events to /dev/input/bridge.
CONFIG_DRIVER_INPUT_BRIDGE        ?= n

########################
# Drivers
########################

# [RPi4 Bluetooth driver]
#  - CONFIG_RPI4_SERIAL_CONSOLE=y keeps PL011 for the console and has no Bluetooth.
CONFIG_DRIVER_RPI4_BT             ?= n

# [RPi5 Onboard Bluetooth]
CONFIG_DRIVER_RPI5_BT             ?= n

########################################################################
# 8. Thermal Management
########################################################################

########################
# Framework
########################

# [Generic CPU performance monitoring driver] (/dev/thermal)
CONFIG_KERN_CPU_PERF              ?= n

# [x86 Power Control driver]
#  - /dev/thermal implementation
CONFIG_DRIVER_X86_POWER_CONTROL   ?= n

# [RPi5 processor temperature] 
#  - Legacy "sysctl hw.thermal"
#  - TODO: need to port to /dev/thermal
CONFIG_DRIVER_RPI5_FAN            ?= n

################################################
# 9. Camera and Capture
################################################

########################
# Framework
########################

# [Camera Core (/dev/camN)]
CONFIG_DRIVER_CAMERA              ?= n

################################################
# 10. Smart Card
################################################

########################
# Framework
########################

# [Smart Card Core]
CONFIG_DRIVER_SMARTCARD           ?= n

########################################################################
# 11. ACPI
########################################################################

########################
# Framework
########################

# [ACPI Core]
CONFIG_DRIVER_ACPI                ?= n

########################################################################
# 12. I2C/SPI/GPIO
########################################################################

########################
# Framework
########################

# [I2C Generic Driver]
CONFIG_DRIVER_I2C                 ?= n

# [GPIO Generic Driver]
#  - Not implemented yet, only Intel GPIO exists.
CONFIG_DRIVER_GPIO                  ?= n

########################
# Drivers
########################

# [Intel LPSS I2C / AMD I2C driver] (Synopsis DesignWare I2C)
CONFIG_DRIVER_PCI_LPSS_I2C        ?= n

# [Intel LPSS GPIO driver]
CONFIG_DRIVER_PCI_LPSS_GPIO       ?= n

# [AMD GPIO driver]
#  - Not implemented yet.
CONFIG_DRIVER_PCI_AMD_GPIO        ?= n

# [I2C-HID driver]
#  - Depends: 
CONFIG_DRIVER_I2C_HID             ?= n

########################################################################
# 98. Debug Options
########################################################################

# [PC/AT: Console to Serial Mirror]
CONFIG_PCAT_SERIAL_MIRROR         ?= n

# [RPi4: Serial Console] (PL011 on GPIO 14 and 15)
CONFIG_RPI4_SERIAL_CONSOLE        ?= n

# [RPi5: Serial Console] (PL011 on 3-pin debug connector)
CONFIG_RPI5_SERIAL_CONSOLE        ?= n

########################################################################
# 99. Tests
########################################################################

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

########################################################################
# Import the configuration file.
########################################################################

#  - Import CONFIG_* variables.
#  - Import vmunix target.
#  - Import bootloader target.
#  - Import hdd-image target.

include $(CONFIGMK)

########################################################################
# Source Files
########################################################################

########################
# Kernel Base
########################

KERN_SOURCES := \
	src/kern/main.c \
	src/kern/init.c \
	src/kern/boot.c \
	src/kern/fd-object.c \
	src/kern/sysctl.c \
	src/kern/resource.c \
	src/kern/panic.c \
	src/kern/entry.c \
	src/kern/lock.c \
	src/kern/waitq.c \
	src/kern/pipe.c \
	src/kern/cdev.c \
	src/kern/devfs.c \
	src/kern/kcrt.c \
	src/kern/heap.c \
	src/kern/random.c \
	src/kern/random-crypto.c \
	src/kern/system-event.c \
	src/kern/shutdown.c \
	src/kern/pmem.c \
	src/kern/cache.c \
	src/kern/dcache.c \
	src/kern/uncached.c \
	src/kern/irq.c \
	src/kern/clock.c \
	src/kern/timer.c \
	src/kern/sched.c \
	src/kern/thread.c \
	src/kern/sleep.c \
	src/kern/buf.c \
	src/kern/readahead.c \
	src/kern/writeback.c \
	src/kern/poll.c \
	src/kern/usync.c \
	src/kern/disk.c \
	src/kern/device-io.c \
	src/kern/io.c \
	src/kern/inode.c \
	src/kern/process.c \
	src/kern/block-identity.c \
	src/kern/vfs.c \
	src/kern/file.c \
	src/kern/namei.c \
	src/kern/namecache.c \
	src/kern/mount.c \
	src/kern/backing-claim.c \
	src/kern/filedesc.c \
	src/kern/acl.c \
	src/kern/quota.c \
	src/kern/tty.c \
	src/kern/tty-csi.c \
	src/kern/thermal.c \
	src/kern/thermal-guard.c \
	src/kern/text-display.c \
	src/kern/partition.c \
	src/kern/klog.c \
	src/kern/ptrace.c \
	src/kern/freeze.c \
	src/kern/handle.c \
	src/kern/record-lock.c \
	src/kern/cred.c \
	src/kern/signal.c \
	src/kern/cwdinfo.c \
	src/kern/elf.c \
	src/kern/exec.c \
	src/kern/sandbox.c \
	src/kern/user-probe.c \
	src/kern/syscall.c \
	src/kern/uaccess.c \
	src/kern/tmpfs.c \
	src/kern/tmpfs-pages.c \
	src/kern/test-checkpoint.c \
	src/kern/vm/swap.c \
	src/kern/vm/swap-control.c \
	src/kern/vm/swap-header.c \
	src/kern/vm/swap-source.c \
	src/kern/vm/vmspace.c \
	src/kern/vm/vmspace-metadata.c \
	src/kern/vm/vmspace-fault.c \
	src/kern/vm/vmspace-copy.c \
	src/kern/vm/vmspace-fork.c \
	src/kern/vm/vmspace-map.c \
	src/kern/vm/vmspace-region.c \
	src/kern/vm/vmspace-unmap.c \
	src/kern/vm/device.c \
	src/kern/vm/object.c \
	src/kern/vm/object-content.c \
	src/kern/vm/object-fault.c \
	src/kern/vm/object-sync.c \
	src/kern/vm/object-cache.c \
	src/kern/vm/object-prefetch.c \
	src/kern/vm/object-page.c \
	src/kern/vm/commit.c \
	src/kern/vm/metadata-lock.c \
	src/kern/vm/private-page.c \
	src/kern/vm/page-track.c \
	src/kern/vm/reclaim.c \
	src/kern/vm/pageout.c \
	src/kern/platform/driver-startup.c \
	src/drivers/generic/console.c \
	src/drivers/generic/memory-device.c \
	src/drivers/generic/system-device.c \
	src/drivers/generic/dma.c

########################
# libc Base
########################

LIB_SOURCES := \
	userland/base/libc/posix.c \
	userland/base/libc/dlfcn.c \
	userland/base/libc/static-tls.c \
	userland/base/libc/poll.c \
	userland/base/libc/termios.c \
	userland/base/libc/pthread.c \
	userland/base/libc/timer.c \
	userland/base/libc/shm.c \
	userland/base/libc/semaphore.c \
	userland/base/libc/mqueue.c \
	userland/base/libc/socket.c \
	userland/base/libc/resolver.c \
	userland/base/libc/resolver-dns.c \
	userland/base/libc/signal.c \
	userland/base/libc/account.c \
	userland/base/libc/crypt.c \
	userland/base/libc/utmpx.c \
	src/libc/heap.c \
	src/libc/string.c \
	src/libc/ctype.c \
	src/libc/locale.c \
	src/libc/wide.c \
	src/libc/int64.c \
	src/libc/strto.c \
	src/libc/format.c \
	src/libc/stdio.c

CRT0_ASM := src/libc/crt/crt0-$(ZEDBSD_ARCHITECTURE).S

CRT1_ASM := src/libc/crt/crt1-$(ZEDBSD_ARCHITECTURE).S

########################
# Frameworks and Drivers
########################

# [PCI Core]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI=1
KERN_SOURCES += \
	src/drivers/pci/pci.c
endif

# [NVMe driver]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_NVME)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI_NVME=1
KERN_SOURCES += \
	src/drivers/pci/pci-nvme.c
endif

# [SATA AHCI driver]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_AHCI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI_AHCI=1
KERN_SOURCES += \
	src/drivers/pci/pci-ahci.c
endif

# [Intel VMD driver]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_VMD)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI_VMD=1
KERN_SOURCES += \
	src/drivers/pci/vmd.c
endif

# [Loopback Disk driver]
ifneq ($(filter y,$(CONFIG_DRIVER_LOOP)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_LOOP=1
KERN_SOURCES += \
	src/drivers/generic/loop.c
endif

# [RAM Disk driver]
ifneq ($(filter y,$(CONFIG_DRIVER_RAMDISK)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_RAMDISK=1
KERN_SOURCES += \
	src/drivers/generic/ramdisk.c
endif

# [UFS]
ifneq ($(filter y,$(CONFIG_DRIVER_FS_UFS)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_FS_UFS=1
KERN_SOURCES += \
	src/drivers/fs/ufs.c
endif

# [FAT]
ifneq ($(filter y,$(CONFIG_DRIVER_FS_FAT)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_FS_FAT=1
KERN_SOURCES += \
	src/drivers/fs/fat.c
endif

# [OverlayFS]
ifneq ($(filter y,$(CONFIG_DRIVER_FS_OVERLAYFS)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_FS_OVERLAYFS=1
KERN_SOURCES += \
	src/drivers/fs/overlayfs.c
endif

# [Network Stack]
ifneq ($(filter y,$(CONFIG_KERN_NET)),)
KERN_CPPFLAGS += -DCONFIG_KERN_NET=1
KERN_SOURCES += \
	src/kern/net/core.c \
	src/kern/net/socket.c \
	src/kern/net/socket-file.c \
	src/kern/net/packet-buf.c \
	src/kern/net/packet-socket.c \
	src/kern/net/unix-socket.c \
	src/kern/net/net-device.c \
	src/kern/net/checksum.c
endif

# [IPv4 Stack]
ifneq ($(filter y,$(CONFIG_KERN_NET_IPV4)),)
KERN_CPPFLAGS += -DCONFIG_KERN_NET_IPV4=1
KERN_SOURCES += \
	src/kern/net/ipv4.c \
	src/kern/net/icmp.c \
	src/kern/net/inet-socket.c \
	src/kern/net/route.c \
	src/kern/net/udp.c \
	src/kern/net/tcp.c \
	src/kern/net/tcp-cubic.c
endif

# [IPv6 Stack]
ifneq ($(filter y,$(CONFIG_KERN_NET_IPV6)),)
KERN_CPPFLAGS += -DCONFIG_KERN_NET_IPV6=1
KERN_SOURCES += \
	src/kern/net/ipv6.c \
	src/kern/net/in6.c \
	src/kern/net/in6-address.c \
	src/kern/net/in6-route.c \
	src/kern/net/in6-neighbor.c \
	src/kern/net/ipv6-frag.c \
	src/kern/net/ipv6-ioctl.c \
	src/kern/net/icmp6.c \
	src/kern/net/nd6.c \
	src/kern/net/mld6.c \
	src/kern/net/route-socket.c
endif

# [Ethernet Stack]
ifneq ($(filter y,$(CONFIG_KERN_NET_ETHERNET)),)
KERN_CPPFLAGS += -DCONFIG_KERN_NET_ETHERNET=1
KERN_SOURCES += \
	src/kern/net/arp.c \
	src/kern/net/ethernet.c
endif

# [WiFi Stack]
ifneq ($(filter y,$(CONFIG_KERN_NET_WIFI)),)
KERN_CPPFLAGS += -DCONFIG_KERN_NET_WIFI=1
KERN_SOURCES += \
	src/kern/net/wifi/wlan-crypto.c \
	src/kern/net/wifi/wlan-frame.c \
	src/kern/net/wifi/wlan-wpa2-codec.c \
	src/kern/net/wifi/wlan-wpa2.c \
	src/kern/net/wifi/wlan-fullmac.c \
	src/kern/net/wifi/wlan-l2.c \
	src/kern/net/wifi/wlan.c
endif

# [USB CDC driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_CDC)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_CDC=1
KERN_SOURCES += \
	src/drivers/usb/usb-cdc-notification.c
endif

# [USB CDC NCM Ethernet driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_CDC_NCM)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_CDC_NCM=1
KERN_SOURCES += \
	src/drivers/usb/usb-cdc-ncm.c \
	src/drivers/usb/usb-cdc-ncm-net.c
endif

# [USB CDC ECM Ethernet driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_CDC_ECM)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_CDC_ECM=1
KERN_SOURCES += \
	src/drivers/usb/usb-cdc-ecm.c
endif

# [Realtek RTL8125 PCIe Ethernet driver]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_RTL8125)),)
KERN_CPPFLAGS += \
	-DCONFIG_DRIVER_PCI_RTL8125=1 \
	-DCONFIG_DRIVER_PCI_RTL8125_PROFILE=$(CONFIG_DRIVER_PCI_RTL8125_PROFILE)
KERN_SOURCES += \
	src/drivers/ethernet/rtl8125.c
endif

# [Realtek RTL8156/8157 USB Ethernet vendor mode driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_RTL815X)),)
KERN_SOURCES += \
	src/drivers/usb/usb-rtl815x.c
endif

# [RPi4 Ethernet driver]
ifneq ($(filter y,$(CONFIG_DRIVER_RPI4_ETHERNET)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_RPI4_ETHERNET=1
KERN_SOURCES += \
	src/drivers/platform/rpi4/rpi4-ethernet.c \
	src/drivers/ethernet/bcm54213pe.c
endif

# [RPi5 Ethernet driver]
ifneq ($(filter y,$(CONFIG_DRIVER_RPI5_ETHERNET)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_RPI5_ETHERNET=1
KERN_SOURCES += \
	src/drivers/ethernet/cadence-gem.c \
	src/drivers/ethernet/mii-phy.c \
	src/drivers/ethernet/bcm54213pe.c
endif

# [Vision Five 2 Ethernet driver]
ifneq ($(filter y,$(CONFIG_DRIVER_VF2_ETHERNET)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_VF2_ETHERNET=1
KERN_SOURCES += \
	src/drivers/vf2/ethernet.c
endif

# [PC/AT NE2000-compatible ISA Ethernet driver]
ifneq ($(filter y,$(CONFIG_DRIVER_NE2000)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_NE2000=1
KERN_SOURCES += \
	src/drivers/pci/ne2000.c
endif

# [PC-98 LGY98 C-BUS Ethernet driver]
ifneq ($(filter y,$(CONFIG_DRIVER_LGY98)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_LGY98=1
KERN_SOURCES += \
	src/drivers/pci/lgy98.c
endif

# [Intel AX211 PCIe WiFi driver]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_INTEL_AX211)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI_INTEL_AX211=1
KERN_SOURCES += \
	src/drivers/wifi/intel-ax211/intel-ax211.c \
	src/drivers/wifi/intel-ax211/intel-ax211-assoc.c \
	src/drivers/wifi/intel-ax211/intel-ax211-boot.c \
	src/drivers/wifi/intel-ax211/intel-ax211-bss.c \
	src/drivers/wifi/intel-ax211/intel-ax211-command.c \
	src/drivers/wifi/intel-ax211/intel-ax211-dma.c \
	src/drivers/wifi/intel-ax211/intel-ax211-firmware.c \
	src/drivers/wifi/intel-ax211/intel-ax211-init.c \
	src/drivers/wifi/intel-ax211/intel-ax211-key.c \
	src/drivers/wifi/intel-ax211/intel-ax211-mmio.c \
	src/drivers/wifi/intel-ax211/intel-ax211-pci-mmio.c \
	src/drivers/wifi/intel-ax211/intel-ax211-pci.c \
	src/drivers/wifi/intel-ax211/intel-ax211-protocol.c \
	src/drivers/wifi/intel-ax211/intel-ax211-runtime-start.c \
	src/drivers/wifi/intel-ax211/intel-ax211-runtime.c \
	src/drivers/wifi/intel-ax211/intel-ax211-rx.c \
	src/drivers/wifi/intel-ax211/intel-ax211-scan-session.c \
	src/drivers/wifi/intel-ax211/intel-ax211-scan.c \
	src/drivers/wifi/intel-ax211/intel-ax211-transport-backend.c \
	src/drivers/wifi/intel-ax211/intel-ax211-transport.c \
	src/drivers/wifi/intel-ax211/intel-ax211-tx-ring.c \
	src/drivers/wifi/intel-ax211/intel-ax211-tx.c
endif

# [Realtek RTL8822BU USB WiFi driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_RTL8822BU)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_RTL8822BU=1
KERN_SOURCES += \
	src/drivers/usb/usb-rtl8822bu.c \
	src/drivers/wifi/rtl8822b/rtl8822b.c \
	src/drivers/wifi/rtl8822b/rtl8822b-security.c
endif

# [RPi4 WiFi driver]
ifneq ($(filter y,$(CONFIG_DRIVER_RPI4_WIFI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_RPI4_WIFI=1
KERN_SOURCES += \
	src/drivers/rpi4/wifi.c
endif

# [RPi5 WiFi driver]
ifneq ($(filter y,$(CONFIG_DRIVER_RPI5_WIFI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_RPI5_WIFI=1
KERN_SOURCES += \
	src/drivers/rpi5/wifi.c
endif

# [GPU Framework]
#  - TODO: Move vk-codec.c to src/drivers/gpu/compiler/
ifneq ($(filter y,$(CONFIG_DRIVER_GPU)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_GPU=1
KERN_SOURCES += \
	src/drivers/gpu/gpu.c \
	src/drivers/gpu/gpu-fence.c \
	src/drivers/gpu/compiler/spirv.c \
	src/drivers/gpu/compiler/optimize.c \
	src/drivers/gpu/i915/vk/vk-codec.c \
	src/drivers/generic/backlight.c
KERN_CPPFLAGS += \
	-DCONFIG_GPU_RESERVATION_MS=$(CONFIG_GPU_JOB_RESERVATION_MS) \
	-DCONFIG_GPU_EXECUTION_MS=$(CONFIG_GPU_JOB_EXECUTION_MS) \
	-DCONFIG_GPU_STOP_MS=$(CONFIG_GPU_JOB_STOP_MS) \
	-DCONFIG_GPU_CONTROL_MS=$(CONFIG_GPU_CONTROL_MS)
endif

# [Legacy 2D Graphics Framework]
ifneq ($(filter y,$(CONFIG_DRIVER_GRAPHICS)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_GRAPHICS=1
KERN_SOURCES += \
	src/drivers/platform/pcat/graphics/vgafont.c
endif

# [Intel i915 GPU Driver]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_I915)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI_I915=1
KERN_SOURCES += \
	src/drivers/gpu/i915/i915.c \
	src/drivers/gpu/i915/command.c \
	src/drivers/gpu/i915/compiler/compile.c \
	src/drivers/gpu/i915/compiler/eu.c \
	src/drivers/gpu/i915/context.c \
	src/drivers/gpu/i915/defaults.c \
	src/drivers/gpu/i915/device.c \
	src/drivers/gpu/i915/device-info.c \
	src/drivers/gpu/i915/display/backlight.c \
	src/drivers/gpu/i915/display/bootfb.c \
	src/drivers/gpu/i915/display/clock.c \
	src/drivers/gpu/i915/display/control.c \
	src/drivers/gpu/i915/display/run-log.c \
	src/drivers/gpu/i915/display/display.c \
	src/drivers/gpu/i915/display/display-power.c \
	src/drivers/gpu/i915/display/dmc-shell.c \
	src/drivers/gpu/i915/display/xd-clock.c \
	src/drivers/gpu/i915/display/xd-ddi.c \
	src/drivers/gpu/i915/display/xd-dmc.c \
	src/drivers/gpu/i915/display/edp-device.c \
	src/drivers/gpu/i915/display/xd9-backlight.c \
	src/drivers/gpu/i915/display/xd9-firmware.c \
	src/drivers/gpu/i915/display/xd9-flip.c \
	src/drivers/gpu/i915/display/xd9-node.c \
	src/drivers/gpu/i915/display/xd9-outputs.c \
	src/drivers/gpu/i915/display/xd9-step.c \
	src/drivers/gpu/i915/display/hpd.c \
	src/drivers/gpu/i915/display/interrupts.c \
	src/drivers/gpu/i915/display/output.c \
	src/drivers/gpu/i915/display/present.c \
	src/drivers/gpu/i915/display/vga.c \
	src/drivers/gpu/i915/display/resident.c \
	src/drivers/gpu/i915/display/scanout.c \
	src/drivers/gpu/i915/display/vbt-shell.c \
	src/drivers/gpu/i915/dma.c \
	src/drivers/gpu/i915/engine.c \
	src/drivers/gpu/i915/firmware.c \
	src/drivers/gpu/i915/ggtt.c \
	src/drivers/gpu/i915/gt-power.c \
	src/drivers/gpu/i915/irq.c \
	src/drivers/gpu/i915/job.c \
	src/drivers/gpu/i915/memory.c \
	src/drivers/gpu/i915/migrate.c \
	src/drivers/gpu/i915/mmio.c \
	src/drivers/gpu/i915/park.c \
	src/drivers/gpu/i915/pci.c \
	src/drivers/gpu/i915/perf.c \
	src/drivers/gpu/i915/power.c \
	src/drivers/gpu/i915/ppgtt.c \
	src/drivers/gpu/i915/product.c \
	src/drivers/gpu/i915/pxp.c \
	src/drivers/gpu/i915/request.c \
	src/drivers/gpu/i915/request-queue.c \
	src/drivers/gpu/i915/reset.c \
	src/drivers/gpu/i915/resource.c \
	src/drivers/gpu/i915/runtime-pm.c \
	src/drivers/gpu/i915/session.c \
	src/drivers/gpu/i915/submit.c \
	src/drivers/gpu/i915/sync.c \
	src/drivers/gpu/i915/tlb.c \
	src/drivers/gpu/i915/trace.c \
	src/drivers/gpu/i915/verify-workarounds.c \
	src/drivers/gpu/i915/workarounds.c \
	src/drivers/gpu/i915/worker.c \
	src/drivers/gpu/i915/workqueue.c \
	src/drivers/gpu/i915/mmio-record.c \
	src/drivers/gpu/i915/display/tc-port.c \
	src/drivers/gpu/i915/display/dp-ext-kern.c \
	src/drivers/gpu/i915/display/head.c \
	src/drivers/gpu/i915/display/xd-io.c \
	src/drivers/gpu/i915/display/xd-platform.c \
	src/drivers/gpu/i915/display/xd-display.c \
	src/drivers/gpu/i915/display/xd-diag.c \
	src/drivers/gpu/i915/display/xd-bandwidth.c \
	src/drivers/gpu/i915/display/survey.c \
	src/drivers/gpu/i915/display/xd-vbt.c \
	src/drivers/gpu/i915/display/xd-edid.c \
	src/drivers/gpu/i915/display/xd-aux.c \
	src/drivers/gpu/i915/display/xd-gmbus.c \
	src/drivers/gpu/i915/display/xd-panel.c \
	src/drivers/gpu/i915/display/xd-power.c \
	src/drivers/gpu/i915/display/xd-tc.c \
	src/drivers/gpu/i915/display/xd-tc-port.c \
	src/drivers/gpu/i915/display/xd-dp.c \
	src/drivers/gpu/i915/display/xd-pipe.c \
	src/drivers/gpu/i915/display/xd-dbuf.c \
	src/drivers/gpu/i915/display/xd-core.c \
	src/drivers/gpu/i915/display/xd-takeover.c \
	src/drivers/gpu/i915/display/xd-screen.c \
	src/drivers/gpu/i915/display/xd-output.c \
	src/drivers/gpu/i915/display/xd-edp.c \
	src/drivers/gpu/i915/display/xd-picture.c \
	src/drivers/gpu/i915/display/xd-hpd.c \
	src/drivers/gpu/i915/vk/vk-batch.c \
	src/drivers/gpu/i915/vk/vk-blit.c \
	src/drivers/gpu/i915/vk/vk-command.c \
	src/drivers/gpu/i915/vk/vk-compute.c \
	src/drivers/gpu/i915/vk/vk-descriptor.c \
	src/drivers/gpu/i915/vk/vk-dispatch.c \
	src/drivers/gpu/i915/vk/vk-draw.c \
	src/drivers/gpu/i915/vk/vk-fence.c \
	src/drivers/gpu/i915/vk/vk-forget.c \
	src/drivers/gpu/i915/vk/vk-image.c \
	src/drivers/gpu/i915/vk/vk-instance.c \
	src/drivers/gpu/i915/vk/vk-math.c \
	src/drivers/gpu/i915/vk/vk-memory.c \
	src/drivers/gpu/i915/vk/vk-object.c \
	src/drivers/gpu/i915/vk/vk-objects.c \
	src/drivers/gpu/i915/vk/vk-pipeline.c \
	src/drivers/gpu/i915/vk/vk-pipeline-prepare.c \
	src/drivers/gpu/i915/vk/vk-render-pass.c \
	src/drivers/gpu/i915/vk/vk-reply.c \
	src/drivers/gpu/i915/vk/vk-state.c \
	src/drivers/gpu/i915/vk/vk-sync.c \
	src/drivers/gpu/i915/vk/vk-transport.c \
	src/drivers/gpu/i915/vk/vk-video.c \
	src/drivers/gpu/i915/vk/vk-video-h264-tables.c \
	src/drivers/gpu/i915/vk/vk-video-mfx.c \
	src/drivers/gpu/i915/vk/vk-video-h265.c \
	src/drivers/gpu/i915/vk/vk-video-hcp.c \
	src/drivers/gpu/i915/vk/vk.c
endif

# [NVIDIA RTX GPU Driver]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_NVRTX)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI_NVRTX=1
KERN_SOURCES += \
	src/drivers/gpu/nvrtx/nvrtx.c \
	src/drivers/gpu/nvrtx/discover.c \
	src/drivers/gpu/nvrtx/identify.c \
	src/drivers/gpu/nvrtx/stage.c \
	src/drivers/gpu/nvrtx/products.c \
	src/drivers/gpu/nvrtx/wire.c \
	src/drivers/gpu/nvrtx/image.c \
	src/drivers/gpu/nvrtx/layout.c \
	src/drivers/gpu/nvrtx/radix3.c \
	src/drivers/gpu/nvrtx/queue.c \
	src/drivers/gpu/nvrtx/vbios.c \
	src/drivers/gpu/nvrtx/hold.c \
	src/drivers/gpu/nvrtx/file.c \
	src/drivers/gpu/nvrtx/memory.c \
	src/drivers/gpu/nvrtx/rom.c \
	src/drivers/gpu/nvrtx/gsp.c \
	src/drivers/gpu/nvrtx/falcon.c \
	src/drivers/gpu/nvrtx/boot.c \
	src/drivers/gpu/nvrtx/records.c \
	src/drivers/gpu/nvrtx/rpc.c \
	src/drivers/gpu/nvrtx/mmu.c \
	src/drivers/gpu/nvrtx/objects.c \
	src/drivers/gpu/nvrtx/channel.c \
	src/drivers/gpu/nvrtx/gr.c \
	src/drivers/gpu/nvrtx/compute.c \
	src/drivers/gpu/nvrtx/draw-push.c \
	src/drivers/gpu/nvrtx/draw.c \
	src/drivers/gpu/nvrtx/heap.c \
	src/drivers/gpu/nvrtx/vram.c \
	src/drivers/gpu/nvrtx/compute-launch.c \
	src/drivers/gpu/nvrtx/runtime.c \
	src/drivers/gpu/nvrtx/vk-node.c \
	src/drivers/gpu/nvrtx/work-records.c \
	src/drivers/gpu/nvrtx/work.c \
	src/drivers/gpu/nvrtx/recover.c \
	src/drivers/gpu/nvrtx/nvdec-records.c \
	src/drivers/gpu/nvrtx/nvdec.c \
	src/drivers/gpu/nvrtx/nvenc-records.c \
	src/drivers/gpu/nvrtx/nvenc.c \
	src/drivers/gpu/nvrtx/nvenc-test.c \
	src/drivers/gpu/nvrtx/perf.c \
	src/drivers/gpu/nvrtx/node.c \
	src/drivers/gpu/nvrtx/display/present.c \
	src/drivers/gpu/nvrtx/display/cursor.c \
	src/drivers/gpu/nvrtx/share.c \
	src/drivers/gpu/nvrtx/display/display.c \
	src/drivers/gpu/nvrtx/display/disp-records.c \
	src/drivers/gpu/nvrtx/display/disp-first.c \
	src/drivers/gpu/nvrtx/display/scanout.c \
	src/drivers/gpu/nvrtx/compiler/sm75/encode.c \
	src/drivers/gpu/nvrtx/compiler/sm75/operands.c \
	src/drivers/gpu/nvrtx/compiler/sm75/schedule.c \
	src/drivers/gpu/nvrtx/compiler/sm75/compile.c \
	src/drivers/gpu/nvrtx/compiler/sm75/registers.c \
	src/drivers/gpu/nvrtx/compiler/sm75/lower.c \
	src/drivers/gpu/nvrtx/compiler/sm75/lower-divide.c \
	src/drivers/gpu/nvrtx/compiler/sm75/lower-memory.c \
	src/drivers/gpu/nvrtx/compiler/sm75/lower-graphics.c \
	src/drivers/gpu/nvrtx/vk/vk-object.c \
	src/drivers/gpu/nvrtx/vk/vk-session.c \
	src/drivers/gpu/nvrtx/vk/vk-stream.c \
	src/drivers/gpu/nvrtx/vk/vk-dispatch.c \
	src/drivers/gpu/nvrtx/vk/vk-device.c \
	src/drivers/gpu/nvrtx/vk/vk-reply.c \
	src/drivers/gpu/nvrtx/vk/vk-memory.c \
	src/drivers/gpu/nvrtx/vk/vk-resource.c \
	src/drivers/gpu/nvrtx/vk/vk-layout.c \
	src/drivers/gpu/nvrtx/vk/vk-target.c \
	src/drivers/gpu/nvrtx/vk/vk-pipeline.c \
	src/drivers/gpu/nvrtx/vk/vk-command.c \
	src/drivers/gpu/nvrtx/vk/vk-record.c \
	src/drivers/gpu/nvrtx/vk/vk-execute.c \
	src/drivers/gpu/nvrtx/vk/vk-queue.c \
	src/drivers/gpu/nvrtx/vk/exec-push.c \
	src/drivers/gpu/nvrtx/vk/exec-copy.c \
	src/drivers/gpu/nvrtx/vk/vk-video.c \
	src/drivers/gpu/nvrtx/vk/vk-video-h264.c \
	src/drivers/gpu/nvrtx/vk/vk-video-h265.c
endif

# [AMD RDNA GPU driver]
ifeq ($(CONFIG_DRIVER_PCI_AMDRDNA),y)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI_AMDRDNA=1
KERN_SOURCES += \
	src/drivers/gpu/amdrdna/amdrdna.c \
	src/drivers/gpu/amdrdna/stage.c \
	src/drivers/gpu/amdrdna/mmio.c \
	src/drivers/gpu/amdrdna/identify.c \
	src/drivers/gpu/amdrdna/readout.c \
	src/drivers/gpu/amdrdna/vbios.c \
	src/drivers/gpu/amdrdna/control.c \
	src/drivers/gpu/amdrdna/bytes.c \
	src/drivers/gpu/amdrdna/block-table.c \
	src/drivers/gpu/amdrdna/boot-database.c \
	src/drivers/gpu/amdrdna/vbios-table.c \
	src/drivers/gpu/amdrdna/layout.c \
	src/drivers/gpu/amdrdna/products.c \
	src/drivers/gpu/amdrdna/bytecode.c \
	src/drivers/gpu/amdrdna/edid.c \
	src/drivers/gpu/amdrdna/sink.c \
	src/drivers/gpu/amdrdna/display-command.c \
	src/drivers/gpu/amdrdna/relight.c \
	src/drivers/gpu/amdrdna/mode-plan.c \
	src/drivers/gpu/amdrdna/modeset.c \
	src/drivers/gpu/amdrdna/hotplug.c \
	src/drivers/gpu/amdrdna/dcn-registers.c \
	src/drivers/gpu/amdrdna/picture.c \
	src/drivers/gpu/amdrdna/gmc.c \
	src/drivers/gpu/amdrdna/ih.c \
	src/drivers/gpu/amdrdna/span-pool.c \
	src/drivers/gpu/amdrdna/hub-registers.c \
	src/drivers/gpu/amdrdna/vm-format.c \
	src/drivers/gpu/amdrdna/interrupt-format.c \
	src/drivers/gpu/amdrdna/firmware-format.c \
	src/drivers/gpu/amdrdna/psp-format.c \
	src/drivers/gpu/amdrdna/thermal-format.c \
	src/drivers/gpu/amdrdna/firmware.c \
	src/drivers/gpu/amdrdna/training.c \
	src/drivers/gpu/amdrdna/psp.c \
	src/drivers/gpu/amdrdna/smu.c \
	src/drivers/gpu/amdrdna/display-memory.c \
	src/drivers/gpu/amdrdna/command-format.c \
	src/drivers/gpu/amdrdna/queue-format.c \
	src/drivers/gpu/amdrdna/settings.c \
	src/drivers/gpu/amdrdna/check-program.c \
	src/drivers/gpu/amdrdna/cp.c \
	src/drivers/gpu/amdrdna/sdma.c \
	src/drivers/gpu/amdrdna/engines.c \
	src/drivers/gpu/amdrdna/context-pool.c \
	src/drivers/gpu/amdrdna/space.c \
	src/drivers/gpu/amdrdna/node.c \
	src/drivers/gpu/amdrdna/runtime.c \
	src/drivers/gpu/amdrdna/vk-node.c \
	src/drivers/gpu/amdrdna/sessions.c \
	src/drivers/gpu/amdrdna/display-format.c \
	src/drivers/gpu/amdrdna/display.c \
	src/drivers/gpu/amdrdna/present.c \
	src/drivers/gpu/amdrdna/reset.c \
	src/drivers/gpu/amdrdna/reset-display.c \
	src/drivers/gpu/amdrdna/reset-format.c \
	src/drivers/gpu/amdrdna/compiler/gfx10-compile.c \
	src/drivers/gpu/amdrdna/compiler/gfx10-encode.c \
	src/drivers/gpu/amdrdna/compiler/gfx10-wait.c \
	src/drivers/gpu/amdrdna/compiler/gfx10-program.c \
	src/drivers/gpu/amdrdna/vk/vk-object.c \
	src/drivers/gpu/amdrdna/vk/vk-session.c \
	src/drivers/gpu/amdrdna/vk/vk-stream.c \
	src/drivers/gpu/amdrdna/vk/vk-dispatch.c \
	src/drivers/gpu/amdrdna/vk/vk-device.c \
	src/drivers/gpu/amdrdna/vk/vk-reply.c \
	src/drivers/gpu/amdrdna/vk/vk-memory.c \
	src/drivers/gpu/amdrdna/vk/vk-resource.c \
	src/drivers/gpu/amdrdna/vk/vk-layout.c \
	src/drivers/gpu/amdrdna/vk/vk-target.c \
	src/drivers/gpu/amdrdna/vk/vk-pipeline.c \
	src/drivers/gpu/amdrdna/vk/vk-command.c \
	src/drivers/gpu/amdrdna/vk/vk-record.c \
	src/drivers/gpu/amdrdna/vk/vk-execute.c \
	src/drivers/gpu/amdrdna/vk/vk-queue.c \
	src/drivers/gpu/amdrdna/vk/vk-pm4.c \
	src/drivers/gpu/amdrdna/vk/vk-state.c \
	src/drivers/gpu/amdrdna/vk/vk-descriptor.c \
	src/drivers/gpu/amdrdna/vk/vk-quad.c \
	src/drivers/gpu/amdrdna/vk/vk-float.c
endif

# [RPi4 VideoCore VI GPU driver]
ifeq ($(CONFIG_DRIVER_RPI4_VIDEOCORE_VI),y)
KERN_CPPFLAGS += -DCONFIG_DRIVER_RPI4_VIDEOCORE_VI=1
KERN_SOURCES += \
	src/drivers/gpu/videocore/v3d.c \
	src/drivers/gpu/videocore/v3d-power.c \
	src/drivers/gpu/videocore/v3d-hardware.c \
	src/drivers/gpu/videocore/v3d-job.c \
	src/drivers/gpu/videocore/v3d-diagnostic.c \
	src/drivers/gpu/videocore/v3d-tfu-probe.c \
	src/drivers/gpu/videocore/v3d-memory.c \
	src/drivers/gpu/videocore/attach.c \
	src/drivers/gpu/videocore/stage.c \
	src/drivers/gpu/videocore/mmu.c \
	src/drivers/gpu/videocore/fdt-util.c \
	src/drivers/gpu/videocore/firmware.c \
	src/drivers/gpu/videocore/cl.c \
	src/drivers/gpu/videocore/compiler/qpu.c \
	src/drivers/gpu/videocore/buffer.c \
	src/drivers/gpu/videocore/share.c \
	src/drivers/gpu/videocore/render-device.c \
	src/drivers/gpu/videocore/render-runtime.c \
	src/drivers/gpu/videocore/render-worker.c \
	src/drivers/gpu/videocore/native-bin.c \
	src/drivers/gpu/videocore/native-pass.c \
	src/drivers/gpu/videocore/native-colour.c \
	src/drivers/gpu/videocore/native-shader.c \
	src/drivers/gpu/videocore/native-texture.c \
	src/drivers/gpu/videocore/native-miptree.c \
	src/drivers/gpu/videocore/native-storage.c \
	src/drivers/gpu/videocore/native-viewport.c \
	src/drivers/gpu/videocore/display/display.c \
	src/drivers/gpu/videocore/display/display-program.c \
	src/drivers/gpu/videocore/display/display-start.c \
	src/drivers/gpu/videocore/display/display-execute.c \
	src/drivers/gpu/videocore/display/display-flip.c \
	src/drivers/gpu/videocore/display/display-device.c \
	src/drivers/gpu/videocore/display/display-irq.c
	src/drivers/gpu/videocore/display/readout.c \
	src/drivers/gpu/videocore/display/list.c \
	src/drivers/gpu/videocore/compiler/shader.c \
	src/drivers/gpu/videocore/compiler/shader-analyze.c \
	src/drivers/gpu/videocore/compiler/shader-lower.c \
	src/drivers/gpu/videocore/compiler/shader-output.c \
	src/drivers/gpu/videocore/compiler/shader-schedule.c \
	src/drivers/gpu/videocore/vk/vulkan-object.c \
	src/drivers/gpu/videocore/vk/vulkan-session.c \
	src/drivers/gpu/videocore/vk/vulkan-stream.c \
	src/drivers/gpu/videocore/vk/vulkan-dispatch.c \
	src/drivers/gpu/videocore/vk/vulkan-device.c \
	src/drivers/gpu/videocore/vk/vulkan-sync.c \
	src/drivers/gpu/videocore/vk/vulkan-queue.c \
	src/drivers/gpu/videocore/vk/vulkan-query.c \
	src/drivers/gpu/videocore/vk/vulkan-memory.c \
	src/drivers/gpu/videocore/vk/vulkan-resource.c \
	src/drivers/gpu/videocore/vk/vulkan-input.c \
	src/drivers/gpu/videocore/vk/vulkan-layout.c \
	src/drivers/gpu/videocore/vk/vulkan-layout-compat.c \
	src/drivers/gpu/videocore/vk/vulkan-descriptor-pool.c \
	src/drivers/gpu/videocore/vk/vulkan-descriptor-sets.c \
	src/drivers/gpu/videocore/vk/vulkan-descriptor-update.c \
	src/drivers/gpu/videocore/vk/vulkan-target.c \
	src/drivers/gpu/videocore/vk/vulkan-pipeline-build.c \
	src/drivers/gpu/videocore/vk/vulkan-pipeline-state.c \
	src/drivers/gpu/videocore/vk/vulkan-pipeline.c \
	src/drivers/gpu/videocore/vk/vulkan-pipeline-decode.c \
	src/drivers/gpu/videocore/vk/vulkan-command-pool.c \
	src/drivers/gpu/videocore/vk/vulkan-command-batch.c \
	src/drivers/gpu/videocore/vk/vulkan-command-buffer.c \
	src/drivers/gpu/videocore/vk/vulkan-barrier.c \
	src/drivers/gpu/videocore/vk/vulkan-barrier-decode.c \
	src/drivers/gpu/videocore/vk/vulkan-barrier-validate.c \
	src/drivers/gpu/videocore/vk/vulkan-record.c \
	src/drivers/gpu/videocore/vk/vulkan-record-decode.c \
	src/drivers/gpu/videocore/vk/vulkan-record-validate.c \
	src/drivers/gpu/videocore/vk/vulkan-draw.c \
	src/drivers/gpu/videocore/vk/vulkan-draw-validate.c \
	src/drivers/gpu/videocore/vk/vulkan-prepared.c \
	src/drivers/gpu/videocore/vk/vulkan-uniform.c \
	src/drivers/gpu/videocore/vk/vulkan-native-draw.c \
	src/drivers/gpu/videocore/vk/vulkan-native-bin.c \
	src/drivers/gpu/videocore/vk/vulkan-native-pass.c \
	src/drivers/gpu/videocore/vk/vulkan-native-execute.c \
	src/drivers/gpu/videocore/vk/vulkan-native-job.c \
	src/drivers/gpu/videocore/vk/vulkan-buffer-copy.c \
	src/drivers/gpu/videocore/vk/vulkan-transfer.c \
	src/drivers/gpu/videocore/vk/vulkan-native-image.c \
	src/drivers/gpu/videocore/vk/vulkan-video.c
endif

# [RPi5 VideoCore VII GPU driver]
ifeq ($(CONFIG_DRIVER_RPI5_VIDEOCORE_VI),y)
KERN_CPPFLAGS += -DCONFIG_DRIVER_RPI5_VIDEOCORE_VI=1
KERN_SOURCES += \
	src/drivers/gpu/videocore/v3d71.c \
	src/drivers/gpu/videocore/v3d71-firmware.c \
	src/drivers/gpu/videocore/v3d71-hardware.c \
	src/drivers/gpu/videocore/v3d71-job.c \
	src/drivers/gpu/videocore/v3d71-cl.c \
	src/drivers/gpu/videocore/v3d71-diagnostic.c \
	src/drivers/gpu/videocore/v3d-memory.c \
	src/drivers/gpu/videocore/stage.c \
	src/drivers/gpu/videocore/fdt-util.c \
	src/drivers/gpu/videocore/mmu.c \
	src/drivers/gpu/videocore/buffer.c \
	src/drivers/gpu/videocore/attach71.c \
	src/drivers/gpu/videocore/share.c \
	src/drivers/gpu/videocore/render-device.c \
	src/drivers/gpu/videocore/render-runtime.c \
	src/drivers/gpu/videocore/render-worker.c \
	src/drivers/gpu/videocore/hevc-decoder.c \
	src/drivers/gpu/videocore/native-colour.c \
	src/drivers/gpu/videocore/native-miptree.c \
	src/drivers/gpu/videocore/native-storage.c \
	src/drivers/gpu/videocore/native71-bin.c \
	src/drivers/gpu/videocore/native71-pass.c \
	src/drivers/gpu/videocore/native71-shader.c \
	src/drivers/gpu/videocore/native71-texture.c \
	src/drivers/gpu/videocore/native71-viewport.c \
	src/drivers/gpu/videocore/display/hvs6.c \
	src/drivers/gpu/videocore/display/hvs6-flip.c \
	src/drivers/gpu/videocore/display/display-device.c \
	src/drivers/gpu/videocore/compiler/qpu71.c \
	src/drivers/gpu/videocore/compiler/shader.c \
	src/drivers/gpu/videocore/compiler/shader-analyze.c \
	src/drivers/gpu/videocore/compiler/shader71-lower.c \
	src/drivers/gpu/videocore/compiler/shader71-output.c \
	src/drivers/gpu/videocore/compiler/shader71-schedule.c \
	src/drivers/gpu/videocore/vk/vulkan-object.c \
	src/drivers/gpu/videocore/vk/vulkan-session.c \
	src/drivers/gpu/videocore/vk/vulkan-stream.c \
	src/drivers/gpu/videocore/vk/vulkan-dispatch.c \
	src/drivers/gpu/videocore/vk/vulkan-device.c \
	src/drivers/gpu/videocore/vk/vulkan-sync.c \
	src/drivers/gpu/videocore/vk/vulkan-queue.c \
	src/drivers/gpu/videocore/vk/vulkan-query.c \
	src/drivers/gpu/videocore/vk/vulkan-memory.c \
	src/drivers/gpu/videocore/vk/vulkan-resource.c \
	src/drivers/gpu/videocore/vk/vulkan-input.c \
	src/drivers/gpu/videocore/vk/vulkan-layout.c \
	src/drivers/gpu/videocore/vk/vulkan-layout-compat.c \
	src/drivers/gpu/videocore/vk/vulkan-descriptor-pool.c \
	src/drivers/gpu/videocore/vk/vulkan-descriptor-sets.c \
	src/drivers/gpu/videocore/vk/vulkan-descriptor-update.c \
	src/drivers/gpu/videocore/vk/vulkan-target.c \
	src/drivers/gpu/videocore/vk/vulkan-pipeline-build.c \
	src/drivers/gpu/videocore/vk/vulkan-pipeline-state.c \
	src/drivers/gpu/videocore/vk/vulkan-pipeline.c \
	src/drivers/gpu/videocore/vk/vulkan-pipeline-decode.c \
	src/drivers/gpu/videocore/vk/vulkan-command-pool.c \
	src/drivers/gpu/videocore/vk/vulkan-command-batch.c \
	src/drivers/gpu/videocore/vk/vulkan-command-buffer.c \
	src/drivers/gpu/videocore/vk/vulkan-barrier.c \
	src/drivers/gpu/videocore/vk/vulkan-barrier-decode.c \
	src/drivers/gpu/videocore/vk/vulkan-barrier-validate.c \
	src/drivers/gpu/videocore/vk/vulkan-record.c \
	src/drivers/gpu/videocore/vk/vulkan-record-decode.c \
	src/drivers/gpu/videocore/vk/vulkan-record-validate.c \
	src/drivers/gpu/videocore/vk/vulkan-draw.c \
	src/drivers/gpu/videocore/vk/vulkan-draw-validate.c \
	src/drivers/gpu/videocore/vk/vulkan-prepared.c \
	src/drivers/gpu/videocore/vk/vulkan-uniform.c \
	src/drivers/gpu/videocore/vk/vulkan-native-draw.c \
	src/drivers/gpu/videocore/vk/vulkan-native-bin.c \
	src/drivers/gpu/videocore/vk/vulkan-native-pass.c \
	src/drivers/gpu/videocore/vk/vulkan-native-execute.c \
	src/drivers/gpu/videocore/vk/vulkan-native-job.c \
	src/drivers/gpu/videocore/vk/vulkan-buffer-copy.c \
	src/drivers/gpu/videocore/vk/vulkan-transfer.c \
	src/drivers/gpu/videocore/vk/vulkan-native-image.c \
	src/drivers/gpu/videocore/vk/vulkan-video-hevc.c \
	src/drivers/gpu/videocore/vk/vulkan-video-hevc-slice.c \
	src/drivers/gpu/videocore/vk/vulkan-video.c
endif

# [Vision Five 2 Imagination PowerVR GPU and VPU driver]
ifneq ($(filter y,$(CONFIG_DRIVER_VF2_GPU_VPU)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_VF2_GPU_VPU=1
KERN_SOURCES += \
	src/drivers/platform/visionfive2/vf2-gpu.c \
	src/drivers/platform/visionfive2/vf2-pmu.c \
	src/drivers/platform/visionfive2/vf2-vpu.c \
	src/drivers/gpu/powervr/powervr.c \
	src/drivers/gpu/powervr/firmware-image.c \
	src/drivers/gpu/powervr/mips-table.c \
	src/drivers/gpu/powervr/retry.c \
	src/drivers/gpu/powervr/stage.c \
	src/drivers/gpu/powervr/stage-log.c \
	src/drivers/gpu/powervr/firmware-memory.c \
	src/drivers/gpu/powervr/firmware-file.c \
	src/drivers/gpu/powervr/firmware-boot.c \
	src/drivers/gpu/powervr/commands.c \
	src/drivers/gpu/powervr/coherency.c \
	src/drivers/gpu/powervr/gpu-mmu.c \
	src/drivers/gpu/powervr/render.c \
	src/drivers/gpu/powervr/render-queue.c \
	src/drivers/gpu/powervr/render-kick-layout.c \
	src/drivers/gpu/powervr/render-sr-program.c \
	src/drivers/gpu/powervr/render-context.c \
	src/drivers/gpu/powervr/render-command.c \
	src/drivers/gpu/powervr/render-job.c \
	src/drivers/gpu/powervr/transfer-context.c \
	src/drivers/gpu/powervr/transfer-job.c \
	src/drivers/gpu/powervr/render-target-layout.c \
	src/drivers/gpu/powervr/render-target.c \
	src/drivers/gpu/powervr/compiler/usc-encode.c \
	src/drivers/gpu/powervr/compiler/pds-encode.c \
	src/drivers/gpu/powervr/compute-program.c \
	src/drivers/gpu/powervr/fixed-area.c \
	src/drivers/gpu/powervr/render-runtime.c \
	src/drivers/gpu/powervr/image-state.c \
	src/drivers/gpu/powervr/descriptor-set.c \
	src/drivers/gpu/powervr/data-program.c \
	src/drivers/gpu/powervr/render-program.c \
	src/drivers/gpu/powervr/transfer-program.c \
	src/drivers/gpu/powervr/render-load-program.c \
	src/drivers/gpu/powervr/render-pass.c \
	src/drivers/gpu/powervr/render-draw.c \
	src/drivers/gpu/powervr/render-depth.c \
	src/drivers/gpu/powervr/video-state.c \
	src/drivers/gpu/powervr/display/display.c \
	src/drivers/gpu/powervr/display/dc8200.c \
	src/drivers/gpu/powervr/display/dc8200-adopt.c \
	src/drivers/gpu/powervr/display/dc8200-flip.c \
	src/drivers/gpu/powervr/display/dc8200-hdmi.c \
	src/drivers/gpu/powervr/display/dc8200-mode.c \
	src/drivers/gpu/powervr/compiler/shader-spirv.c \
	src/drivers/gpu/powervr/compiler/shader-spirv-interface.c \
	src/drivers/gpu/powervr/compiler/shader-spirv-control.c \
	src/drivers/gpu/powervr/compiler/shader-spirv-instruction.c \
	src/drivers/gpu/powervr/compiler/shader-backend.c \
	src/drivers/gpu/powervr/vk/vk-object.c \
	src/drivers/gpu/powervr/vk/vk-stream.c \
	src/drivers/gpu/powervr/vk/vk-device.c \
	src/drivers/gpu/powervr/vk/vk-memory.c \
	src/drivers/gpu/powervr/vk/vk-image.c \
	src/drivers/gpu/powervr/vk/vk-layout.c \
	src/drivers/gpu/powervr/vk/vk-command.c \
	src/drivers/gpu/powervr/vk/vk-queue.c \
	src/drivers/gpu/powervr/vk/vk-pass.c \
	src/drivers/gpu/powervr/vk/vk-transfer.c \
	src/drivers/gpu/powervr/vk/vk-query.c \
	src/drivers/gpu/powervr/vk/vk-video.c \
	src/drivers/gpu/powervr/vk/vk-video-h265.c \
	src/drivers/gpu/powervr/vk/vk-draw.c \
	src/drivers/vpu/wave5-core.c \
	src/drivers/vpu/wave5-decode.c \
	src/drivers/vpu/wave5-video.c \
	src/drivers/vpu/vpu-bits.c \
	src/drivers/vpu/vpu-h264-ps.c \
	src/drivers/vpu/vpu-h265-ps.c \
	src/drivers/vpu/vpu-slots.c
endif

# [GHCI GPU driver]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_GHCI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI_GHCI=1
KERN_SOURCES += \
	src/drivers/gpu/ghci/ghci.c \
	src/drivers/gpu/ghci/pci-attach.c \
	src/drivers/gpu/ghci/cp.c \
	src/drivers/gpu/ghci/recover.c \
	src/drivers/gpu/ghci/service.c \
	src/drivers/gpu/ghci/memory.c \
	src/drivers/gpu/ghci/check.c \
	src/drivers/gpu/ghci/caps.c \
	src/drivers/gpu/ghci/cmd.c \
	src/drivers/gpu/ghci/vm.c \
	src/drivers/gpu/ghci/range.c \
	src/drivers/gpu/ghci/share.c \
	src/drivers/gpu/ghci/isa.c \
	src/drivers/gpu/ghci/jobs.c \
	src/drivers/gpu/ghci/capset.c \
	src/drivers/gpu/ghci/display.c \
	src/drivers/gpu/ghci/display-scan.c \
	src/drivers/gpu/ghci/adv7511.c \
	src/drivers/gpu/ghci/compiler/gsi1-compile.c \
	src/drivers/gpu/ghci/compiler/gsi1-encode.c \
	src/drivers/gpu/ghci/compiler/gsi1-io.c \
	src/drivers/gpu/ghci/compiler/gsi1-memory.c \
	src/drivers/gpu/ghci/compiler/gsi1-texture.c \
	src/drivers/gpu/ghci/compiler/gsi1-image.c \
	src/drivers/gpu/ghci/compiler/gsi1-schedule.c \
	src/drivers/gpu/ghci/compiler/gsi1-spill.c \
	src/drivers/gpu/ghci/vk/vk-node.c \
	src/drivers/gpu/ghci/vk/vk-stream.c \
	src/drivers/gpu/ghci/vk/vk-dispatch.c \
	src/drivers/gpu/ghci/vk/vk-object.c \
	src/drivers/gpu/ghci/vk/vk-reply.c \
	src/drivers/gpu/ghci/vk/vk-caps.c \
	src/drivers/gpu/ghci/vk/vk-format.c \
	src/drivers/gpu/ghci/vk/vk-device.c \
	src/drivers/gpu/ghci/vk/vk-memory.c \
	src/drivers/gpu/ghci/vk/vk-command.c \
	src/drivers/gpu/ghci/vk/vk-queue.c \
	src/drivers/gpu/ghci/vk/vk-layout.c \
	src/drivers/gpu/ghci/vk/vk-pipeline.c \
	src/drivers/gpu/ghci/vk/vk-pass.c \
	src/drivers/gpu/ghci/vk/vk-execute.c \
	src/drivers/gpu/ghci/vk/vk-meta.c
endif

# [Venus GPU Driver]
ifneq ($(filter y,$(CONFIG_DRIVER_PCI_VENUS)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_PCI_VENUS=1
KERN_SOURCES += \
	src/drivers/gpu/venus/transport.c \
	src/drivers/gpu/venus/venus.c \
	src/drivers/gpu/venus/display.c \
	src/drivers/gpu/venus/share.c
endif

# [Boot Linear Framebuffer driver]
ifneq ($(filter y,$(CONFIG_DRIVER_GRAPHICS_BOOTLFB)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_GRAPHICS_BOOTLFB=1
KERN_SOURCES += \
	src/drivers/generic/boot-fb.c \
	src/drivers/generic/fb-text.c \
	src/drivers/generic/graphics-device.c \
	src/drivers/platform/pcat/graphics/splash.c
endif

# [PC/AT VGA driver]
ifneq ($(filter y,$(CONFIG_DRIVER_GRAPHICS_VGA)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_GRAPHICS_VGA=1
KERN_SOURCES += \
	src/drivers/platform/pcat/graphics/pcat-graphics.c \
	src/drivers/platform/pcat/graphics/backend.c \
	src/drivers/platform/pcat/graphics/font.c \
	src/drivers/platform/pcat/graphics/text.c
endif

# [PC-98 GDC driver]
ifneq ($(filter y,$(CONFIG_DRIVER_GRAPHICS_PC98)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_GRAPHICS_PC98=1
KERN_SOURCES += \
	src/drivers/platform/pc98/graphics/pc98-graphics.c \
	src/drivers/platform/pc98/graphics/backend.c \
	src/drivers/platform/pc98/graphics/text.c
	src/drivers/platform/pc98/graphics/display-auto.c \
	src/drivers/platform/pc98/graphics/display-gdc.c \
	src/drivers/platform/pc98/graphics/display-cirrus.c \
	src/drivers/platform/pc98/graphics/display-glyph.c \
	src/drivers/platform/pc98/graphics/jisx0208.c
endif

# [Audio Core]
ifneq ($(filter y,$(CONFIG_DRIVER_AUDIO_CORE)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_AUDIO_CORE=1
KERN_SOURCES += \
	src/drivers/audio/audio.c
endif

# [Intel HD Audio driver]
ifneq ($(filter y,$(CONFIG_DRIVER_AUDIO_HDA)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_AUDIO_HDA=1
KERN_SOURCES += \
	src/drivers/pci/pci-hda/hda.c
endif

# [RPi4 PWM Audio driver]
ifneq ($(filter y,$(CONFIG_DRIVER_AUDIO_PWM)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_AUDIO_PWM=1
KERNEL_SOURCES += \
	src/drivers/platform/rpi4/rpi4-audio.c \
	src/drivers/platform/rpi4/rpi4-audio-ring.c
endif

# [RPi5 RP1 Audio driver]
#  - XXX: not implemented yet??
ifneq ($(filter y,$(CONFIG_DRIVER_AUDIO_RP1)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_AUDIO_RP1=1
KERNEL_SOURCES +=
endif

# [Vision Five 2 Audio driver]
ifneq ($(filter y,$(CONFIG_DRIVER_AUDIO_VF2)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_AUDIO_VF2=1
KERNEL_SOURCES += \
	src/drivers/platform/visionfive2/vf2-pwmdac.c
endif

# [Generic HID driver]
ifneq ($(filter y,$(CONFIG_DRIVER_HID)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_HID=1
KERN_SOURCES += \
	src/drivers/generic/input.c \
	src/drivers/generic/hid-input.c \
	src/drivers/generic/hid-report.c \
	src/drivers/generic/hid-digitizer.c \
	src/drivers/generic/hid-touch.c \
	src/drivers/generic/hidraw.c \
	src/drivers/generic/hidraw-describe.c
endif

# [USB Framework]
ifneq ($(filter y,$(CONFIG_DRIVER_USB)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB=1
KERN_SOURCES += \
	src/drivers/usb/usb.c
endif

# [USB 1.1 OHCI driver]
#  - Not implemented yet
ifneq ($(filter y,$(CONFIG_DRIVER_USB_OHCI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_OHCI=1
KERN_SOURCES += \
	src/drivers/pci/pci-ohci.c
endif

# [USB 1.1 UHCI driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_UHCI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_UHCI=1
KERN_SOURCES += \
	src/drivers/pci/pci-uhci.c
endif

# [USB 2.0 EHCI driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_EHCI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_EHCI=1
KERN_SOURCES += \
	src/drivers/pci/pci-ehci.c
endif

# [USB 3.0 XHCI driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_XHCI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_XHCI=1
KERN_SOURCES += \
	src/drivers/pci/pci-xhci.c \
	src/drivers/pci/pci-xhci-isoch.c
endif

# [USB-C USCI driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_USCI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_USCI=1
KERN_SOURCES += \
	src/drivers/typec/typec-kern.c \
	src/drivers/typec/typec.c \
	src/drivers/typec/ucsi-acpi.c \
	src/drivers/typec/ucsi.c
endif

# [USB Hub driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_HUB)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_HUB=1
KERN_SOURCES += \
	src/drivers/usb/usb-hub.c
endif

# [USB Human Interface Device (HID) driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_HID)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_HID=1
KERN_SOURCES += \
	src/drivers/usb/usb-hid.c
endif

# [USB Mass Storage Device (MSD) driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_STORAGE)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_STORAGE=1
KERN_SOURCES += \
	src/drivers/usb/usb-storage.c
endif

# [USB Attached SCSI Protocol (UASP) driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_UASP)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_UASP=1
KERN_SOURCES += \
	src/drivers/usb/usb-uas.c \
	src/drivers/usb/usb-uas-transport.c \
	src/drivers/usb/usb-uas-disk.c
endif

# [USB Chip Card Interface Device (CCID) driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_CCID)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_CCID=1
KERN_SOURCES += \
	src/drivers/usb/usb-ccid.c \
	src/drivers/usb/usb-ccid-proto.c
endif

# [USB Bluetooth driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_BT)),)
KERN_SOURCES += \
	src/drivers/usb/usb-bt.c
endif

# [USB Video Class (UVC) driver]
ifneq ($(filter y,$(CONFIG_DRIVER_USB_UVC)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_USB_UVC=1
KERN_SOURCES += \
	src/drivers/usb/usb-uvc.c \
	src/drivers/usb/usb-uvc-desc.c \
	src/drivers/usb/usb-uvc-stream.c \
	src/drivers/usb/usb-uvc-payload.c
endif

# [Generic Bluetooth HCI]
ifneq ($(filter y,$(CONFIG_DRIVER_BLUETOOTH)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_BLUETOOTH=1
KERN_SOURCES += \
	src/drivers/generic/bt-hci.c \
	src/drivers/generic/bt-hci-proto.c
endif

# [Bluetooth Input Bridge] (/dev/input/bridge)
ifneq ($(filter y,$(CONFIG_DRIVER_BLUETOOTH_INPUT_BRIDGE)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_BLUETOOTH_INPUT_BRIDGE=1
KERN_SOURCES += \
	src/drivers/generic/input-bridge.c \
	src/drivers/generic/input-bridge-setup.c
endif

# [RPi4 Bluetooth driver]
ifneq ($(filter y,$(CONFIG_DRIVER_BCM2711_BT)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_BCM2711_BT=1
KERN_SOURCES += \
	src/drivers/platform/rpi4/rpi4-bt.c \
	src/drivers/platform/rpi-common/rpi-bt-link.c
endif

# [RPi5 Onboard Bluetooth]
ifneq ($(filter y,$(CONFIG_DRIVER_BCM2711_BT)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_BCM2711_BT=1
KERN_SOURCES += \
	src/drivers/platform/rpi5/rpi5-bt.c \
	src/drivers/platform/rpi-common/rpi-bt-link.c
endif

# [Generic CPU performance monitoring driver] (/dev/thermal)
#  - XXX: TODO: move this module to drivers/
ifneq ($(filter y,$(CONFIG_KERN_CPU_PERF)),)
KERN_CPPFLAGS += -DCONFIG_KERN_CPU_PERF=1
KERN_SOURCES += \
	src/kern/cpuperf.c
endif

# [Smart Card driver] (/dev/smartcard)
ifneq ($(filter y,$(CONFIG_DRIVER_SMARTCARD)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_SMARTCARD=1
KERN_SOURCES += \
	src/drivers/generic/smartcard.c
endif

# [x86 Power Control driver]
ifneq ($(filter y,$(CONFIG_DRIVER_X86_POWER_CONTROL)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_X86_POWER_CONTROL=1
KERN_SOURCES += \
	src/drivers/platform/pcat/cpu-x86/cpux86-hwp.c \
	src/drivers/platform/pcat/cpu-x86/cpux86-pstate.c \
	src/drivers/platform/pcat/cpu-x86/cpux86-power.c \
	src/drivers/platform/pcat/cpu-x86/cpu-x86-control.c
endif

# [RPi5 processor temperature] 
ifneq ($(filter y,$(CONFIG_DRIVER_RPI5_TEMP)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_RPI5_TEMP=1
KERN_SOURCES += \
	src/drivers/platform/rpi5/rpi5-fan.c
endif

# [Camera Core (/dev/camN)]
ifneq ($(filter y,$(CONFIG_DRIVER_CAMERA)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_CAMERA=1
KERN_SOURCES += \
	src/drivers/camera/cam.c
endif

# [ACPI Core]
ifneq ($(filter y,$(CONFIG_DRIVER_ACPI)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_ACPI=1
KERN_SOURCES += \
	src/drivers/acpi/acpi-dell-codec.c \
	src/drivers/acpi/acpi-dell.c \
	src/drivers/acpi/acpi-dev.c \
	src/drivers/acpi/acpi-ec.c \
	src/drivers/acpi/acpi-event.c \
	src/drivers/acpi/acpi-kern.c \
	src/drivers/acpi/acpi-pci-power.c \
	src/drivers/acpi/acpi-pci-root.c \
	src/drivers/acpi/acpi-power.c \
	src/drivers/acpi/acpi-resource.c \
	src/drivers/acpi/acpi-sleep.c \
	src/drivers/acpi/acpi-tables.c \
	src/drivers/acpi/acpi-text.c \
	src/drivers/acpi/acpi-thermal.c \
	src/drivers/acpi/acpi-wmi-table.c \
	src/drivers/acpi/acpi-wmi.c \
	src/drivers/acpi/aml-define.c \
	src/drivers/acpi/aml-eval.c \
	src/drivers/acpi/aml-field.c \
	src/drivers/acpi/aml-namespace.c \
	src/drivers/acpi/aml-object.c \
	src/drivers/acpi/aml-operator.c \
	src/drivers/acpi/aml-osi.c \
	src/drivers/acpi/aml-skip.c \
	src/drivers/acpi/aml-stream.c \
	src/drivers/acpi/aml-sync.c \
	src/drivers/acpi/aml-table.c \
	src/drivers/acpi/aml-thread.c
endif

# [I2C Generic Driver]
ifneq ($(filter y,$(CONFIG_DRIVER_I2C)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_I2C=1
KERN_SOURCES += \
	src/drivers/i2c/i2c.c
endif

# [GPIO Generic Driver]
#  - XXX: Not implemented yet
ifneq ($(filter y,$(CONFIG_DRIVER_GPIO)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_GPIO=1
KERN_SOURCES +=
endif

# [Intel LPSS I2C / AMD I2C driver] (Synopsis DesignWare I2C)
ifneq ($(filter y,$(CONFIG_DRIVER_I2C_SYNOPSYS)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_I2C_SYNOPSYS=1
KERN_SOURCES += \
	src/drivers/i2c/lpss-i2c.c
endif

# [Intel LPSS GPIO driver]
ifneq ($(filter y,$(CONFIG_DRIVER_LPSS_GPIO)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_LPSS_GPIO=1
KERN_SOURCES += \
	src/drivers/gpio/intel-gpio.c
endif

# [AMD GPIO driver]
#  - XXX: Not implemented yet
ifneq ($(filter y,$(CONFIG_DRIVER_AMD_GPIO)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_AMD_GPIO=1
KERN_SOURCES +=
endif

# [I2C-HID driver]
ifneq ($(filter y,$(CONFIG_DRIVER_I2C_HID)),)
KERN_CPPFLAGS += -DCONFIG_DRIVER_I2C_HID=1
KERN_SOURCES += \
	src/drivers/i2c/i2c-hid.c
endif

########################################################################
# Platform Makefile
########################################################################

# Deterimen the build directory.
BUILD := build/$(PLATFORM)

# Determine the platform-specific Makefile
PLATFORM_MAKEFILE := platform/$(PLATFORM)/Makefile

# Fail if the platform Makefile doesn't exist.
ifeq ($(wildcard $(PLATFORM_MAKEFILE)),)
ifneq ($(strip $(PLATFORM)),)
$(error Unsupported PLATFORM '$(PLATFORM)' in config.mk)
endif
endif

# Include the platform-specific Makefile.
include $(PLATFORM_MAKEFILE)

########################################################################
# Packages
########################################################################

USERLAND_PACKAGE_BINDIR   := /usr/bin
USERLAND_PACKAGES         :=
USERLAND_DOWNLOAD_TARGETS :=
USERLAND_PATCH_TARGETS    :=

define USERLAND_PACKAGE
USERLAND_PACKAGES += $(1)
USERLAND_$(1)_LABEL := $(2)
USERLAND_$(1)_PLATFORMS := $(if $(filter base comp desktop firmware packages/%,$(8)),*,$(3))
USERLAND_$(1)_DEFAULT := $(4)
USERLAND_$(1)_CLASS := $(5)
USERLAND_$(1)_SOURCES := $(6)
USERLAND_$(1)_SELECTABLE := $(7)
USERLAND_$(1)_MENU := $(8)
USERLAND_$(1)_PACKAGE := $(9)
USERLAND_$(1)_REQUIRE := $(10)
USERLAND_$(1)_MODE := $(11)
USERLAND_$(1)_TYPE := $(if $(12),$(12),$(if $(filter library,$(5)),library,program))
USERLAND_$(1)_DATA := $(13)
USERLAND_$(1)_HEADERS := $(14)
USERLAND_$(1)_INSTALL_DIR := $(if $(15),$(15),bin)
endef

USERLAND_PACKAGE_MAKEFILES := $(sort \
	$(wildcard userland/*/Makefile) \
	$(wildcard userland/*/*/Makefile) \
	$(wildcard userland/*/*/*/Makefile))

#include $(USERLAND_PACKAGE_MAKEFILES)

ALL_USER_PROGRAMS := \
	$(foreach program,$(USERLAND_PACKAGES), \
		$(if $(filter y,$(USERLAND_$(program)_SELECTABLE)), \
			$(program)))

user_program_applies := \
	$(or \
		$(filter *,$(USERLAND_$(1)_PLATFORMS)), \
		$(filter $(ZEDBSD_PLATFORM),$(USERLAND_$(1)_PLATFORMS)))

DEFAULT_USER_PROGRAMS := \
	$(foreach program,$(ALL_USER_PROGRAMS),\
		$(if $(filter y,$(USERLAND_$(program)_DEFAULT)),\
			$(if $(call user_program_applies,$(program)), \
				$(program))))

USERLAND_BASIC_PROGRAMS := \
	$(foreach program,$(USERLAND_PACKAGES),\
		$(if $(filter basic,$(USERLAND_$(program)_CLASS)), \
			$(program)))

USERLAND_NETWORK_PROGRAMS := \
	$(foreach program,$(USERLAND_PACKAGES),\
		$(if $(filter network,$(USERLAND_$(program)_CLASS)), \
			$(program)))

USER_PROGRAMS ?= $(DEFAULT_USER_PROGRAMS)

IMAGE_PROFILE ?=
INSTALLER_MENUS := base desktop packages/%

ifeq ($(IMAGE_PROFILE),installer)
INSTALLER_COMMA := ,
INSTALLER_FIRMWARE := \
	$(sort \
		$(foreach row,$(shell sed -n 's/^\(CONFIG_[A-Z0-9_]*\)|\(.*\)$$/\1=\2/p' config/drivers/firmware.list | tr ' ' ','), \
			 $(if $(filter y,$($(firstword $(subst =, ,$(row))))),\
				$(subst $(INSTALLER_COMMA), ,$(lastword $(subst =, ,$(row)))))))
override USER_PROGRAMS := \
	$(foreach program,$(ALL_USER_PROGRAMS),\
		$(if $(filter $(INSTALLER_MENUS), \
			$(USERLAND_$(program)_MENU)), \
			$(if $(call user_program_applies,$(program)),
				$(program)))) \
	$(filter $(INSTALLER_FIRMWARE),$(foreach program,$(ALL_USER_PROGRAMS),\
		$(if $(filter firmware,$(USERLAND_$(program)_MENU)), \
			$(program))))
override USER_PROGRAMS := \
	$(filter-out $(INSTALLER_TEST_OMIT),$(USER_PROGRAMS))
		else ifneq ($(strip $(IMAGE_PROFILE)),)
$(error Unknown IMAGE_PROFILE '$(IMAGE_PROFILE)'; the only profile is installer)
endif
