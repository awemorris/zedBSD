# q160 progress

Status: finished, 2026-09-09. [Authoritative results](q160-results.md) complete
p005. The live observations below are historical; all sessions are terminal.

Entry evidence is q159 public6 cancel/install/rerun plus conflict2 terminal PASS.
The accepted target is preserved; all boot-time writes use a disposable copy.
run-installed-boot-qemu.py is syntax-checked and running as session 59952:
temp/q160-boot1, /tmp/q160-boot1.log. It supplies only the installed NVMe and OVMF
variables, no source USB/IDE or injected rootfs. It checks generated PARTUUID,
overlay/swap initialization, ordinary write/sync, QMP-confirmed terminal CLI/HLT,
cold restart and software reboot persistence. This first runner does not yet
cover absent/duplicate config or auxiliary lookalike selection; those cells
remain required before p005 completion.

Session 59952 is now terminal exit 0. temp/q160-boot1/result.json reports PASS
installed boot/persistence: initial boot/write/halt, cold boot and software
reboot persistence, and a second normal halt. Both halts have three consecutive
QMP samples with all four CPUs in terminal CLI/HLT. The source/production hashes
remain unchanged. This is not yet full p005 acceptance.

The selection runner is now live as session 13781: temp/q160-select1,
/tmp/q160-select1.log. It removes the config in one disposable copy, adds a
distinct first-candidate config in another, and attaches an auxiliary NVMe
before the installed controller in the third. The auxiliary has independent
GPT disk/partition identities, matching file names and a deliberately unusable
kernel selection, so ignoring it must follow physical boot-disk identity.

Selection1: absent and duplicate cases passed. With two NVMe controllers the
loader selects the correct installed payload, but the kernel logs `additional
controller rejected by initial profile` and accepts only the auxiliary first
controller. The generated boot0 PARTUUID consequently cannot resolve; VFS
enters idle. src/drivers/pci/pci-nvme.c has a single nvme_primary registry. This
is BUG-016, deferred to bounded WS004 multi-controller work, not a loader
selection failure. Preserve the failed cell. The original p005 requirement is
an auxiliary FAT disk, so its remaining acceptance will use a USB auxiliary
with distinct GPT identities and an intentionally unusable config. It is not
the installation source; no source disk is reintroduced.

Selection1/session 13781 is terminal exit 1, preserving its successful absent/
duplicate cells and the two-NVMe limitation. The auxiliary-only continuation
temp/q160-select2 (session 13104, /tmp/q160-select2.log) is terminal exit 0 and
reports PASS installed selection. Its loader explicitly reports both auxiliary
partitions as OTHER DISK, selects installed payload 1, and boots overlay/swap
through login and normal halt. Source/production hashes remain unchanged.
No q160 QEMU remains live. Required runtime cells now have evidence; remaining
work is the reusable consolidated per-cell acceptance record, reconciliation
of cell 4 with p004's actual fault fixtures, and the WS003 candidate record.
p005 remains in-progress until those completion conditions are checked.
