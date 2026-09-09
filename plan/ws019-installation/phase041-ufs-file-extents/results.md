# p041 results

Mapping-provider implementation completed q174. UFS backing admission remains closed.

Added UFS file_extents provider: fixed scratch, contiguous sector-run coalescing,
sector-exact EOF, direct and three-level indirect resolution, allocated-fragment
checks, CG metadata and summary exclusion, disk bounds, snapshot refusal and
error propagation. Native rotational-offset-free geometry is supported. This
provider does not enable UFS formatting reservations or swap by itself.

Current production source is included directly by the host fixture. Old source
fragment extraction cannot handle the refactored driver (missing consolidation
markers), so the test now opts into the complete current driver. Initial build
attempts exposed obsolete extraction, unused whole-filesystem service linkage,
and missing host IRQ stubs. Recorded in /tmp/zedbsd-q174-host-build*.log and
/tmp/zedbsd-q174-host4.log; none was a target runtime failure.

/tmp/zedbsd-q174-host6.log: normal and ASan/UBSan both PASS, 117 checks, terminal 0.
Independent medium layouts cover contiguous/fragmented data, single/double/triple
indirection, final 512-byte EOF, holes, reserved/out-of-range/misaligned pointers,
summary collision, free allocation bits, snapshot refusal, callback errors,
I/O failure lock release and actual native 1024/8192-byte fragment/block geometry.
No storage write occurred. The host fixture uses the current complete source,
not generated copies or an alternative provider implementation.

Before enabling UFS swap, remaining backing-owner admission must check physical
self-overlap and data/indirection overlap. Existing backing_claim_finalize only
compares ranges to OTHER claims; swap validate_extents only checks logical
coverage and disk bounds. Allocation-bit validation is not ownership proof.
Canonical UFS identity and symmetric snapshot exclusion also remain necessary.
These are p007 admission prerequisites, not proven by this mapping iterator.


All three disk-image builds completed with exit 0:
/tmp/zedbsd-q174-amd64.log, /tmp/zedbsd-q174-pcat.log,
/tmp/zedbsd-q174-pc98.log. No QEMU acceptance of UFS swap is claimed: its
admission is still FAT-only and the required protection work remains next.
No build or test process remains running.
