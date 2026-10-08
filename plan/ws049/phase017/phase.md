<!-- awesome-plan project=zedbsd record=ws049p017 -->

# ws049-p017: driver 向けの公開の口

Phase ID: `ws049-p017`
Parent: [WS049](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-093 PASS。⑤ は 2026-10-05 に不要として閉じ済み（UCSI は hal_space_map_device の error で判断））（旧: in-progress（2026-10-04。実装と host の確認、T1-093 の QEMU の回帰が PASS。Q1 の判定待ち。⑤ はユーザーの判断待ち））
Phase disposition: normal
Queue: q696 / q696-i01（P1）。承認: Q1 の Queue（[queue.md](../../queue.md) の q696、WS050 design §12 の提案、2026-10-04）

## 目的と受け入れ

WS050（UCSI）の p003 と、WS051・WS052・WS132 の ACPI の利用者が使う公開の口を `include/drivers/acpi/acpi.h` と WS049 の source に足す
（[WS050 design](../../ws050/design.md) §12）。HAL の API は変えない。

1. AML の mutex を名前で取った interpreter の entry の中で callback を走らせる `drv_acpi_run_locked()`。
2. package の生成と要素の設定の公開（`drv_acpi_object_package_new()`・`drv_acpi_object_package_set()`）。
3. `drv_acpi_notify_remove()` と、install・remove を interpreter の entry の中で行う直列化。
4. `_CRS` の共通の解析（`drv_acpi_resources_walk()`: IO・FixedIO・IRQ・Extended IRQ・Memory24・Memory32・Memory32Fixed・Word/DWord/QWord・
   Extended の address space）。EC の driver の `read_ports()` をこれに置き換える。
5. memory map の型を問う口の要否の調査（HAL の API が要るなら止めて相談）。

受け入れ: host の試験（ASL と C の harness）、style-check 0、ASan・UBSan、run-asl、check-latitude5330、kernel-check・vmunix の build（warning 0）、
QEMU の回帰は T1（guest-events・guest-compare・boot-test）。

## 実装（2026-10-04、P1 generation16）

| 口 | 実装 |
| --- | --- |
| ① `drv_acpi_run_locked(mutex_path, work, argument)` | `aml-sync.c`。entry に入り、名前の AML mutex を entry の thread として取り（sync level の規則、`TIMEOUT_FOREVER`）、work を呼び、放して出る。work の中の `drv_acpi_evaluate` は entry の入れ子として同じ mutex を再取得できる |
| ② `drv_acpi_object_package_new()`（`aml-internal.h` から `acpi.h` へ移した）・`drv_acpi_object_package_set()` | `aml-object.c`。package は要素に自分の参照を取り、元の要素を release。範囲外は EINVAL |
| ③ `drv_acpi_notify_remove()`、install・remove を entry の中で | `aml-sync.c`。install は node・handler の NULL を EINVAL、list の変更を `drv_acpi_enter`/`leave` の中で。remove は handler と argument で探し unlink（無ければ ENOENT）。配送は次の entry を先に読むので、handler が自分を remove してよい（handler は AML を評価しない） |
| ④ `drv_acpi_resources_walk(device, method, visitor, argument)` と `struct drv_acpi_resource` | `acpi-resource.c`（新）。IO・FixedIO・IRQ（small）・Memory24・Memory32・Memory32Fixed・Word/DWord/QWord/Extended の address space（memory・IO、bus number は飛ばす）・Extended IRQ。長さの越えは EIO、visitor の停止は負の値の慣行。EC の `read_ports()` をこれに置き換えた（`port_visitor`） |
| ⑤ memory map の型を問う口 | **要否の結論: kernel に口は無く、足すには HAL の API（`hal.h`、または `hal_get_arch_handoff` の新しい名前）が要る**。memory map の型は HAL の `src/hal/amd64/bsp-pcat/boot.c` の `boot_memory_range[]`（`ZBL6_MEMORY_*`）にだけある。規則どおり実装せず Q1 に報告（WS050 p003 の前に、`UBCB` が usable・boot reclaim でないかを確かめる手段として承認を得るか、UAT の記録だけで済ませるかを判断してもらう） |

### 試験

- 新しい ASL の試験 `plan/ws049/tests/asl/api.asl`（.args・.output）: 全種類の descriptor の `_CRS` の walk（値を手で確かめた）、`\MTX0` を取った
  run_locked の中で `\TAKE` が同じ mutex を再取得、自分を remove する handler が 2 回の Notify で 1 回だけ走る、driver の作った package を `\PKG` が
  確かめる。`badcrs.asl`: 長さの足りない Memory32Fixed を EIO で拒む。harness（`aml-host.c`）に `--resources`・`--run-locked`・`--notify-once`・
  `--package-arg` を足した。
- `make -C plan/ws049/tests`（ASan・UBSan）warning 0、`run-asl.py` 20 passed、style-check total 0、kernel-check warning 0、check-latitude5330
  （1349 method、EC の port は新しい walk で 930/934）、ecdt-check 500 0 failures、`git diff --check`。
- vmunix（CI config）warning 0・vmunix check PASS。
- QEMU（T1-093、main da07422、KVM）: guest-events PASS（first SCI 1）、guest-compare namespace same・device の違い 10 項目（T1-091 と同じ）、boot-test PASS。

## 再開点

- ⑤（memory map の型を問う口、HAL の API が要る）のユーザーの判断。足すことになれば `hal.h` の差分の案を `plan/ws049/proposed/` に作り承認を得る。
- 実機の確認は WS050 p003 で（UCSI が最初の利用者）。

2026-10-05: ⑤ は不要として閉じる（`hal_space_map_device` の実装が RAM を拒むので、UCSI の driver はその error で判断できる。plan/ws050/design.md の「A3 の解決」）。
