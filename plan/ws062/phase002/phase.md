<!-- awesome-plan project=zedbsd record=ws062p002 -->

# ws062-p002: native の image での harness・swap・性能・回帰

Phase ID: `ws062-p002`
Parent: [WS062](../ws.md)
Status: cleared
Queue: q437-i01（cleared）

## 目的

native の image で `guest.py`（SSH）、`serial.py`、swap の使用（512 MiB の guest で page out）、expat の configure を `/root` と `/tmp` で測る。native root の UFS の書き込みの費用（同期の metadata、BUG-040）を標本で見て、tmpfs の +20% に届かなければ原因を記録して直す。

## 受け入れ

configure（`/root`）が tmpfs の +20% 以内、または届かない原因の記録と次の Phase。sh・make の差分試験、`SMP-STRESS.ELF`、COW、512 MiB の make の差分試験。

## 進行の記録（q437-i01、2026-09-25）

### 方針の変更（ユーザー）

- 「tmpfs は RAM disk なので、比較するなら Linux host での実行時間が妥当」「USB ではなく NVMe で起動して計測する」「NVMe の emulation が遅さの原因なら virtio の disk を実装してもよい」「root 4 GB・swap 4 GB で試験」。
- overlay（USB 上の loop の UFS）の同期書きの調査（ws061-p004）は、この方針で不要になった。

### 変更

| 変更 | 場所 |
| --- | --- |
| harness の NVMe 起動: `guest.py start --disk nvme`（既定は usb のまま。p003 で既定を変える）、`boot-test.sh` の `BOOT_MODE=uefi-nvme` | `plan/tools/guest/guest.py`、`plan/tools/boot-test.sh` |
| UFS の image の producer の上限を 2 GiB から 16 GiB に（cylinder group の最大 64 → 512、inode の最大 16384 → 65536。1 group は bitmap が 1 block に収まる約 62 MiB） | `tools/build/zedimage-host.c` |
| Noct の整数は 32 bit なので、4 GiB の byte 数が 0 になる。UFS の大きさは MiB 単位（`4096M`）で backend に渡し（`ufsBuildProfileInodesMiB`）、swap の image は header の 64 bit の大きさを 32 bit ずつ書いて `truncate -s NM` で伸ばす | `tools/build/ufs_format.noct`、`make-arch-overlay-ufs.noct`、`make-swapfile.noct`、`zedimage-host.c`（`M` の接尾辞） |
| native の既定を root 4096 MiB・inode 65536・swap 4096 MiB に | `platform/amd64/vmunix.mk` |

image は見かけ 8.7 GB、実 487 MB（sparse）。NVMe で起動して root は `/dev/nvme0n1p2`（4 GiB、空き 3.9 GB）、swap は 4 GiB（1,048,575 slot）。

### 測定（QEMU、8 GiB、NVMe、2 GiB の root の image）

| 測定 | 結果 |
| --- | --- |
| expat の configure、tmpfs | 30 秒（子の user 7.6 秒・system 21.2 秒、fault 53.7 万） |
| expat の configure、NVMe の UFS root | 41 秒（user 7.7 秒・system 27.9 秒） |
| `cc t.c -o t` | 217〜242 ms |
| `clang --version` / `true` | 55 ms / 7 ms |

host（Linux、64 core、同じ clang）の configure は 11 秒。差の本体は **kernel の system 時間**（tmpfs でも 21 秒。fault は 1 fault 2〜4 µs で 1〜2 秒分）で、残りは process の生成・終了と system call。UFS の root ではさらに +7 秒（同期の metadata の書き）。

### 4 GiB の root・4 GiB の swap の image（`build/ws062-native-4g.img`、NVMe、8 GiB）

