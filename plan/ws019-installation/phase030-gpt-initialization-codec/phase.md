# ws019-p030: whole-disk GPT initialization codec

Status: completed, q163; [results](results.md)
Parent: [WS019](../ws.md); prerequisite of [p006](../phase006-whole-disk-provisioning/phase.md)
Authorization: standing autonomous goal; q163 presented before code edits.
Timebox: 60 active minutes.

Existing diskpart loads and edits matching GPT/MBR only. Add a preparation API
to its userland codec which snapshots every metadata region it will overwrite,
constructs primary/backup GPT with 128 128-byte entries and a protective MBR,
and permits existing dp_add validation to construct all partitions before I/O.
Support 512/4096-byte logical sectors, signed-offset bounds, nonzero disk GUID,
minimum GPT geometry, blank and arbitrarily old metadata. This is a table
initialization, not a secure erase of disk contents. Installer MiB alignment,
ESP sizing and filesystem creation remain p006.

Preparation performs no writes. Persist only after every captured region is
unchanged; snapshots detect changes but do not constitute exclusive admission.
Write/flush/readback backup, then primary, then protective MBR. Any failed write
may have partially modified its region. Report started accurately and do not
promise crash atomicity, rollback or successful kernel reload. Free all owned
buffers on preparation error and normal cleanup. Preserve existing edit behavior.

Do not expose a new command on live block devices in this phase. p006 must first
provide fd-lifetime exclusive admission against mounts (including read-only),
swap/loop claims, other writers and media replacement; current BLKREREADPART
preflight does not reserve future use. Noct invokes the eventual existing
diskpart command; no private installer helper command is introduced.

Acceptance: blank/old metadata; 512/4096 geometry and >2-TiB PMBR saturation;
partition bounds/overlap; no writes on invalid input/preimage changes; every
read/write/flush failure including short partial writes; write/readback order;
independent field/CRC inspection; old parser/editor/CLI regressions under host
ASan/UBSan; supported amd64/pcat/pc98 builds. Disposable host images only; native
CLI and kernel admission acceptance follow in p006, not claimed by this phase.
