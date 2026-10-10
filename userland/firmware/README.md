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
a firmware package installs its data without enabling a hardware driver.

The entries are `rtl8822b/`, `intelax211/`, `intelbt/`, and `i915/`; future
RTL8822C support owns a separate `rtl8822c/` entry. RTL8822B uses its frozen GitHub
acquisition mirror. AX211 uses the official `linux-firmware` tag `20260410`
dereferenced commit and installs the exact `-89.ucode`, PNVM, complete Intel
license, WHENCE, and manifest. `i915/` (package `i915-firmware`) uses the same
official commit and installs the Intel display DMC firmware
`i915/adlp_dmc.bin` (ADL-P, v2.20) and `i915/tgl_dmc_ver2_12.bin` (TGL, v2.12)
below `/lib/firmware/i915/`, byte-identical to the blobs the i915 driver was
brought up with, together with the complete `LICENSE.i915`, WHENCE, and
manifest. `LICENSE.i915` permits redistribution of the unmodified binaries
only, with its notice, so these bytes belong to this package and not to the
kernel. Its focused check is
`plan/ws031/tests/run-i915-firmware-package-test.sh`. `intelbt/` (package
`intelbt-firmware`) uses the same official commit and installs the whole
Solar block of WHENCE (`BT_Solar_REL82122_23.50.26053.82122`): the three
`intel/ibt-*-0041.{sfi,ddc}` pairs that bluetoothd loads into an AX211's
Bluetooth controller, and, as copies of the same verified bytes, the eight
names WHENCE links to them, because the image builder installs files only.
`LICENCE.ibt_firmware` permits redistribution of the unmodified binaries
with its notice and forbids reverse engineering; bluetoothd reads only the
HCI command frames a `.sfi` file is made of. Its focused check is
`plan/ws143/tests/intelbt-firmware-package-test.sh`. Every entry is
default-off, and ordinary builds perform no firmware fetch. This hierarchy
is package organization, not a common hardware-driver layer.
