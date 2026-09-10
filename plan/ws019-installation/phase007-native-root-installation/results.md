# q183 native UFS final acceptance

Status: completed q183

q182 completed the actual native/coexistence installer, source-free root/swap/
persistence/halt and supported builds. This phase tests real paging, not only
activation. The test-only wrapper reuses the WS016 pressure worker and checks
16 sentinel offsets per page across three generations, positive page-in/page-out,
unchanged I/O errors, and release/reactivation. A second file is allocated in
alternating 64-KiB writes with another file and must report at least 32 extents
before it is accepted as fragmented.

q183-pressure1 was intentionally interrupted (exit 130) before the fragmented
branch: the fixture originally selected /tmp, which is tmpfs on an installed
root. It observed over 2,000 real page-outs to /swapfile with no I/O errors but
is not accepted as a completed run. The corrected fixture uses /root and checks
its device number against /swapfile before work. Accepted q182-native5 remains
unchanged; each run boots a disposable clone with no source disk, 128 MiB RAM
and four CPUs. Only its ESP receives the test probe; no production command is
added and the installed root is not provisioned by the host.

q183-pressure2 stopped before pressure (exit 1): bare stat resolved to the shell
builtin, which lacks -c. The fixture now uses /bin/stat, as the actual installer
does. This is a fixture correction, not evidence of a swap defect.

## Final result

q183-pressure3 PASS, terminal 0; /tmp/zedbsd-q183-pressure3.log and
ws019 temp/q183-pressure3/result.json retain the complete run. The installed
/swapfile had 5 extents. Three generations each verified 28,480 pages at 16
sentinel offsets/page, with 2,057 page-ins and 4,120 page-outs per generation;
including lifecycle work, totals were 6,192 page-ins / 12,360 page-outs.
The deliberately fragmented file had 513 extents. Three generations each
verified 28,480 pages, with 2,062 page-ins and 4,130 page-outs; totals including
lifecycle were 6,207 page-ins / 12,390 page-outs. No I/O error increment or
pattern mismatch occurred. Each generation released/reactivated the active
source; no additional swap source was present. The test removed both scratch
files, restored /swapfile, verified persisted installer content, and confirmed
normal halt with all four CPUs stopped and interrupts disabled.

Accepted q182-native5 SHA256 before/after remained
0e7b072d44f912e5ecc403c7bfffdf8e6f9b9ec85bda464bc589af0ab91d9146.
A post-test CPU query found the QMP socket already removed because the test had
completed; this did not affect its result. No production source changed in q183;
the q182 supported-build gates remain applicable. The new probe compiled with
-Wall -Wextra -Werror, and git diff --check passes.

## Consolidated acceptance

- Actual public native install, unchanged source, default NO, preserved tree
  attributes, boot configuration last, two source-free boots and persistence:
  [p049](../phase049-native-installer-integration/results.md), q182-native5.
- Coexistence cancel/install/rerun/conflict and protected bytes: p049 q182-coexist.
- Native format bounds, inode/block layout and formatter failures:
  [p036](../phase036-native-ufs-codec/results.md),
  [p037](../phase037-native-ufs-command/results.md),
  [p048](../phase048-native-format-preflight/results.md).
- Ownership, data/metadata aliases, holes, indirect mappings, snapshots and
  activation/mutation cleanup: p040-p045, including q178 native lifecycle tests.
- Filename-safe census, archive copy, hardlinks/symlinks/timestamps and copy/
  metadata errors: [p028](../phase028-native-tree-copy-tools/results.md).
- Publication and cleanup faults: p049 transaction/workspace/finish gates.
- Actual UFS paging, fragmented indirect file, repeated lifecycle and halt:
  q183-pressure3 above.

p007 is complete. This completes the text installer acceptance. The required
BeUI frontend p029 is still unimplemented; WS019 remains open for that work.