| 測定 | 結果 |
| --- | --- |
| expat の configure、`/tmp`（tmpfs） | 29 秒（user 8.5 秒・system 19.8 秒） |
| expat の configure、`/root`（UFS） | 35 秒（user 7.8 秒・system 21.2 秒）。tmpfs の +21% |
| expat の `make -j4`、`/root` | 34 秒（user 15.2 秒・system 14.2 秒）。host 6.0 秒 |
| `true` 1 回（sh のループ、4 vCPU） | 3.43 ms（sh の system 0.94 ms、true の system 1.76 ms、user 0.7 ms）。1 vCPU の guest では 1.0 ms、host 0.5 ms |

kernel heap の `kern_free` が O(n)（`kern_heap_owner` が block の連鎖を全部歩く）だったのを O(1) に（前後の block の link の整合だけ確かめる。全部歩くのは `KERN_KERNEL_HEAP_TRACE` のときだけ。`src/kern/heap.c`）。

### BUG-053（RAM を超える anonymous memory）

この Phase の swap の試験で見つけた BUG-053 を直した（原因 2 つ。[BUG-053](../../bugs/BUG-053.md) の表。commit 5a32f4e7）。受け入れ: 512 MiB の guest で swaphog 450 MiB 15 秒・900 MiB 96 秒（NVMe）が正常終了、USB は 58 秒・151 秒。1300 MiB は 35 分で終わらず未確認（BUG-053 に残りとして記録）。

### UFS の root の書き込みの費用（受け入れの「原因の記録」）

UFS の書き込みは全部 write-through（`buf_write_context` は dirty にしてすぐ `buf_writeback_context`）で、UFS は順序のために `disk_sync`（buf_sync + `bio_flush` の device flush）を 11 か所（bmap_ensure、allocation_run_abort/write_run、detach_inode_block、truncate_indirect、dir_add_finish、journal_flush）で呼ぶ。configure（/root）の tmpfs との差 6 秒は主にこの待ち（system +1.4 秒）。buffer cache は dirty list と `buf_writeback` を持つので、flusher を足せば delayed write にできる（耐久性の方針の判断が要る。Future Work）。

### 残りの原因（configure の kernel の時間）と次の Phase

host の `perf`（guest の cycles を vmunix の symbol に解決）と KVM の exit 統計で、configure の時間の本体は process の生成・終了の固定費用（`true` 1 回 3.4 ms、host 0.5 ms）で、その 7 割は fork した子を別 CPU に置くことによる HLT・IPI・cache の冷えと、解放した page を使い回さない allocator（QEMU の EPT violation 33k/秒）だと分かった。詳細と修正は [ws061-p005](../../ws061/phase005/phase.md)。

### 回帰（8 GiB、この Phase の image、2026-09-25）

make の差分試験 91/91、`SMP-STRESS.ELF` 6/6、COW、itimer、sh の差分試験 1388/1425（既知の 2 件）、boot test PASS（`build/boot-test-amd64-bug053/login.png`）。512 MiB の make の差分試験は p005 の image で実施（下の結果）。

### 結果（2026-09-25、cleared）

受け入れの「configure（`/root`）が tmpfs の +20% 以内、または届かない原因の記録と次の Phase」のうち、後者で cleared。

- 最終の image（ws061-p005 の変更を含む）で configure は tmpfs 12.8 秒、`/root`（UFS）23.1〜25.3 秒。差の 10〜12 秒は UFS の書き込みが全部 write-through で、順序のための `disk_sync`（device flush）を 11 か所で待つこと（上の「UFS の root の書き込みの費用」）。
- 直すには delayed write（flusher）が要り、電源断で失う範囲の方針はユーザーの判断（[F-015](../../future-work.md)）。判断待ちとして次の Phase の候補に置く。
- 回帰（最終の image、ws061-p005 の記録）: boot test PASS（NVMe）、make の差分試験 91/91（8 GiB・512 MiB）、SMP 6/6、COW、itimer、swaphog 450 MiB（512 MiB）、sh の差分試験（ws061-p005 の記録）。
- 実機は未実施。
