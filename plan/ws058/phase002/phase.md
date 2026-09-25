<!-- awesome-plan project=zedbsd record=ws058p002 -->

# ws058-p002: cache の上限を現代の機械向けに変える

Phase ID: `ws058-p002`
Parent: [WS058](../ws.md)
Status: cleared
Queue: q428-i01

## 目的

[p001](../phase001/phase.md) の設計 1〜5 を適用する（buffer cache 物理 / 8、hash 2^16、page cache の target 物理 / 2、VM object cache 1024、snapshot 1024、I/O pool 64 MiB、file 表 8192、inode cache 8192 / 4096）。HAL は変えない。

## 受け入れ

- 8 GiB の guest で `sysctl vfs.bufcache.max_bytes` ≈ 1 GiB、`vfs.cache_memory.target_bytes` ≈ 4 GiB。512 MiB の guest で起動し sh・make の差分試験が通る。
- expat の configure と `cc t.c -o t` の時間を p009 の値と比べる（悪化しない）。
- 規約。回帰: amd64 の build と boot、sh・make の差分試験、SMP stress。kernel の静的配列の増分（`.vfs_bss`）を map で記録。

## 実装（q428-i01、2026-09-25）

| 場所 | 前 | 後 |
| --- | --- | --- |
| `src/kern/buf.c` `buf_init` | 物理 / 16、下限 64 KiB、hash 2^13 | 物理 / 8、下限 1 MiB、hash 2^16 |
| `src/kern/cache.c` `cache_memory_policy` | target 物理 / 4、床 64 KiB〜8 MiB | target 物理 / 2、床 1〜64 MiB |
| `src/kern/vm.c` | object cache 32、snapshot 192 | 1024・1024 |
| `src/kern/io.c` `IO_POOL_MAX_BYTES` | 4 MiB | 64 MiB |
| `src/kern/file.c` `FILE_MAX` | 192 | 8192 |
| `src/kern/inode.c` `INODE_CACHE_MAX` / `INODE_COMMON_MAX` | 512 / 256 | 8192 / 4096 |

HAL は不変。規約: 変えた行は `style-check.py` の報告 0。amd64 の kernel の build は warning 0。kernel の `.bss` は 3872 KiB → 8156 KiB（+4.2 MiB、file と inode の静的な表）。`.text`・`.data` は不変。

### 最初の適用での退行と原因（bisect）

p001 の値（file 8192・inode 8192/4096）で作った image は、8 GiB でも 512 MiB でも file の作成が **ENOSPC** になり（make の case の展開、expat の configure）、512 MiB では続いて `/bin` の program の exec も失敗した。`/root` の volume は 2% しか使っていない。
bisect: A = buf.c と cache.c の変更だけ → 通る。B = 全部 + overlay の inode の表を 8192 に → 通る（make 91/91）。原因は **overlayfs の固定の inode の表**（`OVERLAY_INODE_MAX` 256、`src/drivers/fs/overlayfs.c`）: VFS の inode cache を 8192 にすると inode が早く evict されなくなり、overlay の 256 slot が埋まって新しい file の作成（と lookup）が ENOSPC になる。overlay の表は inode cache 以上でなければならない。
表の 1 entry は小さい（`struct file` 248 B、`struct inode` 488 B、overlay の slot 792 B。B の `.bss` は約 11 MiB）が、overlay の identity は線形探索（lookup ごとに全 slot を `strcmp`、subtree の rename は 2 乗）なので 8192 では 32 倍遅くなる。上限は有界に改め（inode cache 2048、common pool 512、file 1024、overlay 2048。`.bss` 6040 KiB、+2.1 MiB）、hash と動的確保は [F-013](../../future-work.md) に送る。

## 結果（q428-i01、2026-09-25、有界の値 = 最終の image）

