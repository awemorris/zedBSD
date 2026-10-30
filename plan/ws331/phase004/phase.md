<!-- awesome-plan project=zedbsd record=ws331-p004 -->

# ws331-p004: i915 の map と MSI、試験の代役

Status: cleared（2026-10-30 Q1。build、i915 の display の host 試験、contract の試験。ktest-sync の 5330 は未実施）
Parent: [WS331](../ws.md)
Queue: q1717（P2）

## 目的と範囲

p001 §5 の p004 の行のとおり。i915 の WC・読み出しだけの map を B1、MSI を B2〜B5、irq の型を kern_irq_ack_t、host の試験の代役の名前と型を直す。ktest-sync.c の HAL の契約の試験の扱いは Q1 と決める。確かめ: build と i915 の host 試験。

承認: 2026-10-30 ユーザー「表A,Bを許可します」。HAL の API は変えない。

## 結果（2026-10-30、P2、worktree `agent/p2-ws331`、commit 157f65d69）

- map（B1）: device.c（GMADR の aperture）・display/bootfb.c（別の device の framebuffer の view）を `KERN_DEVICE_WRITECOMBINE`、display/vbt-shell.c（option ROM）・display/xd-display.c（OpRegion）・display/xd9-firmware.c（VT-d の register の page）を `KERN_DEVICE_UNCACHED | KERN_DEVICE_READONLY`。unmap は `kern_device_unmap`。戻り値の比較は `HAL_OK` から 0 に。amd64 の HAL に渡る属性は前と同じ（WC は READ\|WRITE\|WC、読み出しだけは READ に NOCACHE が付くが、amd64 の device の窓は WC 以外を全部 cache 無しで作るので同じ）。
- MSI（B2〜B5）: pci.c は `kern_irq_alloc_msi`・`kern_irq_free_msi`（message address は `uint64_t`）、irq.c は `kern_irq_attach_msi`・`kern_irq_detach_msi_sync`、handler の引数の型は `kern_irq_ack_t`、EOI は `kern_irq_send_eoi`。log の文字列 `hal_irq_attach_msi(irq=%d)` は `kern_irq_attach_msi(irq=%d)` に（照合する道具は無い、grep で確かめた）。
- i915.c（`kern_boot_handoff`）・worker.c（`kern_cpu_current`）・ppgtt.c（`kern_physaddr_t`）、include だけの display.c・ktest-gt.c の `hal/hal.h` を外した。
- host の試験の代役: tests/display/host-kernel.c は `kern_device_map`・`kern_device_unmap` の代役に、tests/contracts/host_unreached.c は `kern_irq_alloc_msi`・`kern_irq_free_msi` の代役に、3 file（host-kernel.c・host_unreached.c・host_render.c）の `kern_pmem_to_kernel` の引数を `kern_physaddr_t` に。
- ktest-sync.c（in-kernel の試験）は Q1 の判断（2026-10-30、案 A: driver の試験は kernel の口を試す）で kernel の口の試験に直した: MSI M0〜M4 は `kern_irq_*_msi` で同じ手順、期待値は errno（INVALID→EINVAL、STATE→EBUSY）。WC の全範囲の確かめは `kern_device_map(..., KERN_DEVICE_WRITECOMBINE | KERN_DEVICE_READONLY)` で同じ。WC の契約の確かめは、kernel の口が方針を 1 つしか名指せないので、知らない方針の値（3）と知らない bit（0x200）を EINVAL で拒み出力を触らない事の確かめに置き換えた。**HAL の WC|NOCACHE・WC|DEVICE の拒否の確かめは driver の試験から無くなった**（HAL の側に試験の置き場は今無い）。照合の文字列は、plan/tools.md・tests/scenarios・未完の WS から参照されていない（grep。plan/ws031 の過去の結果の記録にだけ "MSI M0-M4" の語がある）。

### 確かめ

- build: `plan/ws331/tests/build-vmunix-all.sh build/p004 amd64 intelmac` rc=0・warning 0。ktest-sync.c を入れる `I915_TESTS=y I915_TEST_SET=execution` の amd64 の vmunix も rc=0・warning 0。
- host 試験: `sh plan/tools/i915-display/run-xd-host-tests.sh build/p004/xd-host` → `run-xd-host-tests: all passed`（P2 が流した）。
- `src/drivers/gpu/i915/tests/contracts/run.sh` は自分の一時の directory を `rm -rf` で消すので P2 は流さず、Q1 が 157f65d69 で流した: `i915 contract tests PASS: mmio dma pci rpm pte sync rps memory forget reset (ordinary + ASan/UBSan)`（Q1 の報告）。
- grep: `src/drivers/gpu/i915` に `hal_`・`HAL_`・`hal/`・`paddr_t` の残り 0。
- QEMU: amd64 の uefi-nvme の login PASS（6f31f7e03、p003 の表）。QEMU に i915 は無いので i915 の map・MSI の実行は通っていない。
- 未実施: ktest-sync の実行と i915 の実機（5330）の起動（5330 は割り当てられていない）。
