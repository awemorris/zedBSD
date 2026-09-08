# refactor-claude-1: src/kern の再配置と統合

日付: 2026-09-08

実施: Claude（このツリーで直接作業）。**commit はしない。**

対象: `src/kern/` 以下の配置・統合・改名と、それに伴う `src/drivers/` への転出。
`plan/coding-style.md`（このツリーの版。modeline なし、goto 許容）を正とする。

## Codex への要点

1. **`src/kern` のシンボルは一切改名しない。** 配置とファイル分割だけを変える。
   `src/drivers/` へ転出したファイルのシンボルのみ `drv_` に改名する。
2. **`.inc` は 12 本すべて include 元へ吸収し、ツリーから消える。**
3. **統合ファイルに区間マーカーは残していない。** 一度 p031 と同じ
   `/* Begin consolidated ... */` で区切って統合し、意味保存を確認したのち、
   規約どおりファイル全体を並べ替えてマーカーを外した（`tools/kstyle/kreorder.py`）。
   順序は「マクロ・定数 → enum → 型 → ファイル変数 → 前方宣言 → public → static」。
   関数を名前で参照するコールバックテーブルだけは前方宣言の直後に置いている。
4. **ビルド検証は amd64 のみ**行った。`platform/{pcat,pc98,arm64,sparcv9,x68k}/vmunix.mk`
   のソース列挙も同じ規則で更新済みだが、**ビルド未検証**。そちらの確認をお願いしたい。
5. 動作確認（QEMU / 実機 / aggregate `make check`）は行っていない。

## 1. `.inc` の吸収（12 本 → 0 本、2,677 行）

| 元 | 行数 | 吸収先 |
| --- | ---: | --- |
| `vm-object-cache.inc` | 435 | `vm.c` |
| `vm-object-dirty.inc` | 64 | `vm.c` |
| `vm-object-index.inc` | 208 | `vm.c` |
| `vm-object-memory.inc` | 269 | `vm.c` |
| `vm-object-prefetch.inc` | 254 | `vm.c` |
| `vm-object-sync-batch.inc` | 211 | `vm.c` |
| `vm-object-writeback.inc` | 204 | `vm.c` |
| `vmspace-exec.inc` | 119 | `vmspace.c` |
| `buf-dirty.inc` | 121 | `buf.c` |
| `disk-async.inc` | 594 | `disk.c` |
| `disk-vector.inc` | 104 | `disk.c` |
| `file-exec-snapshot.inc` | 94 | `file.c` |

各 `.inc` は元々 1 つの `.c` からのみ include されていたため、吸収は機械的である。
唯一 `vm-object-index.inc` だけが host fixture から直接 include されていた（後述）。

## 2. `src/kern` 内の統合

| 統合先 | 吸収するファイル | 結果行数 |
| --- | --- | ---: |
| `vm.c`（新規名） | `vm-object.c` + `vm-commit.c` + `vm-lock.c` + `vm-reclaim.c` + 上記 7 `.inc` | 7,113 |
| `swap.c` | `swap-boot.c` + `swap-control.c` + `swap-format.c` + `swap-source.c` + `swap-fat.c` の生存部 | 3,662 |
| `disk.c` | `disk-async.inc` + `disk-vector.inc` | 3,257 |
| `file.c` | `file-exec-snapshot.inc` | 2,827 |
| `buf.c` | `buf-dirty.inc` | 2,000 |
| `vmspace.c` | `vmspace-exec.inc` | 4,935 |
| `writeback.c` | `writeback-domain.c` + `writeback-policy.c` | 1,508 |
| `readahead.c` | `readahead-worker.c` | 863 |
| `resource.c` | `resource-limit.c` | 370 |
| `cache.c`（新規名） | `cache-memory.c` + `cache-worker.c` | 478 |
| `io.c`（新規名） | `io-error.c` + `io-pool.c` + `io-scratch.c` + `io-stats.c` | 595 |

