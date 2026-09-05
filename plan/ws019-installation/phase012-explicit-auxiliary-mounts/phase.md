# WS019 Phase 012: explicit auxiliary filesystem mounts

Last updated: 2026-09-05

Phase ID: `ws019-p012`

Status: uncleared in finished q076; auto-mount fix verified, explicit/reboot regression pending

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
