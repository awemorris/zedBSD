# q163 / ws019-p030 results

Completed 2026-09-09. This completes the userland GPT initialization codec,
not whole-disk installer admission, formatting or native installation.

`dp_initialize_gpt` snapshots the protective-MBR sector, both header sectors
and both 16-KiB entry arrays. It prepares a new empty GPT using a caller-supplied
nonzero GUID. Existing dp_add bounds/overlap/GUID/name checks populate the plan
before any write. The new MBR has one saturated protective record and no old
boot code. This replaces metadata, not all user-data sectors or old filesystem
signatures. The installer must subsequently format its partitions.

`dp_write` rechecks all five old regions before its first write; writes and
flush-verifies backup then primary GPT; finally writes and flush-verifies the
protective MBR. Existing edits retain their prior behavior. Any write attempt
sets started, including partial failures. No rollback/crash-atomicity claim.
Preparation error and dp_free release original and replacement buffers.

| Acceptance | Evidence |
| --- | --- |
| Blank and arbitrary old media, 512/4096 sectors | Production initialization, dp_add, write and reload through parser pass |
| Minimal geometry and large media | Exactly one usable LBA passes; one sector smaller refuses; >2-TiB PMBR count saturates; signed-byte-offset overflow refuses |
| No early writes | Invalid GUID/geometry and changes to each of five snapshots refuse with zero writes |
| Backend failures | Each of 23 preparation/preflight/write/flush/readback operations faulted for both sector sizes; partial writes return EIO with started; preparation/preflight failures remain unwritten |
| Silent corruption | Each of five writes can silently corrupt its bytes; subsequent readback refuses |
| Write bounds/order | Only intended front/rear metadata regions change; backup before primary before PMBR; three flushes |
| Independent decoding | Python struct/UUID/zlib reads actual emitted head/tail windows and validates both headers, GUIDs, entries, CRCs, PMBR and zero padding |
| Existing behavior | Old GPT/MBR parser/editor tests and production CLI suite pass |
| Sanitizers | ASan, UBSan and leak detection pass outside sandbox |
| Target compilation | amd64, pcat and pc98 disk-image builds pass using CI configs and make -j16 |

Host runner: `tests/run-diskpart-table-test.sh`; inspector:
`tests/gpt-init-inspect.py`. Log `/tmp/zedbsd-q163-host.log`: ordinary and
sanitized modes each report 188396 assertions (including per-byte sentinels,
not 188396 scenarios), independent inspection PASS and 207 CLI assertions PASS.
Initial sandbox attempt completed ordinary tests but LeakSanitizer failed on
its ptrace restriction; the authorized unsandboxed rerun exited 0.

Build logs: `/tmp/zedbsd-q163-amd64.log`, `/tmp/zedbsd-q163-pcat.log`,
`/tmp/zedbsd-q163-pc98.log`, all exit 0. No QEMU or physical device write was
needed for this codec-only phase. No test/build process remains running.

## Next prerequisite

Complete p006 exclusive admission before exposing `diskpart init`:
`BLKREREADPART` currently reserves only its own synchronous operation.
`backing_claim_prepare_disk` supports swap/loop and excludes writable mounts;
`backing_claim_check_mount` permits read-only mounts. Reusing these unchanged
would not protect initialization. `disk_reload_begin` already checks a sole
physical-device open and idle children under the registry lock, but its owner
is a thread and must not simply be retained across arbitrary userspace calls.
Design fd-lifetime ownership, close/error release, admission of new opens and
mounts, claimed I/O and owned reload; verify races/aliases and removed devices.
Then implement the public command, block FAT32/UFS formatters and p006/p007
integration. Product choices are settled; no user approval is pending.
