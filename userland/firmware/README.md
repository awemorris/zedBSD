# Optional firmware packages

Copyright (C) 2026 Awe Morris. SPDX-License-Identifier: Zlib

Device firmware is not part of the zedBSD base system. Each supported firmware
family has one independently selectable directory below this one, downloads
only from a reviewed immutable upstream or acquisition-mirror revision when
explicitly selected, verifies all declared bytes before publishing its cache,
and installs the firmware below `/lib/firmware` together with its applicable
license, WHENCE record when required, and provenance manifest.

Every Firmware menu entry is selectable on every CPU architecture. The build
registry supplies the same catalog to the menu, configuration saving and image
packaging; selection follows the attached device rather than the CPU. Selecting
a firmware package installs its data without enabling a hardware driver. The
other way round, menuconfig selects the firmware a driver needs when the
driver is turned on, and drops it when no driver that is on needs it any more
(`config/drivers/firmware.list`), and the firmware a board needs whatever
its drivers are when the board is chosen; the licenses are still accepted
when the image is built.

The entries are `rtl8822b/`, `intelax211/`, `intelbt/`, `i915/`, `brcmbt/`,
`bcm43455/`, `nvidia-gsp/`, `powervr/`, `mali-csf/`, and `amlogic-s7d/`; future
RTL8822C support owns a separate `rtl8822c/` entry. RTL8822B uses its frozen GitHub
acquisition mirror. AX211 uses the official `linux-firmware` tag `20260410`
dereferenced commit and installs the exact `-89.ucode`, PNVM, complete Intel
license, WHENCE, and manifest. `i915/` (package `i915-firmware`) uses the same
official commit and installs every Intel display DMC program the i915 driver
names below `/lib/firmware/i915/`: `adlp_dmc.bin` (ADL-P, v2.20, and its
fallback `adlp_dmc_ver2_16.bin`, v2.16), `tgl_dmc_ver2_12.bin` (TGL, v2.12),
`rkl_dmc_ver2_03.bin` (RKL, v2.03), `adls_dmc_ver2_01.bin` (ADL-S and RPL-S,
v2.01) and `icl_dmc_ver1_09.bin` (ICL, v1.09); the ADL-P and TGL ones are
byte-identical to the blobs the driver was brought up with. It installs them
together with the complete `LICENSE.i915`, WHENCE, and
manifest. `LICENSE.i915` permits redistribution of the unmodified binaries
only, with its notice, so these bytes belong to this package and not to the
kernel. Its focused check is
`plan/ws233/tests/run-i915-firmware-package-test.sh`. `intelbt/` (package
`intelbt-firmware`) uses the same official commit and installs the whole
Solar block of WHENCE (`BT_Solar_REL82122_23.50.26053.82122`): the three
`intel/ibt-*-0041.{sfi,ddc}` pairs that bluetoothd loads into an AX211's
Bluetooth controller, and, as copies of the same verified bytes, the eight
names WHENCE links to them, because the image builder installs files only.
`LICENCE.ibt_firmware` permits redistribution of the unmodified binaries
with its notice and forbids reverse engineering; bluetoothd reads only the
HCI command frames a `.sfi` file is made of. Its focused check is
`plan/ws143/tests/intelbt-firmware-package-test.sh`. `brcmbt/` (package
`brcmbt-firmware`) installs `brcm/BCM4345C0.hcd`, the patch file bluetoothd
loads into the Bluetooth part of the Raspberry Pi 4's BCM43455 over its
UART, from Raspberry Pi's `RPi-Distro/bluez-firmware` commit
`adb4d55c8f193ff16ece74dd72fd6e8801632103` (1.2-9+rpt4). That tree carries
the file under the Cypress Wireless Connectivity Devices EULA but keeps the
licence only as a saved HTML page, so the package installs the plain
`LICENCE.cypress` of the official linux-firmware commit above (the blob the
saved page shows), the tree's `debian/copyright`, and the manifest. The EULA permits
redistribution of the object code only, solely for use with Cypress
integrated circuits, and forbids modification and reverse engineering;
the Raspberry Pi 4 image configuration is the only one that selects it.
Its focused check is `plan/ws215/tests/brcmbt-firmware-package-test.sh`.
`bcm43455/` (package `bcm43455-firmware`) installs the three files the
Raspberry Pi 4's on-board WiFi driver writes into the BCM43455:
`cypress/cyfmac43455-sdio-minimal.bin` (firmware 7.45.241, the variant
with the firmware supplicant), `cypress/cyfmac43455-sdio.clm_blob`
(Raspberry Pi's regulatory data, used with the world locale), and
`brcm/brcmfmac43455-sdio.txt` (the Pi 4 NVRAM), all from Raspberry Pi's
`RPi-Distro/firmware-nonfree` commit
`3bab0f823f5b53150b76aab77093adef6655b920` (1:20260519-1~bpo13+1+rpt1),
whose `debian/copyright` places them under the Cypress licence. The
same NVRAM values are published in linux-firmware under GPL-2.0+, so the
package installs the plain `LICENCE.cypress` and `GPL-2` of the official
linux-firmware commit above together with the full `debian/copyright` and
the manifest. Only the Raspberry Pi 4 image configuration selects it.
Its focused check is `plan/ws206/tests/bcm43455-firmware-package-test.sh`.
`nvidia-gsp/` (package `nvidia-gsp-firmware`) uses the same official
linux-firmware commit and installs NVIDIA's GSP-RM firmware 570.144 for
Turing, Ampere and Ada, which the nvrtx driver boots
(plan/ws037/nvrtx-design.md section 9, plan/ws242/phase001/phase.md):
below `nvidia/tu102/` the Turing GSP-RM image `gsp/gsp-570.144.bin`,
`gsp/bootloader-570.144.bin`, `gsp/booter_load-570.144.bin`,
`gsp/booter_unload-570.144.bin` and `acr/bl.bin`; below `nvidia/tu116/`
the GTX 16 booters; below `nvidia/ga102/` the Ampere and Ada GSP-RM image
with the Ampere bootloader and booters; below `nvidia/ad102/` the Ada
bootloader and booters; together with the complete `LICENCE.nvidia`,
WHENCE, and manifest.  WHENCE links the other chips' names (tu104, tu106,
tu117, ga103 to ga107, ad103 to ad107) and the shared images to these; each
file is installed once and the driver maps every chip to the directories
it reads.  `LICENCE.nvidia` permits redistribution of the unmodified
binaries solely for use on an operating system under an OSI-approved open
source license, with a copy of the license, and forbids reverse engineering
and translation; nvrtx reads only the container headers needed to place the
images.
`powervr/` (package `powervr-firmware`) installs Imagination's PowerVR Rogue
firmware 1.1.OS@6976702 for the BXE-4-32 (BVNC 36.50.54.182) of the
VisionFive 2's StarFive JH7110, which the powervr driver loads
(`CONFIG_DRIVER_POWERVR`, plan/ws269/design.md sections 3.3 and 4):
`powervr/rogue_36.50.54.182_v1.fw`, together with the complete
`LICENSE.powervr`, WHENCE, and manifest. The file is not in kernel.org's
linux-firmware; the upstream is Imagination's fork
`gitlab.freedesktop.org/imagination/linux-firmware`, branch `powervr`, pinned
to commit `8a58f81883f7be458daa34e418cc4079f995b279`, and the zedBSD firmware
repository keeps the same bytes below `powervr/`. `LICENSE.powervr` permits
redistribution of the unmodified binary with its notice and disclaimer, only
for use with products licensed by Imagination. **It forbids reverse
engineering, decompilation and disassembly: do not disassemble this firmware.**
The driver is developed from the firmware's trace buffer, its interface's
replies and register values alone. The driver is not built yet; when its catalog
row is added (plan/ws269, p004, on by default for the VisionFive 2), its
`config/drivers/firmware.list` row makes selecting it select the package.
Until then the package is chosen in the Firmware menu. The license is accepted
when the image is built. The kernel reads the file from the root file system, which is built from the
same tree for every root image.
`mali-csf/` (package `mali-csf-firmware`) uses the official linux-firmware
commit above and installs `arm/mali/arch10.8/mali_csffw.bin`, the program of the
Command Stream Frontend of the ODROID-C5's Mali-G310, under the arch 10.8,
10.10 and 10.12 names linux-firmware gives the same bytes, with the complete
`LICENCE.mali_csffw`, WHENCE and the manifest; the mali driver
(`CONFIG_DRIVER_MALI_GPU`) selects it. `amlogic-s7d/` (package
`amlogic-s7d-bootloader`) is the ODROID-C5's bootloader: Amlogic's signed boot
stages and Hardkernel's U-Boot, in sectors 1 to 6538 of Hardkernel's official
ODROID-C5 image `ubuntu-22.04-gnome-desktop-odroidc5-20260610-bsp-boot.img.xz`.
A build cannot fetch that image from Hardkernel, so it is kept unmodified,
with Amlogic's `LICENSE.amlogic`, in the firmware repository below, which is
the package's only network source (without it the two files are placed by
hand, as the notice says). The package checks the image's size and SHA-256,
decompresses it, cuts the sectors and checks their size and SHA-256 too.
Choosing the ODROID-C5 board selects it; the image build writes the area from
the SD card's sector 1 and the root file system gets only `LICENSE.amlogic`,
the notice and the manifest. Amlogic's license allows redistribution only
with its written permission, which Hardkernel holds; the files are
redistributed unmodified and only for use on the ODROID-C5.
Every entry is default-off, and ordinary builds perform no firmware fetch.

Every file a package fetches is also kept, byte for byte and with its license
texts, in the GitHub repository `awemorris/zedBSD-firmware`, one directory per
package (`rpi4-boot/` holds the Raspberry Pi 4 boot firmware of
`platform/arm64/firmware.mk`). `zedbsd-firmware.mk` names the repository's
commit; while that is set, a package fetches each file from the repository
first and from its upstream only when that fails, and checks the same size and
SHA-256 either way. While it is empty, the packages fetch from their upstream
alone.

The first time a package's firmware is fetched, `license-agree.sh` shows what
the firmware is and its copyright (the package's `*.notice` file), the whole
license text, and asks `yes/no`. An accepted text is recorded beside the cache
(`build/sources/firmware/PACKAGE/license-accepted`) and is not asked about
again; a changed text is. Any other answer, or none, stops the build without
fetching. A build goal asks while make reads the makefiles, so `make -j` is
safe; an unattended build answers first with `yes | make firmware-licenses`,
as CI does. The build menu (`make menuconfig`, Build boot image) asks in a
license screen of its own before it builds: `make firmware-licenses-pending`
lists what would be asked without asking, and `license-agree.sh` in its
accept mode writes the record.
This hierarchy is package organization, not a common hardware-driver layer.