**名前衝突は事前に検査済みで 0 件**（static 関数・ファイル変数・マクロ・型）。
唯一 `vm-object.c` と `vm-reclaim.c` の `PAGE_SIZE` が重複するが、双方とも
`#define PAGE_SIZE ZEDBSD_PAGE_SIZE` の同一定義なので 1 つに畳んだ。

`vm.c` は「あとで分割しなおす」前提の一時的な統合である。区間マーカーを残してあるので、
分割時はマーカー境界をそのまま使える。

## 3. 改名

| 元 | 新 |
| --- | --- |
| `src/kern/process-timer.c` | `src/kern/timer.c` |
| `src/kern/posix-acl.c` | `src/kern/acl.c` |

ヘッダ（`include/kern/process-timer.h`, `include/kern/posix-acl.h`）とシンボルは変更しない。

## 4. `src/drivers/` への転出（ここだけシンボルを改名する）

| 元 | 新 | シンボル |
| --- | --- | --- |
| `src/kern/overlayfs.c` | `src/drivers/fs/overlayfs.c` | `overlayfs_init` → `drv_overlayfs_init`、`overlay_mount_at` → `drv_overlay_mount_at` |
| `src/kern/system-device.c` + `src/kern/system-swap-device.c` | `src/drivers/generic/system-device.c` | `system_device_register` → `drv_system_device_register`、`system_swap_device_ioctl` → `drv_system_swap_device_ioctl` |

**overlayfs の登録フローは変えていない。** `src/kern/vfs.c` が `overlayfs_init()` を
呼ぶ形のままで、呼び先の名前だけが `drv_overlayfs_init()` になる。
`fat`/`ufs` のように `filesystem_type` を公開して `vfs.c` が `filesystem_register` する
形へは寄せていない（フロー変更になるため）。必要なら別 phase で。

overlayfs の include は `kern/{file,inode,kmem,namecache,namei,pipe}.h` のみで、
`src/drivers/fs/fat.c` と同じ層なので転出に層違反はない。

## 5. `src/kern/net/wifi/` の新設

`src/kern/net/` の WLAN 実装を `src/kern/net/wifi/` へ移動する。
**ファイル名は `wlan*` のまま**（シンボルが `wlan_*` のため）。

```
src/kern/net/wifi/wlan.c  wlan-crypto.c/.h  wlan-frame.c  wlan-l2.c/.h
                  wlan-wpa2.c/.h  wlan-wpa2-codec.c/.h
```

## 6. 削除

`src/kern/swap-fat.c` を削除する。

- `swap_fat_activate` … **呼び出し元ゼロの死にコード**。ファイル自身の説明が
  "Compatibility adapter for the original single FAT /swapfile caller. New boot code
  prepares parameter-selected sources through swap-source.h" と書いているとおり、
  汎用 swap-source 経路へ移行済み。**削除した。**
- `swap_fat_extent_count` … `system-device.c` から現用。**`swap.c` へ移した**（シンボル不変）。
- `include/kern/swap-fat.h` は削除し、`swap_fat_extent_count` の宣言を
  `include/kern/swap.h` へ移した。

## 7. 影響を受ける host fixture（要確認）

| fixture | 影響 | 対応 |
| --- | --- | --- |
| `plan/ws025-io-memory-cache/tests/page-index-host.c` | `src/kern/vm-object-index.inc` を直接 include していた | `vm.c` の区間マーカーから抽出する方式に変更 |
| `plan/ws016-swap-control/tests/run-system-swap-device-test.sh` | `src/kern/system-swap-device.c` をコンパイルしていた | 統合先の区間から抽出する方式に変更。テスト本体の呼び出しは `drv_system_swap_device_ioctl` に追随 |
| `plan/ws001-posix/tests/credential-vfs-overlay-fault-host-test.mk` | `src/kern/overlayfs.c` のパス | `src/drivers/fs/overlayfs.c` へ更新 |
| `plan/ws001-posix/tests/directory-fsync-host-test.mk` | 同上 | 同上 |

## 8. ビルド定義

`src/kern` のソースは 6 機種の `platform/*/vmunix.mk` に**明示列挙**されている
（計 201 参照）。全機種を新しい配置に更新したが、**ビルド検証は amd64 のみ**。

