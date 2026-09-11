# q131 boot provenance results

Date: 2026-09-09
Status: completed

## Implemented contract

UEFI V7 appends an 88-byte provenance record after the unchanged V6 memory
prefix. It copies the firmware partition and selected configuration partition
signatures and geometry, plus bounded configuration match count. No firmware
pointer survives into the kernel record. An aligned HAL copy is validated and
retained by generic boot code. Configured boot0 does not overwrite it.

Existing sysctl exposes read-only kern.boot.firmware_partition,
kern.boot.config_partition and kern.boot.config_matches. GPT sources are
PARTUUID selectors. Legacy provenance absence reports unavailable/zero; MBR
selectors are unavailable with a retained valid match count. No private
command or Noct binding was added. include/boot/provenance.h remains a shared
loader/kernel record and is an explicit UEFI build dependency.

## Evidence

- temp/q131-provenance-host: real loader path parser/copy, generic boot consumer
  and envelope classifier; 30 checks ordinary and ASan/UBSan PASS. Covers
  copied ownership, GPT byte order, invalid version/count, zero GUID, invalid
  partition/size/overflow, truncated path view, MBR unavailable, legacy reset,
  V7 truncated envelope/missing flags and preserved V6 classification.
- temp/q131-legacy-host: existing WS025 memory ownership and WS003 parameter
  fixtures ordinary + ASan/UBSan PASS. No obsolete fragment generator used.
- temp/q131-ordinary-final: PASS ordinary. Expected IDs are read from actual
  GPT entries; USB firmware ESP differs from selected payload. An unrelated
  NVMe contains its own zedbsd.cfg but the reported match count stays one.
- temp/q131-override: PASS override. Boot log explicitly resolves
  boot0 UUID=7819-0000 to /dev/nvme0n1p1, with root/data loops from boot0. Reported
  firmware/config sources remain the real USB origins and count stays one.
- temp/q131-ambiguous: PASS ambiguous. Both ESP and payload contain valid
  configurations; the loader chooses ESP, reports both origins as ESP, preserves
  match count two and emits its existing ambiguity warning. Runtime boot0 is
  explicitly kept on payload.
- temp/q131-legacy: PASS legacy. The explicitly retained pre-V7 loader still
  boots; the new sysctl reports unavailable sources and zero count.
- All QEMU cases reject writing the match count and preserve it afterward;
  production image hashes remain unchanged. Only disposable copies were used.
- /tmp/zedbsd-q131-{amd64-final,pcat,pc98}.log: explicit make -j16 PASS for all
  three architectures. /tmp/zedbsd-q131-fixture-final.log PASS. The final amd64
  vmunix, sysctl and BOOTX64.EFI hashes are identical to those used by the
  override/ambiguous/legacy cases after include-path/formatting maintenance.
- git diff --check PASS. No commits or aggregate make check.

Initial QEMU fixture preparation assumed two GPT entries; the ordinary image
also carries a BIOS-loader entry. It now locates ESP by type and payload by
its actual filesystem location. That failed preparation never launched QEMU.

## Remaining installer boundaries

Source selectors establish origin, not a content snapshot or destination
ownership. p017 still supplies command staging; p004 must reject ambiguous,
missing or duplicate identities and validate artifacts before publication.
MBR installation remains outside the initial GPT installer scope.
