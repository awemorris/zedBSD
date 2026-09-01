# Optional firmware packages

Copyright (C) 2026 Awe Morris. SPDX-License-Identifier: Zlib

Device firmware is not part of the zedBSD base system. Each supported firmware
family has one independently selectable directory below this one, downloads
only from an immutable GitHub revision when explicitly selected, verifies all
declared bytes before publishing its cache, and installs the firmware below
`/lib/firmware` together with its applicable license and provenance manifest.

The initial entry is `rtl8822b/`. Future RTL8822C and Intel AX201 support will
own separate `rtl8822c/` and `intelax201/` entries; those entries are added only
with their drivers and reviewed firmware/license identities. This hierarchy is
not a common RTL88 driver layer.