## 9. 実施中に判明した事項

### vm-reclaim の weak フォールバックを削除した

`vm-reclaim.c` は `vm_object_reclaim_one` と `vm_object_page_count` の
**weak フォールバック定義**（それぞれ `ENOMEM` と `0` を返す）を持っていた。
VM オブジェクトを持たない構成のための保険だが、6 機種の `vmunix.mk` を調べたところ
**vm-* を 1 つでも持つ機種は 4 本すべてを持つ**（pc98/pcat はオブジェクト列挙側で
4 本とも持つ）。統合で同一翻訳単位になると強い定義と衝突するため、weak 側を削除した。
これが本 phase で唯一の意図的な挙動対象の削除である（実際のリンク結果は変わらない。
強い定義が常に優先されていたため）。

あわせて `vm-reclaim.c` 区間に `#include "kern/vm-object.h"` を足した。統合前は
自分の weak 定義が宣言を兼ねていたため、区間を単体抽出すると宣言が無くなるためである。

### ビルド定義は Makefile にもあった

`platform/*/vmunix.mk` だけでなく、ルート `Makefile` にも `KERN_NET_SOURCES` と
`KERN_ACL_SOURCES` があり、`src/kern/net/wlan*.c` と `src/kern/posix-acl.c` を
列挙していた。こちらも更新した。

### 既存の破損（本 phase とは無関係）

`plan/ws019-installation/tests/run-format-reservation-test.sh` は
`cache_memory_*` の未定義参照でリンクできない。**変更前のスナップショットでも
同じ失敗を再現した**ので、本 phase による退行ではない。そのまま残してある。

## 10. 検証方法

配置替えではファイル単位の比較ができないため、**関数単位の逐一比較**で確認した。
統合前の各ソースを作業前スナップショット（`build/kstyle/baseline`）からコンパイルし、
統合後のオブジェクトと関数ごとの逆アセンブルを突き合わせている
（`tools/kstyle/kmerge-verify.sh`、`__LINE__`/`__FILE__` は固定）。

先行して実施した `src/kern` のコーディング規約適用（modeline 除去 100 ファイル、
未適用 16 ファイルの pass 2、`net/` ヘッダ 6 本の説明追加）は、
**103 の `.c` すべてが amd64・i386 両方で逐関数バイト同一**であることを確認済み。

統合後の確認結果（amd64・i386 の両方）:

| 統合先 | 関数 | 欠落 | 追加 | 変化 |
| --- | ---: | ---: | ---: | ---: |
| `vm.c` | 189 | 0 | 0 | 2（削除した weak フォールバック） |
| `swap.c` | 89 | 1（削除した `swap_fat_activate`） | 0 | 0 |
| `writeback.c` | 44 | 0 | 0 | 0 |
| `readahead.c` | 27 | 0 | 0 | 0 |
| `resource.c` | 9 | 0 | 0 | 0 |
| `buf.c` / `disk.c` / `file.c` / `vmspace.c`（`.inc` 吸収） | 63 / 105 / 79 / 119 | 0 | 0 | 0 |
| `drivers/fs/overlayfs.c` | 117 | 2 | 2 | 0（`drv_` 改名のみ） |
| `drivers/generic/system-device.c` | — | — | — | 区間本文が baseline と `drv_` 改名を除いて一致 |

amd64 カーネルはビルド成功。host fixture は
`run-page-index-host.sh`、`run-system-swap-device-test.sh`、`run-swap-drain-test.sh`、
`run-swap-commit-resize-test.sh`、`run-file-cache-host.sh` を実行して PASS。

区間抽出には `tools/kstyle/kregion.sh SRC REGION...` を追加した。
`run-swap-drain-test.sh` は削除した weak フォールバックの代わりに
`plan/ws016-swap-control/tests/vm-object-stubs.c` を link する。

## 11. B 案（区間廃止・全体並べ替え）の結果と、残っている問題

