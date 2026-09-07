# p026 integration execution design

2026-09-08 JST. Mandatory dependency p025 is complete. Standing user approval
covers this finite queue; review evidence at each 90 active minutes.

1. Freeze source/config provenance from p025-final-evidence-1 and inventory the
   maintained runners. Run the mandatory production-linked host/sanitizer gates
   for memory/vmap/pool, BIO/vector, cache/buffer, FAT/UFS batching, durability,
   writeback/readahead/exec and journal/crash. Preserve diagnostics and execute
   one build/test/runtime at a time. A runner adaptation is not a production
   switch. Stop on a failure, diagnose it, and rerun the affected dependencies.
2. Run the final BIOS/UEFI RAM matrix (256, 512, 1024, 4096, 8192, 16384 MiB),
   four CPUs and USB root, checking reported usable ranges. Combine ordinary
   writeback/ZUJ2/readahead at low and high RAM; include NVMe through the bounded
   adapter. p025 already supplies actual equal-capacity USB media replacement and
   checked shutdown. Retain prior p023/p024 specialized high/fragmented mapping
   and SG proofs where their implementation is unchanged; state exact evidence.
3. Run FS50 and Wi-Fi30 maintained acceptance and q087 transfer regression. Build
   any required user fixtures with make -j16, without adding probes to production.
   Repeat native baseline at 4/16 GiB with recorded FAT alignment, syscall/backend
   counts and latency. Do not attribute timing differences to one cause without
   a controlled comparison; reduced copies are not a CPU-speedup claim.
4. Audit final effective defaults and the old per-call allocation/adapter paths.
   Record which filesystem/device/operation uses each path. Keep justified
   memory-pressure/unsupported fallbacks. Writeback remains opt-in unless the
   evidence and persistence contract warrant a concrete default change; document
   limitations and disable/drain behavior. ZUJ2 v2 is the only supported journal.
5. Explicitly decide p027–p030 adoption against measured bottlenecks/hardware.
   Missing evidence means not adopted in WS025 and planned with a concrete resume
   condition, not falsely implemented. Physical acceptance is user-accepted and
   never an agent hardware measurement or a source of invented tuning numbers.
6. Finish supported x86 builds and any necessary ordinary artifact restoration,
   source/artifact hashes, mandatory acceptance mapping and M/W/P/Q updates.
   Mark WS025 and its active goal complete only after all required work passes.

A source edit invalidates dependent gate evidence, not unrelated earlier proven
contracts. Reuse is explicit, with source comparison; do not blindly rerun large
unchanged tests or hide a failure behind an earlier successful phase.
