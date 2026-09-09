# q160 results: installed NVMe acceptance

Completed 2026-09-09, ws019-p005. No physical test or disk write was performed.

The ordinary installed fallback loader boots with the source USB removed,
resolves the generated payload PARTUUID, mounts the current UFS overlay upper
and activates ZEDSWAP2. File contents persist across normal halt/cold restart
and software reboot. Both halts have three consecutive independent QMP samples
with all four CPUs in terminal CLI/HLT. This is not inferred from monitor quit.

| Cell | Evidence |
| --- | --- |
| 1 — preserved installation | q159 public6 cancel/install/rerun and conflict2; GPT/FAT boot/sentinels/NVRAM unchanged across installer operations |
| 2 — installed boot | temp/q160-boot1/result.json and boot0/boot1 guest logs: fallback, explicit PARTUUID, overlay, swap, login and persistence |
| 3 — missing/duplicate config | temp/q160-select1: absent config visibly refuses; duplicate warns and selects first candidate 0. Host discovery rejects ambiguous installation layouts. |
| 4 — refusal/failure boundaries | collect-installed-acceptance.py reruns admission, selection, discovery, destination, transaction and copy models in JIT and interpreter modes. All 12 runs pass in temp/q160-acceptance. Includes capacity, source alias, filesystem/layout, existing conflicts and injected transaction/copy failures. q159 conflict2 additionally proves actual single-byte refusal and restoration. These are model/component failures, not physical power-cut evidence. |
| 5 — other FAT disk | temp/q160-select2: USB auxiliary has distinct GPT identities and matching filenames with an invalid kernel selection. Both auxiliary partitions are OTHER DISK; the installed NVMe boots normally and halts. |
| 6 — generated storage | q159 creates data/swap using current formatters; q160 initializes overlay lower/upper and one 16383-slot ZEDSWAP2 source. p004's source contract copies only loader, kernel and immutable rootfs; live upper/swap are generated rather than copied. |

The reusable collector verifies terminal records, matching immutable hashes and
model results, then records all six cells in temp/q160-acceptance/result.json.
It does not relabel historical partial-run failures as success. All q160 runs
are terminal; no QEMU remains live.

BUG-016 remains: two NVMe controllers exceed the driver's singleton profile.
q160-select1 preserved this independent failure. User-required WS004-p050 will
resolve it in this same goal after the full installer is finished. This p005
acceptance covers its specified auxiliary FAT disk using USB.

WS003 candidate: [candidate record](../../ws003-bringup/phase018-latitude-nvme-install-boot/q160-candidate.md).
Dedicated installation, source selection and native tree-copy UI remain p006,
p007, p027 and p028. p005 completion does not complete WS019.
