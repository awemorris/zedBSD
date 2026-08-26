# ws003-p002 Latitude UEFI memory-map evidence

Date: 2026-08-26

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

## Latest physical acceptance result

The 2026-08-26 Latitude run of the post-sequencing image again remains visibly
at:

```text
A64 UEFI ENTRY
A64 UEFI ELF
A64 UEFI READY
```

There is no visible improvement. This rejects the earlier final-map timing
hypothesis as a complete fix. It does not prove that `ExitBootServices()`
failed: after `READY` the loader uses only QEMU port `0xe9`, and an early kernel
handoff fatal occurs before framebuffer console initialization. The next image
must leave persistent GOP markers after successful `ExitBootServices`, final
normalization, CR3 load, and kernel entry, and must print bounded RSDP,
framebuffer, and paging-mode facts before exit.

One specific static mismatch is retained as a hypothesis, not yet a finding:
the loader accepts a 64-bit UEFI RSDP address while amd64 `bsp_boot_init()`
silently rejects addresses at or above 1 GiB. The diagnostic run must record
the Latitude RSDP before any mapping redesign is selected.

## q011 diagnostic image

The loader now prints these bounded pre-exit facts:

```text
A64 RSDP 0x...
A64 GOP  0x...
A64 LOW  0x...
A64 CR4  0x...
A64 MAPSZ 0x...
A64 DESCSZ 0x...
A64 DESCVER 0x...
A64 RANGES 0x...
A64 MARK 1=EBS 2=MAP 3=CR3 4=KERN
A64 UEFI READY
```

The marker is drawn directly into the top-right GOP framebuffer. One through
four large blocks respectively prove successful `ExitBootServices()`, final
map normalization, execution under the new CR3, and the first kernel
instruction. A normal kernel console clears it. Final firmware-call failures
are printed using the still-live UEFI console; a post-exit map rejection leaves
one large block plus 1--7 small lower blocks for the map-result enum.

The final q35/OVMF/xHCI/SMP=4 USB control reported:

```text
A64 RSDP 0x000000001f77e014
A64 GOP  0x0000000080000000
A64 LOW  0x00000000001f0000
A64 CR4  0x0000000000000668
A64 MAPSZ 0x0000000000001560
A64 DESCSZ 0x0000000000000030
A64 DESCVER 0x0000000000000001
A64 RANGES 0x000000000000001f
A64 UEFI BOOT SERVICES EXITED
A64 UEFI EXIT
A64 ENTRY PASS
login:
```

The host memory-map fixture and production build passed, and a PC/AT BIOS xHCI
USB control passed 1/1. This validates the diagnostic path under QEMU but does
not clear BR-T32. One Latitude cold boot of the exact final image and a photo
containing the numeric lines and marker panel are the next evidence.

The exact diagnostic image is `build/amd64/hdd-image.img`, SHA-256
`0c52d4da2c37100cf5cb0a9f9cec8707a0534541b8f3614b1fbee180e0143af1`.

## q011 physical diagnosis

The user-operated Latitude boot of that diagnostic image displayed four large
top-right blocks and these bounded facts:

```text
A64 RSDP 0x0000000064ffe014
A64 GOP  0x0000004000000000
A64 LOW  0x00000000001f0000
A64 CR4  0x0000000000000668
A64 MAPSZ 0x0000000000002070
A64 DESCSZ 0x0000000000000030
A64 DESCVER 0x0000000000000001
A64 RANGES 0x0000000000000087
```

`MAPSZ/DESCSZ` is 173 descriptors and 135 normalized ranges remains below the
256-range handoff capacity. CR4.LA57 is clear. The four blocks prove successful
`ExitBootServices()`, accepted final-map normalization, bootstrap-CR3
execution, and the first kernel instruction. The physical tier is therefore
U1, not U0.

The first subsequent condition in program order was the old silent
`bsp_boot_init()` rejection of `rsdp >= 0x40000000`, before framebuffer console
initialization. The measured `0x64ffe014` RSDP crosses that 1-GiB limit, so this
is the confirmed old stop rather than a remaining firmware-exit hypothesis.

## Corrected high-RSDP path

