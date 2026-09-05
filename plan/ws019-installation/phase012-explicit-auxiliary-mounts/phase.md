# WS019 Phase 012: explicit auxiliary filesystem mounts

Last updated: 2026-09-05

Phase ID: `ws019-p012`

Status: completed in finished q077; original q076 residual retained below

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Authorization and scope

During q076 the user explicitly requested removal of auxiliary FAT mounting.
Remove the boot-time all-partitions `/diskN` auto-mount loop and its now-unused
helpers. Detection still publishes `/dev` partition devices. Preserve explicit
boot/config, root, overlay and swap source mounts, and explicit `mount` commands.
Do not weaken whole-disk EBUSY, change filesystem drivers, or repair the shell.

## Verification / timebox

Review after 30 active minutes. Build maintained amd64/i386 with `make -j16`.
Integrated disposable QEMU must boot with overlay/swap intact, expose auxiliary
partition devices but no auxiliary mounts or `/diskN` mounts, and allow explicit
read-only/read-write FAT mount/unmount. Reboot must retain this policy.

The first two q076 launches found existing auto-mount policy and a test status
observation error respectively, before writer/reload acceptance. Preserve both
as failures. The user's additional boot-policy correction authorizes a bounded
new validation window of at most two launches (120-second boot / 600-second
cell). A disposable guest-only waitpid wrapper observes actual exit codes;
production shell changes are outside scope. If these cells cannot establish
the contract, record uncleared rather than silently extending the budget.

## Result / resume

The auto-mount loop and both now-unused helpers were removed. Final QEMU
login showed auxiliary devices without `/diskN` mounts, overlay root active,
and swap0 active with 16,383 slots. amd64/i386 builds passed. Existing explicit
mount code is unchanged, but the test used unsupported `/mnt/q076` and failed
before exercising it. The corrected root-level `/q076` test and reboot are
pending; the declared launch budget was not extended again. See
[q076 evidence](../tests/q076-results.md). A newly approved validation Queue
can clear this remaining test gate without further boot-policy changes.

## Subsequent residue audit

The user's 2026-09-05 follow-up found unused synthetic-root and inode-only
mount scaffolding after the automatic loop's removal. Its bounded deletion
plan belongs to [ws018-p013](../../ws018-kernel-architecture/phase013-legacy-disk-mount-residue/phase.md),
not a silent expansion of finished q076. This Phase's existing explicit/reboot
gate remains uncleared; a future approved Queue may share runtime evidence.

## Q077 residual acceptance — 2026-09-05

Completed by final-source `q077-resume-02` after the p013/p014/p015 corrections.
Explicit root-level ro/rw/virtual mounts and mutation protection pass; whole-disk
reload returns EBUSY for ro, rw and the actual root disk. Mounted addition
persists and exits 3 without live replacement; reboot discovers p2 with no
automatic auxiliary mounts. MBR round-trip, GPT non-table preservation and
production-input immutability pass. [Q077 evidence](../../ws018-kernel-architecture/tests/q077-results.md)
records commands, hashes and both launches. Q076's earlier failure record is
unchanged; this is new acceptance, not a retroactive PASS for its failed cells.
