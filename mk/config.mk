# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Configurations
#

################################################
# Platform
################################################

PLATFORM                          ?=
ARCHITECTURE                      ?=
BOARD                             ?=
VARIANT                           ?=

################################################
# Boot
################################################

# [Development]
#  - Description: Add development headers to the rootfs.
CONFIG_ROOTFS_DEVELOPMENT         ?= n

# [Boot Splash]
#  - Description: Show boot splash screen.
CONFIG_BOOT_SPLASH                ?= n

# [Quiet Boot]
#  - Description: Suppress boot messages.
CONFIG_QUIET_BOOT                 ?= n

# [Graphical Login]
#  - Description: Start desktop automatically.
CONFIG_GRAPHICAL_LOGIN            ?= n

################################################
# 0. Bus
################################################

########################
# Framework
########################

# [PCI Core]
#  - Description: Enable PCI core.
CONFIG_DRIVER_PCI                 ?= n

# [USB Core]
#  - Description: Enable USB core.
CONFIG_DRIVER_USB                 ?= n

################################################
# Storage
################################################

########################
# Drivers
########################

# [NVMe driver]
#  - Description: Enable NVMe driver.
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_NVME            ?= n

# [SATA AHCI driver]
#  - Description: Enable SATA AHCI driver.
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_AHCI            ?= n

# [Intel VMD driver]
#  - Description: Enable Intel VMD SATA driver.
#  - Depends: CONFIG_DRIVER_PCI
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
# Network
################################################

########################
# Netowrk Stack
########################

# [Network Core]
CONFIG_KERN_NET                   ?= n

# [IPv4 Stack]
#  - Depends: CONFIG_KERN_NET
CONFIG_KERN_NET_IPV4              ?= n

# [IPv6 Stack]
#  - Depends on CONFIG_KERN_NET_IPV4
CONFIG_KERN_NET_IPV6              ?= n

# [Ethernet Stack]
#  - Depends: CONFIG_KERN_NET
CONFIG_KERN_NET_ETHERNET          ?= n

# [WiFi Stack]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
CONFIG_KERN_NET_WIFI              ?= n

########################
# Ethernet Drivers
########################

# [USB CDC driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
#  - Depends: CONFIG_DRIVER_USB
CONFIG_DRIVER_USB_CDC             ?= n

# [USB CDC NCM Ethernet driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_USB_CDC
CONFIG_DRIVER_USB_CDC_NCM         ?= n

# [USB CDC NCM Ethernet driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_USB_CDC
CONFIG_DRIVER_USB_CDC_ECM         ?= n

# [Realtek RTL8125 PCIe Ethernet driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_RTL8125         ?= n

# [Realtek RTL8156/8157 USB Ethernet vendor mode driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
#  - Depends: CONFIG_DRIVER_USB
CONFIG_DRIVER_USB_RTL815X         ?= n

# [RPi4 Ethernet driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
CONFIG_DRIVER_RPI4_ETHERNET       ?= n

# [RPi5 Ethernet driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
CONFIG_DRIVER_RPI5_ETHERNET       ?= n

# [Vision Five 2 Ethernet driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
CONFIG_DRIVER_VF2_ETHERNET        ?= n

# [PC/AT NE2000-compatible ISA Ethernet driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
CONFIG_DRIVER_NE2000              ?= n

# [PC-98 LGY98 C-BUS Ethernet driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
CONFIG_DRIVER_LGY98               ?= n

########################
# WiFi Drivers
########################

# [Intel AX211 PCIe WiFi driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
#  - Depends: CONFIG_KERN_NET_WIFI
CONFIG_DRIVER_PCI_INTEL_AX211     ?= n

# [Realtek RTL8822BU USB WiFi driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
#  - Depends: CONFIG_KERN_NET_WIFI
#  - Depends: CONFIG_DRIVER_USB
CONFIG_DRIVER_USB_RTL8822BU       ?= n

# [RPi4 WiFi driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
#  - Depends: CONFIG_KERN_NET_WIFI
CONFIG_DRIVER_RPI4_WIFI           ?= n

