# q159 progress

Status: finished; [authoritative results](q159-results.md). All sessions below
are historical and terminal. p004, p025 and p026 completed; q160 executes p005.

## Current update: public runtime prerequisites

The earlier rebuild session 32738 finished successfully; amd64/pcat/pc98 and
standalone/package verification passed. The earlier `/usr/bin/noct` decision is
superseded by the user's explicit `/bin/noct` instruction. Actual package and
BIOS-image producer/checker paths and the launcher have been updated.

Public4 ended before installation: its truncate fixture did not create the
nonexistent file. This is preserved in `temp/q159-public4/result.json`.
User-authorized [p025](../phase025/phase.md) now fixes
the shell status and implements null/zero rather than retaining test workarounds.
Its focused host tests, maintained builds and 16 native checks passed; see
[p025 results](../phase025/results.md). The public installer runner now samples `$?` on a separate input line
and uses real `/dev/null` and `/dev/zero`.

Full public integration ended with failure in `temp/q159-public5` (session 26799,
exit 1, `/tmp/q159-public5.log`). Usage/noninteractive/source-alias refusal and
cancellation passed, including whole-disk cancellation hash equality. Install
failed with `/bin/cp status=124`; all unpublished files were removed. GPT, FAT
BPBs, sentinels, NVRAM and production hashes remain unchanged. No public5
process remains live. Command errors now include operands. The isolated
`temp/q159-copy` attempt stopped on a test mistake: unqualified cp selected the
shell's minimal builtin, not /bin/cp; it is not evidence about /bin/cp throughput.

The user then explicitly selected Noct-managed image copies. [p026](../phase026/phase.md)
implements bounded reads/writes and progress, retaining cp for attribute-only
empty-file creation and future native tree copies. Host injected tests pass 73
cases in both modes and real binary copy/failure checks pass. The production
image contains the new module. `temp/q159-public6` is live as session 9572
(`/tmp/q159-public6.log`): cancellation passed and loader/kernel copying completed;
rootfs byte progress reached 100%, all six managed files were published and
`Installation complete.` returned status 0. The runner is now terminal: cancel,
install and rerun passed; the conflict fixture failed before corruption because
unqualified cp selected the shell builtin. All protected hashes match. p026 is
completed with [results](../phase026/results.md). The corrected
conflict-only continuation uses a disposable copy of the accepted target:
`temp/q159-conflict1`, session 53670, `/tmp/q159-conflict1.log`, currently live.
Preserve the original public6 terminal FAIL record and its successful cases.
p004 and p005 remain incomplete. p006/p007 decisions are now released, and
p027 source selection is planned from the user's subsequent requirements.

The remaining text records the earlier q159 work; its live-session and path
statements below are historical, not the current execution state.

Applied the privately tested no-extra-capacity rerun correction to production:
stage directories only for missing files on that partition; refresh presence
after confirmation; no seed for a complete destination. Host JIT/interpreter
destination cases pass, including complete-destination zero space/no seed.

Registered optional amd64 zedinst requiring base/noct, /bin/zedinst launcher and
15 modules under /lib/zedinst. Enabled only the maintained amd64 CI profile;
the user's config.mk is unchanged. The public runtime runner now boots the
ordinary production rootfs and invokes zedinst. It observes exit status through
the existing shell's echo/$? rather than adding the private storage-exit binary.
Added the prepared single-byte conflict/refusal/restore acceptance.

First production build failed because package-class executables need explicit
ZEDBSD_PACKAGE_INPUTS/FILES registration, unlike basic C commands. Corrected that
registration and corrected the launcher to the verified /usr/bin/noct location.
The failed log is `/tmp/zedbsd-q159-amd64.log`. Rebuild is live as session 32738,
log `/tmp/zedbsd-q159-amd64-2.log`. Do not run QEMU before successful build.

Remaining: three maintained builds, manifest/permissions/standalone package
verification, public launcher runtime/cancel/install/rerun/conflict and remaining
p004 acceptance reconciliation. q158 is retained as accepted private integration;
p004 and the overall Priority goal are not complete.
