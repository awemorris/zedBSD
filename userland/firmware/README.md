# Optional firmware packages

Copyright (C) 2026 Awe Morris. SPDX-License-Identifier: Zlib

Device firmware is not part of the zedBSD base system. Each supported firmware
family has one independently selectable directory below this one, downloads
only from a reviewed immutable upstream or acquisition-mirror revision when
explicitly selected, verifies all declared bytes before publishing its cache,
and installs the firmware below `/lib/firmware` together with its applicable
license, WHENCE record when required, and provenance manifest.

The entries are `rtl8822b/` and `intelax211/`; future RTL8822C support owns a
separate `rtl8822c/` entry. RTL8822B uses its frozen GitHub acquisition mirror.
AX211 uses the official `linux-firmware` tag `20260410` dereferenced commit and
installs the exact `-89.ucode`, PNVM, complete Intel license, WHENCE, and
manifest. Every entry is default-off, and ordinary builds perform no firmware
fetch. This hierarchy is package organization, not a common hardware-driver
layer.