統合ファイルは規約順に並べ替え済みで、区間マーカーは 0 件。ファイル説明も
統合後の実態に書き直した。amd64 ビルドと `git diff --check` は通る。
関数単位の同値確認も並べ替え後に再実行し、意図した 2 件の削除以外の差分はない。

### 区間廃止で動かせなくなった host fixture（3 本）

いずれも「統合前は 1 部分だけをコンパイルし、カーネル基本機能をホスト実装で
差し替える」設計のため、統合ユニット全体を取り込むと成立しない。

| fixture | 理由 |
| --- | --- |
| `plan/ws016-swap-control/tests/run-swap-drain-test.sh` | `vm_metadata_*` を pthread 実装で置き換えているが、`vm.c` が実体を持つため multiple definition |
| `plan/ws025-io-memory-cache/tests/run-file-cache-host.sh` | `readahead.c` が `kern/thread.h` を含むようになり、ホストの `sys/types.h` に `tid_t` がない |
| `plan/ws016-swap-control/tests/run-system-swap-device-test.sh` | driver 全体を取り込むと `kern/net/socket.h` 経由でホストの `sigset_t` と衝突 |

`vm.c` は新しい境界で分割する予定なので、分割後にこの 3 本を作り直すのが素直である。
それまでこの 3 本は失敗する。

### 動作を確認した host fixture

`run-page-index-host.sh`（統合ユニットを `#include` する形に変更）と
`run-swap-commit-resize-test.sh`（`vm.c` をコンパイル）は PASS。
`run-format-reservation-test.sh` は**本 phase 以前から**壊れている（§9 参照）。

### 残っている整形

統合ファイルに 80 桁超えのコード行が 782 行残っている（前方宣言は規約上除外）。
`vm.c` 173、`file.c` 129、`vmspace.c` 120、`disk.c` 76、`writeback.c` 66、
`buf.c` 61、`readahead.c` 60、`io.c` 32、`swap.c` 31、`cache.c` 24、`resource.c` 3。

## 12. 段落化パス（quasi-block 構造）の完了

コーディングスタイルの「意味のある段落に空行で区切り、各段落に目的コメントを置く」
規則を、`src/kern`・`src/drivers`・`src/softfloat` の全 `.c` に適用し終えた。

測定は `python3 tools/kstyle/kruns.py FILE 6`（コメントの付いていない 6 文以上の
連続を数える。コメント済みの段落、関数先頭の宣言群、goto ラベルは数えない）。

| ツリー | 開始時 | 現在 |
| --- | --- | --- |
| `src/kern` | 234（28 ファイル） | 0 |
| `src/drivers` | 114（39 ファイル） | 0 |
| `src/softfloat` | 0 | 0 |

### 検証

全ファイルを `tools/kstyle/kdelta.py` で baseline と比較した（`-Os` で関数ごとの
命令数と呼び出し先シンボルの多重集合を比較）。**すべて命令数一致・呼び出し集合の
変化なし**。既知の例外 3 件（§9 の `vm_object_content_finish` の −1 命令、
vm-reclaim の weak fallback 削除、`io.c`／`swap.c` の統合由来 inline 展開）以外に
差分はない。

`src/drivers/platform/rpi4/rpi4-sdhci.c` だけは aarch64 のインラインアセンブラを
含むため amd64 の代表コンパイル指令では kdelta にかけられない。変更はコメント 1 行の
挿入のみで、arm64 ビルドで確認されたい。

amd64 は `make ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix` が通り、
`check-amd64-vmunix` も PASS。

### 追加したツール

`tools/kstyle/kins.py`。標準入力から

    関数名;;アンカー正規表現;;出現番号;;コメント本文

の形式の指定を読み、その関数内のアンカー行の直前に空行と目的コメントを挿入する。
アンカーが 1 つでも見つからなければ何も書かずに終了するので、途中まで適用された
状態にはならない。1 回の呼び出しに 20〜35 件まとめて渡すのが実用的だった。

`syscall.c` の `syscall_dispatch_body`（447 case の switch）はサブシステム単位の
グループコメントで段落化し、`if (error != 0) return -error; return 0;` という
末尾定型 48 箇所は一括パスで処理した。
