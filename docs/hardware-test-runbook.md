# zedBSD hardware regression runbook

The package image is the only image that may be tested. Before writing media,
compare its SHA-256 with `manifest.json`, identify the target device by model,
serial number, and capacity, and ensure it is expendable test media. Never use
the system disk.

## Required evidence

Record machine ID, operator, UTC time, firmware/BIOS version, CPU count, RAM,
storage controller and medium, display, and network adapter. Capture the serial
or debug-port log. If that is impossible, retain both a photograph/video with
the machine and run IDs visible and a log written by zedBSD to the test medium.

Perform three genuine cold boots (power removed between runs), not three warm
reboots. For every required case in `manifest.json`, record `PASS`, `FAIL`,
`SKIP`, or `BLOCKED` and an evidence filename. `SKIP` requires a reason such as
absent optional hardware. A QEMU result is never hardware evidence.

PC-98 runs must record BIOS disk geometry and confirm H=8/S=17 media discovery,
keyboard input, timer/signal behavior, filesystem write+fsync+remount, and the
graphics-to-console transition. Record additional drives and LGY-98/Cirrus
presence even when absent.

amd64 UEFI runs must record firmware vendor/version, Secure Boot and CSM state,
APIC CPU topology, all-CPU online marker, dynamic linker marker, filesystem
write+remount, and SMP stress. Record the actual storage path (IDE/SATA/NVMe);
unsupported hardware is `BLOCKED`, not `PASS`.

## Result validation

Create `result.json` with schema 1, `machine_class`, `machine_id`, `operator`,
`image_sha256`, at least three `{ "result": "PASS", "evidence": "..." }`
objects in `cold_boots`, and one `{ "result": ..., "evidence": ... }` object
per required case in `cases`. Validate it with:

```sh
python3 scripts/hardware-test-manifest.py validate \
  --manifest manifest.json --result result.json
```

Preserve the manifest, result, logs, photos, tool output, and image hash
together. A failed run must keep its evidence and receive a new run ID after a
fix; do not overwrite it.
