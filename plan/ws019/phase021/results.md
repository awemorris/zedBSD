# q154 process-path mount acceptance

Date: 2026-09-09
Status: completed

The mount/unmount syscalls now resolve using a retained snapshot of the caller's
cwd/root. Mount follows the actual target, requires an existing directory and
checks its covered inode identity again under the namespace transaction before
publication. Unmount retains the resolved mount identity across lookup cleanup
and the existing quiesce/busy/sync/teardown protocol. Bootstrap mount and mount_at
callers keep their previous contracts; root-only syscall authority is unchanged.

## Evidence

- Actual mount/namei/inode/cwdinfo host fixture: 175 checks each in normal and
  ASan/UBSan runs, including target replacement, symlinks, child/open ownership,
  failed sync retry and process-root containment. `/tmp/zedbsd-q154-nested3.log`.
- Existing namespace suite: 1303 checks, 21 threaded admission/rollback races,
  stale lookup and covered-directory protection, normal and sanitizers.
  `/tmp/zedbsd-q154-mount-regression.log`.
- Existing writeback/unmount suite: 153 checks each, normal and sanitizers;
  failed sync/prepare, restored policy ownership and retry.
  `/tmp/zedbsd-q154-writeback.log` and WS025 `temp/q154-unmount/` artifacts.
- `make -j16` with explicit amd64, PCAT and PC98 CI configs passes. Logs:
  `/tmp/zedbsd-q154-amd64.log`, `-pcat.log`, `-pc98.log`.
- Private fixture build passes: `/tmp/zedbsd-q154-fixture3.log`.
- QEMU `../temp/q154-source2/result.json`: PASS source inspection. Guest log
  also records native nested mount PASS for relative/symlink resolution,
  child mount and unlinked-open-file EBUSY, failed mount retry, read-only
  refusal, nonroot EPERM and complete owned-directory cleanup.
- The same guest successfully mounts the real USB ESP and payload read-only
  below /run, captures EFI/kernel/rootfs identities and SHA-256 values, checks
  their formats, builds the manifest and unmounts both. Protected GPT/FAT boot
  metadata/sentinel hashes and the production image hash remain unchanged.

## Boundaries and failures retained

There is no public chroot syscall. The first private fixture failed to compile
because it assumed one; the native test now exercises existing setuid authority.
Root containment is proven by the real host cwdinfo/namei implementation, not
claimed as a native chroot result.

The first native run (`temp/q154-source`) exposed populated tmpfs teardown
returning EBUSY for internal directory-entry inode references. That independent
ownership defect remains [p022](../phase022/phase.md). The final
path test removes its own child directory before teardown and uses an unlinked
open file so its busy check cannot pass merely because of namespace references.
No populated-tmpfs teardown success or physical-machine test is claimed.
