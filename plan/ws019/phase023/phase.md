# WS019-p023: update Noct for large File.seek

Date: 2026-09-09
Status: completed (q155); [host/native acceptance](results.md)
Parent: [WS019](../ws.md)
Timebox: 120 active minutes

The user reports an upstream File.seek fix lifting the 2-GiB restriction.
The current package pins v2.0.1 / ed621e79139f55d06dd1a474243afbf0ce5efe0a.
Read-only remote discovery finds main at bb239816e20294c073702dc127142f6767375072;
verify the actual change before selecting the immutable new package revision.

Upstream review confirms `bb23981 Fix File.seek`: the INT32_MAX guard now
applies only when LONG_MAX is 32-bit. This lifts the restriction on the LP64
host and amd64 target, not on 32-bit-long platforms. The intervening delta also
fixes installed headers and adds optional CLI module I/O callbacks; include
module loading and target-adapter builds in regression coverage.

The host and target trees are verified archive extractions, not separate git
checkouts. Do not run git pull inside them (git would find the parent zedBSD
repository). Update userland/base/noct/version.mk's commit, archive URL/root/name,
size and SHA-256 consistently, retaining explicit upstream version identity.
Review the upstream delta and rebase the two target-adapter patches if needed.
Preserve existing extracted trees/builds by moving them aside only after their
identity and local changes have been checked; do not overwrite unreviewed work.
Use the canonical acquisition/manifest checks for both host and target builds.

Acceptance: host JIT/interpreter and amd64 native File.seek/read/write/tell tests
at INT32_MAX boundaries and above 4 GiB using disposable sparse files; reject
unrepresentable/invalid offsets without overflow or position corruption. Verify
existing bounded reads and source/admission/transaction Noct tests. Run maintained
builds and native package identity checks. Exercise backup-GPT reads above 2 GiB
through the production Noct interface, replacing the planned dd workaround if
the actual target off_t/API path supports it.

After acceptance update active p004 admission design and affected phase resume
conditions. Preserve historical q152 failure/limitation records, linking their
resolution instead of rewriting history. File.seek availability removes that
specific restriction; it does not complete unrelated discovery, ownership,
confirmation, publication or installed-boot requirements.
