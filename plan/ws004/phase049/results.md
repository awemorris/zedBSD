# ws004-p049 results

Date: 2026-09-06. Result: completed for the selected synchronous/BOT boundary.

- Synchronous storage URBs reserve private staging before reclaim/I/O locks.
  Core synchronous helpers also isolate their payload. Two additional checked
  cancellation attempts and bounded drain replace the old indefinite caller wait.
  Failure retains the HCD reference, request, and staging. Reuse returns EBUSY
  until real retirement. No synthetic completion or unconfirmed DMA release.
- EP0 keeps a separate inflight marker through HCD retirement after the original
  caller returns. Existing binding tests initially exposed the missing distinction;
  both finite return and one-at-a-time EP0 admission now pass.
- CSW STALL clears halt and rereads CSW once. A transport failure may reset/reissue
  once; one subsequent current-reset 06/29/00 may be retried. Unsolicited/reset
  repetition and changed/absent medium do not authorize continued writes.
  USB object retention and core admission prevent retry on a replacement device.
- One command deadline spans reissue and sense; retirement/endpoint recovery
  retains its separate bounded grace. Final flush failure is latched only after
  recovery fails. No mount readonly flag is cleared.
- Storage maximum BIO size follows logical-sector bytes. 4KiB sectors fit two
  blocks in the 8KiB reserve; a larger logical block reserves storage before disk
  publication rather than using an undersized buffer.

S01–S18 pass production USB/core and BOT fixtures, ordinary and ASan/UBSan.
USB core recovery: 1111 checks; function model: 1833; binding transactions:
971, ordinary/sanitized; HCD unregister and production source/object gates pass.
The existing xHCI lifecycle model passes under ASan/UBSan; it is a model, not
a hardware-injected completion-19 run. Final actual xHCI USB-root integration
and three configured builds pass; physical device fault injection was not done.

GCC analyzer's binding-fixture merged path incorrectly assumes a successful
non-NULL bus->ports allocation and then takes bus->ports == NULL. Only its
malloc-leak diagnostic is disabled for that fixture; the exact allocation ledger,
ordinary/sanitizer gates and other analyzer diagnostics remain active.
Retained-resource accounting is not inferred from an analyzer suppression.

[Shared evidence and per-scenario result](../../ws018/phase019/results.md).
Controller/device reconstruction and physical recovery remain explicit
[follow-ups](../../old/fs-report-followups.md), not completed here.

