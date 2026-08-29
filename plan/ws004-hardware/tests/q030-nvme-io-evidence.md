# q030 ws004-p023 NVMe I/O and lifecycle evidence

Date: 2026-08-30

Scope: automatic software acceptance only. All NVMe writes target a disposable
QEMU namespace. This record makes no claim about the Latitude SN740; that
read-only hardware checkpoint remains `ws004-p025`.

## Focused fixtures

```sh
TMPDIR=$PWD/build/q030-tmp \
  plan/ws004-hardware/tests/run-nvme-admin-test.sh
TMPDIR=$PWD/build/q030-tmp \
  plan/ws004-hardware/tests/run-nvme-lifecycle-test.sh
TMPDIR=$PWD/build/q030-tmp \
  plan/ws004-hardware/tests/run-nvme-io-test.sh
TMPDIR=$PWD/build/q030-tmp \
  plan/ws004-hardware/tests/run-nvme-io-lifecycle-test.sh
TMPDIR=$PWD/build/q030-tmp \
  plan/ws004-hardware/tests/run-nvme-shutdown-lifecycle-test.sh
TMPDIR=$PWD/build/q030-tmp \
  plan/ws004-hardware/tests/run-devfs-block-range-test.sh
```

Result: every fixture passed its ordinary, ASan/UBSan, and GCC analyzer gate.
Together they cover admin/I/O encoding, exact status translation, checked
64-bit LBA and devfs-byte ranges, maximum-transfer splitting, queue/CQ phase
wrap, foreign and stale completions, CID/epoch reuse, multiple outstanding and
out-of-order commands, read-copy-before-slot-release, flush exclusion, timeout
and reset recovery, non-retry of an uncertain write, shutdown with in-flight
I/O, late completion, quarantine, and exactly-once BIO/DMA/resource release.

## Production builds

```sh
TMPDIR=$PWD/build/q030-tmp make -j16

mkdir -p build/q030-pcat-tmp
TMPDIR=$PWD/build/q030-pcat-tmp make -j16 \
  ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-pcat-xhci.mk \
  CONFIG_DRIVER_PCI_NVME=y \
  BUILD=build/q030-p023-pcat-final \
  vmunix
```

Result: the ordinary amd64 production build passed. The independent i386
PC/AT build compiled and linked `build/q030-p023-pcat-final/vmunix` with NVMe
enabled and passed the PC/AT image contract.

## Disposable QEMU I/O, wrap, concurrency, and restart

```sh
mkdir -p build/q030-tmp
TMPDIR=$PWD/build/q030-tmp \
  plan/ws004-hardware/tests/qemu-nvme-io.sh \
  build/q030-tmp/p023-qemu-run6
```

The retained evidence directory is
`build/q030-tmp/p023-qemu-run6`. Its `metadata.txt` records QEMU 10.0.11,
OVMF/q35, four CPUs, an IDE boot copy, and a standard PCI NVMe controller with
a 5-GiB disposable namespace. `results.tsv` records all three gates as pass:

```text
build                  pass    build.log
write-fsync-readback   pass    guest-1-logical.log
restart-readback       pass    guest-2-logical.log
```

On the first boot the raw `/dev/nvme0n1` descriptor completed
write/`fsync`/readback at byte offsets 8,388,608 and 4,294,971,392. It then
completed 96 disjoint 4-KiB write/readback commands beginning at
4,296,015,872 and four-worker concurrent write/readback totaling 128 commands
and 524,288 bytes beginning at 4,311,744,512. After QEMU and the controller
were stopped and recreated, the second boot verified all four regions from the
same namespace.

Both cells require at least 224 SQ1 I/O commands and require SQ1 tail and CQ1
head to wrap to zero. The retained QEMU traces satisfy those gates. Aggregate
trace counts were:

| Trace | NVM I/O commands | Reads | Writes | Flushes |
| --- | ---: | ---: | ---: | ---: |
| `nvme-trace-1.log` | 2,048 | 233 | 1,808 | 7 |
| `nvme-trace-2.log` | 233 | 233 | 0 | 0 |

`metadata.txt` records `result=pass`, `input_integrity=pass`, and
`acceptance_exit_status=0`. The production source image SHA-256 remained
`68d09ce83a2aaf1bb59cad5df34b9fcb94d14845ab286bcd4af5b51b6e7ea124`
before and after the run. The helper is installed only in the phase-owned test
image and is absent from the ordinary production image.

## Boot regressions

```sh
STORAGE_MODE=ide BOOT_TIMEOUT_SECONDS=90 \
  plan/ws004-hardware/tests/usb-overlay-boot-stress.sh \
  build/amd64/hdd-image.img build/q030-p023/ide-final 1

BOOT_TIMEOUT_SECONDS=90 \
  plan/ws003-bringup/tests/legacy-xhci-usb-boot.sh \
  build/amd64/hdd-image.img build/q030-p023/usb-final
```

Result: the IDE-root control passed its one requested pristine-copy boot and
reached `login:`. The legacy q35 xHCI USB-storage-only root also reached
`login:`. Their retained metadata identifies the input image SHA-256 as
`559691db5daf34f5c6cf66e11d4e4ed1bd95088b29b64a51bef97886b95a8f1c`.

## Remaining boundary

p023 deliberately does not provide scatter/gather, arbitrary user-page DMA,
IOMMU isolation, multiple I/O queues, namespace hotplug, multipath, power-state
management, or a physical-device claim. It accepts one 512-byte namespace and
uses bounded coherent 4-KiB bounce buffers. Strict primary/backup GPT
validation and partition publication remain `ws004-p024`; read-only SN740
acceptance remains `ws004-p025`.