The general 1-GiB direct map and physical allocator remain unchanged. ACPI
discovery instead uses PDPT slot 509 as a dedicated 16-MiB, 4-KiB-granularity
sparse window. Mappings are persistent during discovery so RSDP/root/MADT
pointers remain valid, and are supervisor-only, read-only, NX, WB, and
non-global. Requests are bounded to 4096 aggregate unique pages, checked after
page rounding against CPUID MAXPHYADDR, and restricted on UEFI to normalized
ACPI reclaim/NVS/reserved ranges. Discovery is early-boot-only and completes
before AP startup. A capacity failure remains a clean unsupported boundary; it
does not expand the general physical-address model.

The legacy BIOS v1 handoff exposes only a usable-memory total. SeaBIOS places
its RSDT in a reserved gap immediately above that total, so the BIOS path keeps
its historical validated sub-1-GiB readable extent. This was verified after a
first regression run exposed the incomplete v1 map.

## Corrected-image software acceptance

BR-T24 uses one pristine production image with OVMF, q35, four CPUs, qemu-xHCI,
and USB mass storage. The user-selected completion matrix passes:

| Guest RAM | RSDP | Result |
| --- | --- | --- |
| 4 GiB | `0x7f77e014` | PASS to ACPI/IRQ ready, CPU 4, USB-root `login:` |
| 8 GiB | `0x7f77e014` | PASS to ACPI/IRQ ready, CPU 4, USB-root `login:` |
| 16 GiB | `0x7f77e014` | PASS to ACPI/IRQ ready, CPU 4, USB-root `login:` |

All three runs observed `A64 UEFI BOOT SERVICES EXITED`, `A64 UEFI EXIT`,
`A64 ENTRY PASS`, `A64 PAGING PASS`, `A64 ACPI RSDP PASS`, `A64 IRQ READY`,
four ready CPUs, and `login:`, with no fatal or storage-error marker. The
legacy-BIOS q35/xHCI USB control passes 1/1. The focused memory-map and sparse
window fixtures pass, as does `make -j16`.

The corrected final image is `build/amd64/hdd-image.img`, SHA-256
`5d6900b49f2edf51a742b94491783f1f6d7c5809ea57cd43d982140a825a0dd8`.
BR-T32 remains Uncleared: this exact image has completed 1/3 required Latitude
cold boots, with two repetitions remaining.

## Corrected-image physical run 1/3

The next user-operated Latitude run crossed every boundary owned by this
Phase. The display included `A64 ENTRY PASS`, `A64 PAGING PASS`,
`A64 ACPI RSDP PASS`, `A64 IRQ READY`, `A64 XMM CONTEXT PASS`, eight-CPU HAL
initialization, and `A64 TIMER TICK`. This is positive physical evidence for
the high-RSDP correction, but the repeatability requirement remains 1/3.

The first new failure is later: both discovered xHCI functions report
`attach failed at capabilities (13)`, no USB storage enumerates, and UUID
resolution has zero physical disks. That boundary is analyzed in
[latitude-xhci-evidence.md](latitude-xhci-evidence.md) and extracted as
`ws003-p003`; it is not a reason to reopen the UEFI/RSDP design.

## q011 software revalidation

The user-requested q011 rerun used the unchanged corrected production image,
SHA-256
`5d6900b49f2edf51a742b94491783f1f6d7c5809ea57cd43d982140a825a0dd8`.
Both focused host fixtures pass and `make -j16` reports the image up to date.

QEMU 10.0.11 reran BR-T24 with q35, qemu-xHCI, USB storage, four CPUs, and one
pristine image copy per case:

| Guest RAM | RSDP | Elapsed | Result |
| --- | --- | --- | --- |
| 4 GiB | `0x000000007f77e014` | 12 s | PASS |
| 8 GiB | `0x000000007f77e014` | 11 s | PASS |
| 16 GiB | `0x000000007f77e014` | 11 s | PASS |

The legacy-BIOS q35/xHCI USB control also passes 1/1 in 11 seconds. The BR-T24
harness now rejects an RSDP equal to 1 GiB as well as one below it, matching the
documented requirement that the RSDP be strictly above 1 GiB. The physical
BR-T32 state remains 1/3; software evidence cannot substitute for the two
remaining Latitude cold boots.
