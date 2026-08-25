# ws003-p002 Latitude UEFI memory-map evidence

Date: 2026-08-25

## Physical baseline

The user-supplied Latitude 5320 photograph shows:

```text
A64 UEFI ENTRY
A64 UEFI ELF
A64 UEFI READY
Normalize memory map: 0x8000000000000001
```

The status is the loader-generated `EFI_LOAD_ERROR`. The old normalizer
required firmware descriptors to arrive in increasing physical-address order,
but its generic diagnostic could not distinguish ordering from overlap,
overflow, malformed stride, capacity, or an empty effective map.

## Implemented software correction

The normalizer now canonicalizes valid arbitrary descriptor order without
allocating after the final `GetMemoryMap`. It merges same-type adjacent ranges
and rejects overlap, overflow, malformed stride, capacity exhaustion, and an
empty map with distinct bounded reason strings.

The focused BR-T23 host fixture passes. A rebuilt production image booted under
OVMF from q35 `qemu-xhci` USB storage and emitted:

```text
A64 UEFI ENTRY
A64 UEFI ELF
A64 UEFI READY
A64 UEFI EXIT
A64 ENTRY PASS
```

It subsequently enumerated USB storage, resolved the FAT UUID, mounted the
loop-backed overlay, started init, and reached `login:`.

## Uncleared physical gate

The first corrected-image run no longer printed a normalization rejection, but
stopped visibly at:

```text
A64 UEFI ENTRY
A64 UEFI ELF
A64 UEFI READY
```

The loader had been normalizing the final snapshot before consuming its map
key. This gap can repeatedly invalidate the key on firmware with asynchronous
map changes and ended in a silent halt after three `EFI_INVALID_PARAMETER`
results. The retry path now performs `GetMemoryMap()` immediately followed by
`ExitBootServices()` and normalizes the accepted snapshot afterward.

BR-T32 remains Uncleared. Re-run OVMF first, then require three Latitude cold
boots to pass ExitBootServices and reach an unambiguous kernel-entry marker.

The post-sequencing OVMF control has now passed. With q35, four CPUs, xHCI, and
the production USB image it emitted `A64 UEFI EXIT`, entered the amd64 kernel,
mounted the loop-backed overlay, started init, and reached `login:`.
