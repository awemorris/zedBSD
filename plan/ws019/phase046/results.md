# q179 result

Completed 2026-09-10. `nativeplan.noct` returns checked ESP/UFS geometry,
the exact existing diskpart argv, measured capacity requirements and native
PARTUUID boot configuration. It is packaged but not yet exposed as an installer
mode. Geometry planning does not authorize writes or replace codec admission.

`installer-nativeplan.noct`: host Noct JIT and `-j0` interpreter both pass
6 valid layouts and 29 refusals. Covers 4-GiB/4-TiB/5-TiB disks, minimum ESP,
larger boot payloads, exact fit and one-sector shortage, source aliases,
read-only/partition/unsupported sector geometry, arithmetic overflow, FAT32
sector ceiling, GUID duplication and invalid measurements.
amd64 image build `/tmp/zedbsd-q179-amd64.log` exits 0; targeted diff check clean.

Remaining integration requirements: create unique GUIDs and check inventory;
check final formatter geometry before destruction; bind NO/YES to reobserved
identity; run reserved public commands; copy/verify/publish; activate native
root-file swap after mounting root. Existing swap0 boot preparation accepts
FAT boot-slot files and raw partitions only, so no unsupported swap0=/swapfile
is emitted. p006/p007 and p029 remain open.
