<!-- awesome-plan project=zedbsd record=ws061p002 -->

# ws061-p002: page fault の固定費用と process 終了の page table の解体を下げる

Phase ID: `ws061-p002`
Parent: [WS061](../ws.md)
Status: uncleared
Queue: q433-i01（uncleared）

## 目的

fault の経路（`vmspace_fault` → `fill_file_page`/anon、HAL の map、rmap、index、lock、page の確保）と exit の `hal_space_unmap`/`detach_empty_tables` を標本で分け、固定費用を下げる。目標: file の fault 16.7 → 数 µs、anon 9.5 → 数 µs、`true` 11 ms → 3 ms 以下。

## 受け入れ

上の目標の数字を guest で測る。回帰: boot、sh・make の差分試験、`SMP-STRESS.ELF`、COW の試験、`ffault`。規約。HAL の変更は差分ごとの事前承認（p002 の HAL の map/unmap の変更は承認待ちの差分として plan に置く）。

## 結果（q433-i01、2026-09-25）: uncleared（anon の目標は達成、file の fault と `true` は未達。残りは p003 へ）

### 変更（commit b055d493 と次の WIP）

| 変更 | 場所 | 効果 |
| --- | --- | --- |
| kcrt の `memcpy`/`memmove`/`memset` を、両方の address が word 境界なら 8 byte ずつ（byte の loop のまま、`__may_alias__` の word） | `src/kern/kcrt.c` | page の複写・0 埋め（fault ごとの 4 KiB）が 1/8 の命令数 |
| VM の page・private page の metadata の slab: 解放は record の address から slab（page 境界）を直接求める（全 slab の線形探索をやめる）。空きのある slab だけを双方向 list に | `src/kern/vmspace.c` | `vm_page_slab_take_locked`・`vm_private_page_slab_take_locked` が標本から消えた |
| amd64 の page table の owner PTE の bit 52〜62 に子 table の present entry の数。`hal_space_unmap` の空 table の判定を O(1) に（`detach_empty_tables` の全 table × 512 の走査をやめる） | `src/hal/amd64/space.c`（hal.h は不変） | exit の解体の 13% が消えた |
| VM object の registry に inode の hash（1024 bucket）を足し、inode で探す 4 箇所を hash に。registry の list を双方向にして unlink を O(1) に | `src/kern/vm.c`、`include/kern/vm-object.h` | file の read・fault のたびの `vm_object_get_shared_internal`（割り込み禁止で全 object を線形探索、configure の kernel 標本の leaf の首位）が消えた。**configure 59 → 37 秒（tmpfs）** |
| amd64: page table の walk（`walk_leaf`）で table を direct map の offset で得る（RAM map の 4 段の検証 walk をやめる）。`hal_pmem_to_kernel` は起動時に direct map が全 page を覆うと確かめた managed extent の中なら offset で返す（外は従来の検証）。`table_owner_of` は物理 address で比べる | `src/hal/amd64/space.c`・`page.c`・`space.h` | 1 回の変換あたり 16 回ほどの memory 読みが消えた。configure 37 → 35 秒 |
| 1 tick ごとの `process_itimer_real_tick_all` を、process の数の 2 乗（`process_find_next_ref` の繰り返し）から 1 回の走査に（armed な process が 16 を超えるときだけ従来の walk） | `src/kern/process.c` | CPU 0 の tick と process tree の lock の競合が減る。configure の時間の差は雑音の範囲 |

HAL の実装の変更（上の 2 行）は 2026-09-25 のユーザーの指示「HALの実装は勝手に修正してください。APIの変更のみ許可が必要です」で承認不要になった（Guardrail）。hal.h は変えていない。

### 測定（QEMU、amd64 guest 8 GiB、同じ session で HEAD 21866849 と比べる）

| 測定 | HEAD | p002 の後 | p002 の目標 |
| --- | --- | --- | --- |
| kbench anon fault | 7.8〜8.8 µs | 3.8〜4.7 µs | 数 µs（達成） |
| kbench file fault | 12.2〜15.0 µs | 6.7〜10.1 µs | 数 µs（未達） |
| `ffault libLLVM` | 13.0〜14.7 µs/page | 9.4〜10.7 µs/page | |
| `true` | 8 ms | 4〜5 ms | 3 ms 以下（未達） |
| `sh -c true` | 13〜16 ms | 9〜15 ms | |
| `clang --version` | 158〜166 ms | 80〜102 ms | |
| `ld.lld --version` | 125〜126 ms | 50〜70 ms | |
| fork+exec true+wait | 4.9〜6.5 ms | 3.8〜4.6 ms | |
| `cc t.c -o t` | （p001: 578〜652 ms） | 344〜426 ms | |
| expat の configure（tmpfs） | 59 秒 | **35〜38 秒** | |
| expat の configure（root の overlay、USB） | 87 秒 | **59〜64 秒** | |

fault の数は変わらない（`clang --version` 5797、`true` 307）。

### 残り（次の Phase へ）

- `cc t.c -o t` は 18365 fault、kernel の時間 0.32 秒（user は 0.04 秒）: 1 fault あたり約 17 µs。file の fault は file の page を private の page に読み写す（`fill_file_page` → `file_io_once` → `memcpy`）。標本は平らになり、単独の hotspot は無い。数を減らす p003（fault-around）と、写さずに map する ws046-p014 が次。
- overlay（USB の boot disk の `data.img` の loop 上の UFS）では configure が 22〜26 秒余計にかかる: UFS の metadata の同期書き（`write_cg`・`free_block`・`ufs_truncate`）が loop を経て FAT の file の書き（`fat_pwrite`）と USB の flush（`loop_submit` → `file_fsync_backend` → `fat_fsync` → `bio_flush`）になる。p004 を立てた。
- `SMP-STRESS.ELF`（静的 link）は HEAD でも 12 回に 1 回 status 7（BUG-045 の類）。p002 の後は 6/6・3/3・6/6（1 回 7）。libc に link した版は 8/8。

### 回帰（最終 image、QEMU）

| 試験 | 結果 |
| --- | --- |
| build | amd64 guest image warning 0、rpi4・pcat・pc98 の kernel diag 0（kcrt・slab の後） |
| boot | `plan/tools/boot-test.sh` PASS（login prompt、`build/boot-test-amd64-q433/login.png`） |
| make の差分試験 | 91/91（kcrt・slab・HAL の後と、最終 image） |
| sh の差分試験 | 1388/1425（kcrt・slab・HAL の後と、最終 image）。以前との差は 2 件: glob の 1 件が通り、`noclobber on &> >` が落ちた。後者は `echo baz &` の background の出力の順序に依存し、guest で 10 回走らせると 4 通りの出力になる（試験の側の競走） |
| `SMP-STRESS.ELF` | 6/6（最終 image） |
| COW の試験 | OK |
| itimer の試験（`alarm`、10 ms 周期 20 回、24 process 同時） | OK（変更の前後で同じ） |
| 規約 | 変えた行に `style-check.py` の指摘 0 |

実機: 未実施。