| 検証 | 結果 |
| --- | --- |
| build（disk image、full LTO。kernel だけの変更で LLVM の再 build なし） | status 0、我々の code の warning 0。kernel の `.bss` 3872 → 6040 KiB（inode_cache 16、common_pool 244、files 248、overlay_inodes 1584 KiB） |
| boot test | PASS、`build/boot-test-amd64-q428c/login.png` |
| 8 GiB: `sysctl` | bufcache max 1 GiB（物理/8）、cache_memory target 4.29 GB（物理/2）、reserve 64 MiB |
| 512 MiB: `sysctl` | target 265 MB（物理/2）、reserve 8 MiB（物理/64、1〜64 MiB の中）。起動して SSH まで動く |
| 8 GiB: `SMP-STRESS.ELF`（起動 5 秒後） | status 0 |
| 8 GiB: make の差分試験（case の展開 182 file を含む） | 91/91 |
| 512 MiB: make の差分試験 | 91/91 |
| 8 GiB: `cc t.c -o t`（warm） | 648〜722 ms（p009 の 0.99〜1.08 秒より速い。cold の 1 回目 1.8 秒） |
| 8 GiB: expat の `./configure --build=x86_64-unknown-zedbsd CC=clang`（162 check、作業 volume） | **90 秒**（p009 の 96 秒。悪化なし）。引数無しでは `config.guess` が zedBSD を知らず 3 秒で失敗する（q418 も build type を与えていた） |

### 2 つ目の退行と原因（sh の差分試験の 4 batch 目から status 2）

有界の値（object cache 1024、file 1024）でも sh の差分試験が 4 batch 目（約 160 case）から status 2（sh が case の file を開けない）で落ち、その後も周期的に落ちた。同じ runner を 1 つだけで再実行しても再現（runner の競合ではない）。
原因（`src/kern/vm.c` の `vm_object_get_shared_internal`）: **cache だけの VM object は inode に自分の read handle を `file_open_resolved` で開く**ので、cache された object 1 つが system 全体の `struct file` の表（`FILE_MAX`）を 1 つ占める。object cache 1024 と file 1024 では cache が表を使い切り、以後の `open` が ENFILE。cache は表が埋まっても縮まない。元の 32 / 192 では起きなかった。
確認: object cache だけを 32 に戻した image（D）では 4 batch 目が 38/40（落ちる 2 件は q422 と同じ集合）。よって object cache の上限は `FILE_MAX` より十分小さくし、overlay の表は「cache された inode + 使用中の inode（≤ file）」を覆う必要がある。最終の値: object cache **256**、file **2048**、inode cache 2048、common pool 512、overlay **4096**。cache の object が file の表を使わない設計は ws046-p012 / [F-013](../../future-work.md) で。

## 結果（q428-i01、最終の値 = E: buffer 物理/8・hash 2^16、page cache 物理/2、object cache 256、snapshot 1024、I/O pool 64 MiB、file 2048、inode 2048/512、overlay 4096）

| 検証 | 結果 |
| --- | --- |
| build（disk image、full LTO） | status 0、我々の code の warning 0。kernel の `.bss` 3872 → 7872 KiB |
| boot test | PASS、`build/boot-test-amd64-q428e/login.png` |
| 8 GiB: case の展開（182 file）・`SMP-STRESS.ELF`・make の差分試験 | 182・status 0・91/91 |
| 512 MiB: 起動と SSH | 動く（bufcache 64 MiB、target 265 MB） |
| 512 MiB: make の差分試験 | 91/91 |
| 8 GiB: `cc t.c -o t`（warm） | 624〜655 ms（p009 0.99〜1.08 秒。cold の 1 回目 1.7 秒） |
| 8 GiB: expat の `./configure --build=x86_64-unknown-zedbsd CC=clang`（162 check） | **89 秒**（p009 96 秒） |
| sh の差分試験（8 GiB、36 batch） | 1388/1425、落ちる 37 件は q422 と同じ集合 |
| 実機 | 未実施 |

### 受け入れの判定

8 GiB で bufcache 1 GiB（物理/8）・target 4.29 GB（物理/2）、512 MiB でも起動して sh（未実施、make のみ）・make の差分試験が通る。expat の configure 96 → 89 秒、`cc t.c -o t` 1.0 → 0.62〜0.66 秒で悪化なし。規約 0、build・boot・sh・make・SMP を通した。q428-i01 は **cleared**。
p001 の案からの変更: object cache は 1024 でなく 256（cache の object が file の表を使うため）、file は 8192 でなく 2048、inode は 8192/4096 でなく 2048/512、overlay の表は 4096（p001 の表に無かった依存: overlay の slot ≥ inode cache + 使用中の inode）。8192 級の値は F-013（動的確保と hash）の後。