# [RPi5 WiFi driver]
#  - Depends: CONFIG_KERN_NET
#  - Depends: CONFIG_KERN_NET_ETHERNET
#  - Depends: CONFIG_KERN_NET_WIFI
CONFIG_DRIVER_RPI5_WIFI           ?= n

################################################
# Graphics
################################################

########################
# Framework
########################

# [GPU Core]
CONFIG_DRIVER_GPU                 ?= n

# [Legacy 2D Graphics Core]
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
#  - Depends: CONFIG_DRIVER_GPU
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_I915            ?= n

# [NVIDIA RTX GPU Driver]
#  - Depends: CONFIG_DRIVER_GPU
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_NVRTX           ?= n

# [AMD RDNA GPU driver]
#  - Depends: CONFIG_DRIVER_GPU
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_AMDRDNA         ?= n

# [RPi4 VideoCore VI GPU driver]
#  - Depends: CONFIG_DRIVER_GPU
CONFIG_DRIVER_RPI4_GPU            ?= n

# [RPi5 VideoCore VII GPU driver]
#  - Depends: CONFIG_DRIVER_GPU
CONFIG_DRIVER_RPI5_GPU            ?= n

# [Vision Five 2 Imagination PowerVR GPU and WAVE511 Video Codex driver]
#  - Depends: CONFIG_DRIVER_GPU
CONFIG_DRIVER_VF2_GPU_VPU         ?= n

# [GHCI GPU driver]
#  - Depends: CONFIG_DRIVER_GPU
#  - Depends: CONFIG_DRIVER_CI
CONFIG_DRIVER_PCI_GHCI            ?= n

# [Venus GPU Driver]
#  - Depends: CONFIG_DRIVER_GPU
#  - Depends: CONFIG_DRIVER_CI
CONFIG_DRIVER_PCI_VENUS           ?= n

########################
# Legacy 2D Drivers
########################

# [Boot Linear Framebuffer driver]
#  - Depends: CONFIG_DRIVER_GRAPHICS
CONFIG_DRIVER_GRAPHICS_BOOTLFB    ?= n

# [PC/AT VGA driver]
#  - Depends: CONFIG_DRIVER_GRAPHICS
CONFIG_DRIVER_GRAPHICS_PCAT       ?= n

# [PC-98 GDC driver]
#  - Depends: CONFIG_DRIVER_GRAPHICS
CONFIG_DRIVER_GRAPHICS_PC98       ?= n

################################################
# Audio
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
#  - Depends: CONFIG_DRIVER_AUDIO
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_HDA             ?= n

# [RPi4 PWM Audio driver]
#  - Depends: CONFIG_DRIVER_AUDIO
CONFIG_DRIVER_RPI4_AUDIO          ?= n

# [RPi5 RP1 Audio driver]
#  - Depends: CONFIG_DRIVER_AUDIO
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_RPI5_AUDIO          ?= n

# [Vision Five 2 Audio driver]
#  - Depends: CONFIG_DRIVER_AUDIO
CONFIG_DRIVER_VF2_AUDIO           ?= n

################################################
# Input Device
################################################

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
# USB
################################################

########################
# Framework
########################

# [USB Core]
CONFIG_DRIVER_USB                 ?= n

########################
# Controller Drivers
########################

# [USB 1.1 OHCI driver]
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_OHCI            ?= n

# [USB 1.1 UHCI driver]
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_UHCI            ?= n

# [USB 2.0 EHCI driver]
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_EHCI            ?= n

# [USB 3.0 XHCI driver]
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_XHCI            ?= n

# [USB-C USCI driver]
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_ACPI
CONFIG_DRIVER_TYPEC               ?= n

########################
# Class Drivers
########################

# [USB Hub driver]
#  - Depends: CONFIG_DRIVER_USB
CONFIG_DRIVER_USB_HUB             ?= n

# [USB Human Interface Device (HID) driver]
# - Depends: CONFIG_DRIVER_USB
CONFIG_DRIVER_USB_HID             ?= n

# [USB Mass Storage Device (MSD) driver]
#  - Depends: CONFIG_DRIVER_USB
CONFIG_DRIVER_USB_STORAGE         ?= n

