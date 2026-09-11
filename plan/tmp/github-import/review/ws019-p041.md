# ws019-p041: UFS file extent provider

Status: completed q174
Parent: [WS019](../ws.md)
Timebox: 90 active minutes

Implement the common file_extents capability for native UFS. Export ordered
512-byte-sector mappings with bounded scratch and contiguous-run coalescing.
Caller retains the prepared backing claim; mount lock serializes metadata
inspection. Validate regular-file geometry, complete-sector EOF, every direct
and indirect allocation, CG metadata exclusions, allocation bits, volume and
summary bounds. Refuse holes, malformed pointers, snapshots and unsupported
legacy rotational CG layout. Propagate callback and I/O errors; do not allocate
file blocks. Fixed scratch, no per-file-size mapping allocation in provider.

Focused production UFS host test covers direct/indirect mappings, fragmentation,
coalescing, tail EOF, malformed pointers/bitmap and callback errors. Three native
builds verify integrated callback registration. Formatter/swap admission remains
FAT-only until canonical identity and snapshot/claim exclusion are implemented
in the following phase. Do not claim UFS swap completion from this provider.
