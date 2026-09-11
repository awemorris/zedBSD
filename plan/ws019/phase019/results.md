# q148 pristine verification results

Date: 2026-09-09
Status: completed

Existing mkfs/mkswap commands now support `--verify-pristine`. The read-only
frontend retains size/name/descriptor identity, closes on failure and performs
no reservation or mutation. UFS records exact generator-verified intervals,
rejects overlap/bounds errors, then verifies all gaps and trailing bytes as
zero. Swap checks the complete canonical header and every unused slot byte.
Normal formatting keeps its original reservation and flush/reopen path.

## Evidence

- `/tmp/zedbsd-q148-pristine2.log`: 3,071 checks each ordinary and ASan/UBSan.
  Independent Noct UFS, journal/snapshot and swap images pass unchanged.
  Minimum, non-block-aligned, multi-CG and maximum UFS geometries pass both
  profiles; corrupt metadata/journal/gaps/free area/last bytes fail. Exact
  interval overlap, bridging, capacity and overflow checks pass. Read error,
  short-read and EINTR cases cover metadata, unused UFS gap and swap slot.
- `/tmp/zedbsd-q148-format-file3.log`: original reserved frontend 2,650 checks
  and new read-only frontend 623 checks each ordinary/sanitized. The new oracle
  forbids write/reserve/fsync, substitutes descriptor/path fields and special
  objects, fails each public operation, checks close/first-error behavior, and
  executes the real command parsers.
- `/tmp/zedbsd-q148-ufs-regression.log`: 12,521 checks each ordinary/sanitized,
  with maintained builder byte equality.
- `/tmp/zedbsd-q148-swap-regression.log`: 49,333 checks each ordinary/sanitized,
  with maintained builder byte equality.
- `/tmp/zedbsd-q148-reservation3.log`: 605 checks each ordinary/sanitized,
  current production file/backing-claim/VM/cache/vmspace ownership. The old
  fixture failed to link after VM consolidation. A separate current-VM wrapper
  displaces obsolete fixture definitions and retains real metadata-owner and
  address-space lock assertions; unexpected hardware/swap operations abort.
- `/tmp/zedbsd-q148-{amd64,pcat,pc98}.log`: explicit CI `make -j16` PASS.
  `/tmp/zedbsd-q148-fixture.log`: private native fixture build PASS.
- `plan/ws019/temp/q148-format/result.json`: PASS format.
  Actual guest mkfs/mkswap pristine success, damaged metadata/free area/end and
  swap-slot rejection, with identical SHA-256 before/after each verification.
  Restored contents pass. Old type, special file, symlink and active swap
  formatting refusal still pass. GPT, FAT boot/label, unrelated sentinel and
  production input hash remain unchanged.

Initial sandbox LSan failure was a ptrace limitation; approved runs retain
leak detection. A first added frontend fixture had a callback-name collision;
it was corrected before the successful suite. No production acceptance gate
was weakened. No physical installation or p004 transaction is claimed.

## Remaining installer work

p004 can use the read-only commands for existing final data/swap images, with
identity revalidation before publication. This is a content observation, not a
lease against concurrent writers. The Noct transaction, managed-file staging,
confirmation, failure cleanup and p005 NVMe-only installed boot remain open.
