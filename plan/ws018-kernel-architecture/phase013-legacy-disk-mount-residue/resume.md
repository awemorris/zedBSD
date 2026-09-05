# Q077 resume preparation — 2026-09-05

Parent: [p013](phase.md). Execution authority and limits: [q077](../../queue.md).

This checkpoint prepares continuation; it does not complete p013/p014/p015 or
start the shared QEMU campaign.

## Workspace and fresh evidence

The target checkout is `/home/awe/zedBSD`, HEAD `d97e21c` (`WIP`). Preserve the
existing tracked edits and untracked q076/q077 plans and fixtures. They contain
the p013 removal, unfinished p014 correction and p015 diagnostics. Other
zedBSD checkouts under `/home/awe` are not this continuation's workspace.

Commands rerun from the checkout root during preparation:

```sh
python3 plan/ws018-kernel-architecture/tests/audit-legacy-disk-mount.py
sh plan/ws018-kernel-architecture/tests/run-legacy-disk-mount-test.sh
git diff --check
```

- KA-T120 PASS: 1,404 active source/test files, all six manifests and retained
  flags/callers.
- KA-T121 PASS in ordinary and ASan/UBSan modes: 1,303 checks each, 21 threaded
  admission cases, root preparation/rollback, stale lookup publication and
  metadata preparation/I/O-lock ordering. Covered-directory probe returns
  `rmdir=17 expected=17 callbacks=0` in both modes.
- `git diff --check` passed before this documentation addition.
- `cc`, Python, make, `qemu-system-x86_64`, the project clang path and maintained
  PCAT/PC98 configuration files are present. No build or QEMU process was
  observed during preparation. No QEMU was launched by this preparation.

These are current focused host results. Earlier supported build passes in
p013 are historical and do not validate the final p014 patch.

## Concrete next action and remaining gates

1. Review the current `prepare_mutation` changes in inode/overlayfs and finish
   the FS-A04 lock-order verification against the actual overlay implementation.
   The passing KA-T121 runner links mount/inode/namei/namecache/cwdinfo, **not**
   `overlayfs.c`; its generic callback ordering check cannot close that boundary.
   Perform the required independent review of the final namespace patch.
2. Rerun the declared storage, backing claims, identity, native FAT, boot-source
   and relevant overlay regressions after the final correction. Preserve all
   baseline failures in the [audit ledger](../tests/q077-filesystem-audit.md).
3. Run the final supported builds:

   ```sh
   make -j16
   make -j16 ZEDBSD_CONFIG=plan/ws021-llvm-toolchain/tests/config-pcat.mk
   make -j16 ZEDBSD_CONFIG=plan/ws021-llvm-toolchain/tests/config-pc98.mk
   make -j16 -f Makefile -f plan/ws019-installation/tests/storage-qemu.mk ws019-storage-qemu-fixture
   ```

4. Only after host/build prerequisites pass and remaining q077 budget is
   established, run the shared cell with its mounted-namespace probe enabled:

   ```sh
   python3 plan/ws019-installation/tests/run-storage-qemu.py combined plan/ws019-installation/temp/q077-resume-01 --mount-protection
   ```

   The output directory must be fresh. Use only disposable media/OVMF copies,
   root-level `/q076`, and `qemu-system-x86_64`. The existing Queue allows at most
   two launches total, 120 seconds to boot and 600 seconds per whole cell; a
   second launch requires a diagnosed in-scope correction. No q077 runtime
   evidence was found in the inspected WS018/WS019 temp directories, matching
   p013's recorded zero-launch checkpoint. Prior active review time is not
   recoverable from these records: do not reset or silently extend the shared
   four-active-hour window.
5. Record real FAT mount protection, explicit ro/rw/root reload EBUSY,
   persisted-add exit 3/no live replacement, reboot discovery and no automatic
   auxiliary mounts, then synchronize P/W/M/Q according to actual results.

## Audit scope still open

FS-A05 is a reproduced UFS1/UFS2 lost metadata update. FS-A06/A07/A08/A09 and
unreviewed coverage rows remain as recorded in the audit ledger. The current
request for preparation supplies no new decision authorizing unrelated UFS or
overlay-remove repairs. Retain them for an explicit scope/verification decision;
do not report a clean filesystem audit from the focused host PASS.

No production source edits, full builds, QEMU launches, commits or pushes were
performed during this preparation.
