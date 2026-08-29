# q030 ws004-p022 NVMe admin/Identify evidence

Date: 2026-08-29

Scope: automatic software acceptance only. This record makes no claim about
the Latitude SN740; that read-only hardware checkpoint remains `ws004-p025`.

## Focused fixtures

```sh
plan/ws004-hardware/tests/run-nvme-admin-test.sh
plan/ws004-hardware/tests/run-nvme-lifecycle-test.sh
```

Result: ordinary, ASan/UBSan, and GCC analyzer variants passed for both the
admin protocol helper and the production-shared cleanup ledger. The fixtures
cover malformed capabilities, queue arithmetic, CQ phase/SQ/SQ-head/CID
ownership, Identify buffers, sparse active namespace identifiers, every attach
prefix, every fallible cleanup point, quarantine/retry, exact release order,
and double-release prevention.

The pre-existing PCI MSI fixture was extended to require exact restoration of
the saved conventional-MSI address/data/control image and the saved MSI-X
control/table entry after checked removal. Its ordinary, ASan/UBSan
(`detect_leaks=0`, because LeakSanitizer is unavailable under the executor's
ptrace environment), and GCC analyzer variants passed. The MSI fixture starts
with nonzero Multiple Message Enable bits, proves the one-message mapping
clears them while active, and proves teardown restores the exact saved value.

## Production builds

```sh
make -j16

make -j16 \
  ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-pcat-xhci.mk \
  CONFIG_DRIVER_PCI_NVME=y \
  BUILD=build/q030-p022-pcat \
  vmunix
```

Result: amd64 built successfully; the independent i386 PC/AT compile, link,
PC/AT contract check, and Multiboot check passed with NVMe enabled.

## QEMU Identify and non-mutation

```sh
BOOT_TIMEOUT_SECONDS=90 \
  plan/ws004-hardware/tests/qemu-nvme-admin.sh \
  build/amd64/hdd-image.img \
  build/q030-p022/nvme-final-4
```

Environment: QEMU 10.0.11, OVMF, q35, four CPUs, disposable IDE root copy, and
one standard 32 MiB PCI NVMe namespace.

Observed result:

```text
nvme: PCI controller 0000:00:04.0 version=10400 queue=64 timeout=7500ms MSI-X
nvme: /dev/nvme0n1 namespace=1 blocks=00000000:00010000 block-size=512 read-only
login:
```

Exactly one NVMe namespace marker was present. The source image digest remained
`3ee1bdc649798162e5edf06caa574d16d4505bb4af6c4c7d99c91ca2819d54cb`.
The namespace digest before and after remained
`83ee47245398adee79bd9c0a8bc57b821e92aba10f5f9ade8a5d1fae4d8c4302`.
The p022 discovery disk intentionally reports `EOPNOTSUPP` for reads, so the
generic partition scan logs `geometry error=21`; usable NVM reads begin in
`ws004-p023`.

## Boot regressions

```sh
STORAGE_MODE=ide BOOT_TIMEOUT_SECONDS=90 \
  plan/ws004-hardware/tests/usb-overlay-boot-stress.sh \
  build/amd64/hdd-image.img build/q030-p022/ide-final-4 1

BOOT_TIMEOUT_SECONDS=90 \
  plan/ws003-bringup/tests/legacy-xhci-usb-boot.sh \
  build/amd64/hdd-image.img build/q030-p022/usb-final-4
```

Result: IDE root passed 1/1 pristine-copy boot; the xHCI USB-only BIOS root
reached login.
