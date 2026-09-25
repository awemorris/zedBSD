<!-- awesome-plan project=zedbsd record=ws061p001 -->

# ws061-p001: host の参照値と guest の 1 check の費用の内訳

Phase ID: `ws061-p001`
Parent: [WS061](../ws.md)
Status: cleared
Queue: q432-i01

## 目的

1. host（Linux、同じ `build/llvm/bin/clang`）で expat の configure・`cc t.c -o t`・`cc t.o -o t`・`sh -c true` の時間を測り、目標を数字にする。
2. guest で同じものを測り、1 check（sh → clang → ld.lld）の費用を、process の生成（fork+exec）、exec と動的 link（libLLVM・libclang-cpp の relocation）、page fault、I/O、syscall に分ける（kbench、`ffault`、gdbstub の標本（non-LTO の kernel））。
3. 上位の費用ごとに次の Phase を切る（候補: ws046-p014 の object の page の直接 map、fork+exec の費用、動的 link の relocation、syscall の経路）。

## 受け入れ

- host と guest の表、費用の内訳の表（標本の割合）、次の Phase の一覧。code は変えない。

## 結果（q432-i01、2026-09-25）

### host の参照値（Linux、64 core、同じ `build/llvm/bin/clang`、`/tmp/expat-g.tar`）

| 測定 | host | guest（ws046-p012 の後、8 GiB） | 比 |
| --- | --- | --- | --- |
| `cc t.c -o t`（warm） | 83〜92 ms | 636〜707 ms | 7〜8 倍 |
| `cc t.o -o t`（link） | 48〜57 ms | 361〜379 ms | 7 倍 |
| `sh -c true` | 4〜6 ms | （下） | |
| `clang --version` | 23 ms | （下） | |
| expat の configure | **11 秒**（145 check、`./configure CC=clang`） | **91 秒**（162 check） | 8 倍 |

目標（WS の受け入れ）: host の 2 倍以内 = configure 22 秒、`cc t.c -o t` 180 ms。

### guest の内訳（non-LTO の kernel、8 GiB、warm）

| 測定 | guest | host | fault の数（`KERN_SYSTEM_GET_VMSTAT` の差、page_in 0） |
| --- | --- | --- | --- |
| `true` | 11 ms | 〜1 ms | 306 |
| `sh -c true` | 20〜27 ms | 4〜6 ms | 448 |
| `clang --version` | 118〜203 ms | 23 ms | **5797** |
| `ld.lld --version` | 94〜137 ms | 〜20 ms | **4547** |
| `cc -c t.c` | 181〜243 ms | 〜40 ms | |
| `cc t.o -o t`（link） | 341〜365 ms | 48〜57 ms | |
| `cc t.c -o t` | 578〜652 ms | 83〜92 ms | |
| kbench | getppid 615 ns、pipe 55 µs、fork+exit 1.07 ms、fork+exec 7.2 ms、anon fault 9.5 µs、file fault 16.7 µs | | |

- **全て page fault に律速**: 1 process の時間 ≈ fault の数 × 約 30 µs（`clang --version` 188 ms / 5797、`true` 11 ms / 306）。1 check は sh → clang → ld.lld で約 1 万 fault = 約 0.3〜0.4 秒。Linux は 1 fault 約 1 µs で、file の fault では隣の 16 page もまとめて map する（fault-around）。
- loader の仕事: libLLVM は RELATIVE 59474・.rela.dyn 1.56 MB・symbol 4 万、libclang-cpp は RELATIVE 167196・.rela.dyn 4.08 MB・symbol 4.5 万、`BIND_NOW`。.gnu.hash はある。`src/rtld/rtld.c` の relocation の loop は relocation ごとに余計な事はしていない（page ごとの `mprotect` は SPARC の PLT だけ）。relocation の表（5.6 MB = 1400 page）を読む fault と、書き先の data の page の COW の fault が数千。
- 従って次の Phase（fault を減らす・安くする）:
  1. **p002: page fault の固定費用**（30 µs → 数 µs）。fault の経路を標本で分けて直す（lock、page の確保、HAL の map、TLB、rmap、index）。
  2. **p003: fault-around**（file の fault で object に resident な隣の page をまとめて map、Linux の 16 page）。fault の数を 1/10 に。
  3. ws046-p014（object の page を直接 map、COW）: 複写を無くす。
  4. process の生成（fork+exec 7.2 ms、`true` の 306 fault: ld.so・libc の起動）は 1〜3 で大半が消える見込み。残れば p004。

### configure 中の kernel の標本（non-LTO、gdbstub、150 回、busy 169 = user 12 / kernel 157。expat は root の overlay = USB の boot disk 上）

| 関数（inclusive） | 標本 | 意味 |
| --- | --- | --- |
| `vmspace_fault` / `kernel_user_fault_handler` | 35 / 34（22%） | page fault の経路 |
| `fill_file_page` 18、`memcpy` 15（leaf 15） | | file の page の複写 |
| `hal_space_unmap` 20、`detach_empty_tables` 17（leaf 15） | 13% | process の終了時の page table の解体（全 table を歩く） |
| `kernel_large_allocations` 14 | 9% | kernel の大きい確保（fault 中の metadata か） |
| `storage_submit`/`bot_command_*`/`drv_usb_urb_wait` 33、`fat_fsync`/`loop_submit`/`bio_flush`/`file_fsync_backend` 21 | **20%** | USB の boot disk（data.img の loop）への同期の書き出し。configure の file の書き込みが USB の flush を待つ |
| `xhci_irq` 13 | 8% | USB の割り込み |

leaf: `memcpy` 15、`detach_empty_tables` 15、`xhci_irq` 10、`spin_trylock` 9、`vm_object_get_shared_internal` 6、`amd64_percpu_current` 5、`vm_object_cache_pin` 5、`asm_cli` 5。

### 受け入れの判定と次の Phase

表と内訳と次の Phase を書いた。code は変えていない。q432-i01 は **cleared**。

| 次 | 内容 | 根拠 |
| --- | --- | --- |
| ws061-p002 | page fault の固定費用（約 30 µs/fault）と process 終了の page table の解体（`detach_empty_tables`）を profile して直す | fault × 30 µs が process の時間の大半。exit の解体が 13% |
| ws061-p003 | fault-around（file の fault で resident な隣の page をまとめて map） | fault の数を 1/10 に |
| ws046-p014 | object の page を直接 map（複写を無くす） | `fill_file_page`・`memcpy` |
| ws061-p004（案） | root の overlay（USB）への書き込みの同期 flush（`file_fsync_backend` → `fat_fsync`）が configure の 20% | 誰が fsync するか（sh の redirect か、UFS の loop の write-through か）を調べる |