# [USB Attached SCSI Protocol (UASP) driver]
#  - Depends: CONFIG_DRIVER_USB
CONFIG_DRIVER_USB_UASP            ?= n

# [USB Chip Card Interface Device (CCID) driver]
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_SMARTCARD
CONFIG_DRIVER_USB_CCID            ?= n

# [USB Bluetooth driver]
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_BT
CONFIG_DRIVER_USB_BT              ?= n

# [USB Video Class (UVC) driver]
#  - Depends: CONFIG_DRIVER_USB
#  - Depends: CONFIG_DRIVER_CAMERA
CONFIG_DRIVER_USB_UVC             ?= n

################################################
# Bluetooth
################################################

########################
# Framework
########################

# [Generic Bluetooth HCI]
CONFIG_DRIVER_BT                  ?= n

# [Bluetooth Input Bridge] (/dev/input/bridge)
# - Depends: CONFIG_DRIVER_BT
# - Depends: CONFIG_DRIVER_HID
CONFIG_DRIVER_INPUT_BRIDGE        ?= n

########################
# Drivers
########################

# [RPi4 Bluetooth driver]
#  - Depends: CONFIG_DRIVER_BT
#  - Note: CONFIG_RPI4_SERIAL_CONSOLE=y keeps PL011 for the console and has no Bluetooth.
CONFIG_DRIVER_RPI4_BT             ?= n

# [RPi5 Onboard Bluetooth]
#  - CONFIG_DRIVER_BT
CONFIG_DRIVER_RPI5_BT             ?= n

################################################
# Thermal Management
################################################

########################
# Framework
########################

# [Generic CPU performance monitoring driver] (/dev/thermal)
CONFIG_KERN_CPU_PERF              ?= n

# [x86 Power Control driver] (/dev/thermal)
#  - Depends: CONFIG_KERN_CPU_PERF
CONFIG_DRIVER_X86_POWER_CONTROL   ?= n

# [RPi5 processor temperature] (sysctl hw.thermal)
#  - TODO: need to port to /dev/thermal
CONFIG_DRIVER_RPI5_FAN            ?= n

################################################
# Camera and Capture
################################################

########################
# Framework
########################

# [Camera Core (/dev/camN)]
CONFIG_DRIVER_CAMERA              ?= n

################################################
# Smart Card
################################################

########################
# Framework
########################

# [Smart Card Core]
CONFIG_DRIVER_SMARTCARD           ?= n

################################################
# ACPI
################################################

########################
# Framework
########################

# [ACPI Core]
CONFIG_DRIVER_ACPI                ?= n

################################################
# I2C/SPI/GPIO
################################################

########################
# Framework
########################

# [I2C Generic Driver]
CONFIG_DRIVER_I2C                 ?= n

# [GPIO Generic Driver]
#  - Note: Not implemented yet, only Intel GPIO exists.
CONFIG_DRIVER_GPIO                  ?= n

########################
# Drivers
########################

# [Intel LPSS I2C / AMD I2C driver] (Synopsis DesignWare I2C)
#  - Depends: CONFIG_DRIVER_I2C
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_LPSS_I2C        ?= n

# [Intel LPSS GPIO driver]
#  - Depends: CONFIG_DRIVER_PCI
CONFIG_DRIVER_PCI_LPSS_GPIO       ?= n

# [AMD GPIO driver]
#  - Depends: CONFIG_DRIVER_PCI
#  - Note: Not implemented yet.
CONFIG_DRIVER_PCI_AMD_GPIO        ?= n

# [I2C-HID driver]
#  - Depends: CONFIG_DRIVER_I2C
CONFIG_DRIVER_I2C_HID             ?= n

################################################
# Debug Options
################################################

# [PC/AT: Console to Serial Mirror]
CONFIG_PCAT_SERIAL_MIRROR         ?= n

# [RPi4: Serial Console] (PL011 on GPIO 14 and 15)
CONFIG_RPI4_SERIAL_CONSOLE        ?= n

# [RPi5: Serial Console] (PL011 on 3-pin debug connector)
CONFIG_RPI5_SERIAL_CONSOLE        ?= n

################################################
# Tests
################################################

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

################################################
# Userland Programs
################################################

CONFIG_USERLAND_PROGRAMS          ?=
