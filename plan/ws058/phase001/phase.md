<!-- awesome-plan project=zedbsd record=ws058p001 -->

# ws058-p001: cache の上限の定数と根拠の調査、新しい上限の設計

Phase ID: `ws058-p001`
Parent: [WS058](../ws.md)
Status: cleared
Queue: q427-i01

## 目的

kernel の cache の上限（`src/kern/buf.c` の buffer cache、`src/kern/vmspace.c` の `VM_OBJECT_CACHE_OBJECTS` 32 と page cache、UFS の inode・directory の cache、その他の固定表）を列挙し、今の値・根拠・実メモリの使用量と、主記憶 4 GB・swap 16 GB を前提にした新しい値を設計する。主記憶の大きさに応じて決める（比例、上限）か固定かも決める。

## 受け入れ

- cache ごとの表（場所、今の上限、根拠、使用量、新しい上限、根拠）。
- 小さい機械（512 MiB）でも動く下限の設計。
- code は変えない（設計の Phase）。ws046-p012 と重なる VM object cache は p012 に委ねる旨を書く。

## 調査（q427-i01、2026-09-25）

guest（QEMU amd64 8 GiB）の実測: `vfs.bufcache.max_bytes` 536 MB（使用 17 MB）、`vfs.cache_memory` managed 8.58 GB・target 2.15 GB・reserve 8 MB・resident 22 MB。

| cache / 表 | 場所 | 今の上限 | 根拠・性質 | 8 GiB での値 | 新しい上限（案） |
| --- | --- | --- | --- | --- | --- |
| buffer cache（disk の block） | `src/kern/buf.c` `buf_init` | `CONFIG_BUF_CACHE_KIB`（全 config で 0 = 自動）→ 物理 / 16、下限 64 KiB | 物理に比例 | 512 MiB | 物理 / 8（下限 1 MiB、上限なし） |
| page cache の予算（file data/metadata・buffer・io pool・dma・worker の合計） | `src/kern/cache.c` `cache_memory_policy` | target = 物理 / 4、reserve（空きの床）= 物理 / 64 を 64 KiB〜8 MiB に clamp | 比例だが床が 8 MiB で固定 | target 2 GiB、床 8 MiB | target 物理 / 2、床 物理 / 64 を 1〜64 MiB に clamp |
| VM object cache（file の page cache の object） | `src/kern/vm.c` `VM_OBJECT_CACHE_OBJECTS` | **32 object**（固定） | 起動時の i386 向け。clang の library の fault で入れ替えが起きる（ws046-p011 で判明） | 32 | 1024（LRU の入れ替えは ws046-p012 の設計） |
| private page の snapshot | `vm.c` `VM_OBJECT_BACKING_SNAPSHOT_MAX` | 192（固定） | — | 192 | 1024 |
| I/O pool | `src/kern/io.c` | 物理 / 64、**上限 4 MiB** | 上限が固定 | 4 MiB | 上限 64 MiB |
| open file の表（system 全体） | `src/kern/file.c` `FILE_MAX` | **192**（固定、`.vfs_bss` の静的配列） | 極端に小さい。sshd・shell・make・build が同時に開くと ENFILE | 192 | 8192（静的配列のまま。`struct file` の大きさ × 8192 を p002 で測る） |
| inode cache | `src/kern/inode.c` `INODE_CACHE_MAX` / `INODE_COMMON_MAX` | **512** / 256（固定） | 小さい。directory を辿る負荷で入れ替え | 512 | 8192 / 4096 |
| 1 process の descriptor | `include/kern/filedesc.h` `KERN_OPEN_MAX` | 1024（固定） | POSIX の慣行どおり | 1024 | 据え置き（4096 も可） |
| buffer cache の hash | `buf.c` `BUF_HASH_BITS` | 2^13 bucket | ws046-p007 で拡大済み | 8192 | 物理 / 8 の cache に対し 2^16 |
| commit の予約 | `vm-commit.h` | 64 page | — | 256 KiB | 据え置き |

小さい機械（512 MiB）の下限: 比例のものはそのまま縮む（buffer 64 MiB、target 256 MiB）。固定の表（file 8192・inode 8192）は数 MiB なので 512 MiB でも問題ない。

## 設計（p002 で適用）

1. `buf_init`: 物理 / 8。`BUF_HASH_BITS` 16。
2. `cache_memory_policy`: target 物理 / 2、reserve の clamp を 1 MiB〜64 MiB。
3. `VM_OBJECT_CACHE_OBJECTS` 1024、`VM_OBJECT_BACKING_SNAPSHOT_MAX` 1024（object の入れ替えの LRU 化は ws046-p012 で。p002 は上限だけ）。
4. `IO_POOL_MAX_BYTES` 64 MiB。
5. `FILE_MAX` 8192、`INODE_CACHE_MAX` 8192、`INODE_COMMON_MAX` 4096（静的配列の大きさを kernel の map で確認）。
6. 受け入れ: 8 GiB の guest で `sysctl` の値が新しい上限、512 MiB の guest でも起動して sh・make の差分試験が通る。expat の configure と clang の compile・link の時間を p009 の値（96 秒・1.0 秒）と比べる。

## 受け入れの判定

表・根拠・新しい上限・小さい機械の下限を書いた。code は変えていない。q427-i01 は **cleared**。
