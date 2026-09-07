# Queue q096: WS025 USB transfer reservations

Date: 2026-09-07
Status: finished
Authorization: user's autonomous WS025 completion instruction covers successive finite Queues.
Timebox: review every 90 active minutes; record facts and resume autonomously.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p009](ws025-io-memory-cache/phase009-usb-reservations/phase.md) | completed | Reserve core staging and HCD request/DMA per storage URB, advertise effective 64 KiB normal BIO limit. Requires completed p001; p005 high RAM integration is available. |

Design: explicit optional paired HCD reserve/release callbacks. URB retains the HCD reservation through its existing references and checked retirement; no reuse or replacement while HCD-owned. xHCI keeps the separate controller 8 KiB reclaim reserve and gives a reserved URB its own request/DMA first. Storage uses two normal 64 KiB bulk reservations plus bounded control resources; unsupported HCDs keep 8 KiB compatibility. Attach/probe and large logical-sector exceptions remain explicit. Core submit commit is already stack-owned and needs no allocation change. Keep BOT serialization and existing cancellation/recovery semantics.

Verify core ownership/rollback, xHCI reuse/64 KiB TRB boundary/retirement, storage max transfer/fallback, multi-device pressure, existing WLAN/HID/USB regressions, three builds and native high-memory USB-root counters. No commit, no aggregate make check; serialize builds/runtime.

Previous: [q095](queue-q095.md). Physical gate remains user-accepted; agent physical runtime is not claimed.

Result: core/HCD/storage reservations and retirement/failure fixtures, USB/HID/RTL8822BU regressions, three supported builds, 4 GiB/16 GiB native baselines PASS. See p009 results.
