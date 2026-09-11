# q134 implementation progress

2026-09-09. Phase and Queue remain **in-progress**, not installer acceptance.

Implemented existing-command extensions: diskpart machine list/show (including
live parents and on-disk metadata spans), blkid export/tag selection/escaping,
stat formatted lstat identity. Host ordinary+ASan/UBSan passes for the actual
commands, including output errors; diskpart existing parser/editing regressions
pass (1722 parser and 207 CLI checks per variant). Tests/logs:
`/tmp/zedbsd-q134-diskpart-host-final.log`,
`temp/q134-identity-host-final/result.json`.

Noct `identity.noct` now reads records, validates numeric widths/device geometry,
rejects duplicate/incomplete identities, and resolves PARTUUID provenance
uniquely. Host tests pass (`/tmp/zedbsd-q134-noct-identity-final.log`).
Use explicit Long arithmetic (`0L`, maximum with L) for disk fields: default
integer arithmetic is 32-bit. This was a caller implementation correction, not
a missing Noct API. TYPE=vfat requires a BPB check for FAT32.

Target command/identity QEMU cell passed: `temp/q134-command-qemu/result.json`
and `guest.log`, including all pure Noct identity cases plus actual diskpart,
blkid and stat integration. Entire NVMe image and production input hashes are
unchanged. Ordinary amd64 and installer fixture builds passed:
`/tmp/zedbsd-q134-amd64-commands-final.log`,
`/tmp/zedbsd-q134-installer-fixture.log`.

Next: complete Noct table/selection and FAT32
preflight, transaction ownership/confirmation/staging/publishing/recovery, install
production launcher/package, all implementation tests and p004 actual installer
acceptance. p005 remains separate installed-NVMe boot. Do not treat the command
fixture as the installer or publish a nonfunctional zedinst command.

## Remaining implementation questions to resolve locally

Generate/verify all six files before publication while budgeting space for
byte-identical reruns and interruption recovery; do not count only the final
layout if another staging copy is needed. Existing generated data/swap are
compared to canonical fresh outputs, never reformatted in place. The bounded
transaction must preserve source/destination mount ownership and recheck
identity after confirmation. Noct System.pcall is available for cleanup.
The current identity module is preparatory library code; no production zedinst
launcher has been installed yet. q134 and p004 remain in-progress.

## q134 bounded result: uncleared

Selection/GPT/live partition/BPB policy is implemented in Noct. Host and target
selection cases pass (`temp/q134-selection-qemu/guest.log`). Existing cksum now
supports streaming SHA256; ordinary and sanitizer host tests pass
(`temp/q134-sha256-host/result.json`), amd64 build passes
(`/tmp/zedbsd-q134-amd64-sha256.log`). Target hash execution remains unverified.

The same QEMU cell fails the proposed /run canonical-image probe: mkfs reports
EOPNOTSUPP. format-file.c requires ZEDBSD_FILE_FORMAT_RESERVE before writes;
backing-claim.c inode_key explicitly supports only disk-backed FAT regular files.
Do not bypass this reservation or claim the scratch route works. The full
installer transaction, installed launcher, failure/recovery tests and acceptance
remain unimplemented. No functional installer has been installed.

Resume by defining a bounded canonical comparison/generation route supported
by the actual formatters (including exact coverage of initialized and unused
bytes), without reformatting existing destination files, duplicating the full
installation on a nearly-full target, or weakening exclusive mutation. Then
complete the transaction and actual installed-command acceptance. These are
implementation tasks, not an unavailable Noct ioctl or human/hardware gate.
The finite q134 cycle is closed uncleared; continue WS002-p023 now.

q135 follow-up: target cksum SHA256 executed on the copied mkfs artifact and
matched host SHA256, with the same digest retained across USB halt/next boot
and reboot (`ws002/temp/q135-halt-dirty`, relative to plan). This
closes only the target hash-command check; installer canonical generation and
transaction remain uncleared.
