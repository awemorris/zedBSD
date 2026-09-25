<!-- awesome-plan project=zedbsd record=ws061p007 -->

# ws061-p007: configure の残りの kernel の時間（readdir の読み、fault の待ちの走査）

Phase ID: `ws061-p007`
Parent: [WS061](../ws.md)
Status: cleared
Queue: q442-i01（cleared）
Disposition: normal

## 目的

2026-09-26 ユーザー指示「configureがホストとほぼ同等の性能になるまで、全般的な最適化を行なってください」。journal を既定にした後の configure（`/root`）13.0 秒（host 10.9〜11.2 秒）の差を詰める。

## 調べたこと（QEMU 8 GiB 4 vCPU NVMe、root に journal）

- configure の子の時間: guest user 5.13 秒・system 8.26 秒、host user 6.2 秒・system 5.8 秒。差は kernel。
- host の `perf kvm --guest -e cycles:G`: `spin_trylock` 8.9%（奪い合いは 2% 程度、残りは lock の cache line の往復）、`buf_read` 4.3%、`vmspace_wait_faults_locked` 3.1%、`amd64_syscall_entry` 2.9%。
- `clang --version` 1 回で UFS の読み 1,177 回・9.7 MB（cache が温まっていても）。gdb で `buf_read` の呼び出し元を 100 回たどると全部 `ufs_readdir` → `next_dirent` → `pread_inode`。readdir が 1 項目ごとに見出しと名前の 2 回、8 KiB の block を一時領域に丸ごと読んでいた。
- `vmspace_wait_faults_locked`（fork などで fault の途中が無いかを待つ）が全 region の全 page の list を毎回歩いていた。

## 変更

| # | 変更 | 場所 |
| --- | --- | --- |
| 1 | `pread_inode` の部分の読みは、覆う sector（4 sector まで）だけを stack に読む（`pread_edge`）。file の穴と大きな部分は従来の経路 | `src/drivers/fs/ufs.c` |
| 2 | `next_dirent` は見出しと名前を 1 回の読み（directory の塊の残り、最大 263 byte）で取る | 同上 |
| 3 | `vmspace_wait_faults_locked` は region の hold だけを見る（`BUSY` の mapping は必ず region を hold している。全 `VM_MAPPING_BUSY` の設定箇所で確かめた） | `src/kern/vmspace.c` |

## 結果（2026-09-26、cleared）

| 測定 | 前 | 後 | host |
| --- | --- | --- | --- |
| `clang --version` 1 回の UFS の読み | 1,177 回・9.7 MB | 635 回・0.94 MB | — |
| configure（`/root`、journal） | 13.0〜13.2 秒 | **11.46〜11.58 秒** | 10.9〜11.2 秒 |
| configure（tmpfs） | — | 11.04 秒 | 同上 |
| configure の子の user / system | 5.13 / 8.26 秒 | 4.72 / 6.33 秒 | 6.2 / 5.8 秒 |
| make（直列、`/root`） | 20.0 秒 | 17.7 秒 | `-j1` 15.2 秒 |

回帰: boot test PASS（`build/boot-test-amd64-p007/login.png`）、make の差分試験 91/91（8 GiB）、COW、`dir-grow.sh`（root）VERIFY-OK、sh の差分試験 1389/1425（新しい FAIL 1 件 `noclobber on &> >` は `echo baz &` の背景の job と後の出力の順が時間で揺れる case。単独で走らせると期待どおり）。

残りの候補: system call が `int 0xc2`（割り込みの門）で入る費用（`amd64_syscall_entry` の先頭の命令に 4.6%）を `syscall`/`sysret` にする（HAL の実装と libc の呼び出し）。lock の cache line の往復（全体で 1 つの lock を per-CPU に）。並列の make（F-016）。QEMU だけ、実機は未実施。
