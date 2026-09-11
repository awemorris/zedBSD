# p026 integration results

Status: completed in q122. Mandatory WS025 p001–p026 are complete; conditional p027–p030 were explicitly not adopted.

The starting ordinary source/config/artifact snapshot is
`../temp/p025-final-evidence-1`. No production source change has been made in
p026 so far. The p025 ordinary 8 GiB/four-CPU USB writeback/ZUJ2/readahead and
media-exchange result, and its checked shutdown/offline-content result, are
carried forward as explicit integration evidence.

Initial `p026-host-1` stopped in the legacy memory runner: new DMA vector code
references hal_fatal, which the old fixture did not provide. Added a test-only
abort/report implementation and linked it for the three DMA memory groups;
production decisions were unchanged. `p026-host-2` is the fresh serial host suite.
Its commands and completed statuses are retained in commands.json/results.json;
only terminal zero-status entries count as passed.

Static old-path audit: syscall_regular_buffer in syscall.c borrows bounded
io_pool storage for ordinary files and uses stack storage for small/unsupported
or exhausted-pool transfers. copy_segment_snapshot in elf.c borrows the same pool.
Persistent cache and workers own their backing separately. No old q087 per-call
large allocation remains on these ordinary syscall/exec copy paths; unrelated
bounded ioctl/sysctl/ELF-header allocations are not that workaround.

Remaining: finish host matrix, RAM/native combinations, FS50/Wi-Fi30, final
performance comparison, explicit effective defaults/conditional adoption, final
source/build evidence and M/W/P/Q synchronization.

## Final integration result

All required integration work is complete. See [mandatory acceptance mapping](acceptance-results.md),
[effective defaults and conditional adoption](effective-policy.md), and
[measured performance](performance.md). Historical pending notes above describe
checkpoints, not the final state.

- 24 host runner families in p026-host-2 pass, with ordinary/sanitizer variants.
- 14 RAM/firmware cells pass, including the separately added mandatory 2 GiB
  BIOS/UEFI cells; low/high USB and NVMe combined native tests pass.
- FS50 is 50/50 and Wi-Fi30 is 30/30 in both host variants. Native USB two-boot
  persistence passes in ws025-p026-final-2. The first FS50 attempt found an old
  test assumption that IN staging copied client bytes; the revised fixture seeds
  actual staging and still proves busy-reuse refusal and safe late completion.
  Wi-Fi diagnostics now use a fresh owning-WS path, preserving old q085 evidence.
- Both final baseline/oracle runs pass (404 samples): 256 KiB writes plus fsync
  median 10 ms on QEMU, with quantization and layout limits in performance.md.
- p026-final-evidence-1 verifies unchanged p025 production/config sources and
  identical amd64/pcat/pc98 kernel artifacts from supported make -j16 builds.
  The ordinary kernel has no link-only wrapper symbols. A fresh extended source
  snapshot includes bootloader files and current artifact hashes.
- No production workaround remains to remove from the ordinary syscall/exec
  transfer path. Correct bounded fallback paths remain. Final policy is recorded,
  including opt-in mount writeback and sole ZUJ2 v2 support.
- p027–p030 remain planned and not implemented, with explicit non-adoption and
  concrete resume criteria. These optional optimizations are not missing mandatory
  work. Physical runtime is user-accepted, not an agent measurement.

All builds/tests/runtimes are terminal. No commits were made. Ordinary artifacts
are restored, source/config provenance is recorded, and git diff --check passes.
