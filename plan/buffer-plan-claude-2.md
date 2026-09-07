# buffer-plan-codex-1.md のレビューと、その先の段階設計

日付: 2026-09-07
照合対象: `/home/awe/zedBSD` HEAD `dd4f31c`
対象: [buffer-plan-codex-1.md](buffer-plan-codex-1.md)（以下「Codex 案」）
範囲: 機能性（性能・安定性）のみ。セキュリティは扱わない。
方法: 静的レビュー。ビルド・実行・計測はしていない。

構成:
- **第 I 部** Codex 案のレビュー（採否と修正点）
- **第 II 部** Codex 案が §13 で「意図的に扱わない」とした機能は必要か
- **第 III 部** それらの段階設計 — インタフェースのスタブから順に入れられる形で

---

# 第 I 部 Codex 案のレビュー

## I-0. 結論

**採用してよい。** 原案（buffer-plan.md）より正確で、特に次の 3 点は原案の不足を正しく補っている。

1. UFS が 8 KiB block ごとに `disk_write()` を呼ぶため、buffer cache だけでは 64 KiB が 8 つに割れる
   → 対策 C（UFS data run）を追加した。
2. `buf_write()` の入力は呼び出し中ずっと有効な連続領域なので、staging buffer は不要
   → 原案 §3.3 の「連続バッファへ寄せる」案より良い。loop 再入時の自己待ちも消える。
3. USB core の staging と xHCI の DMA bounce は別物で、両方の寿命を固定しないと HCD 内の動的確保が残る
   → 対策 E の二段 reservation。

「計測 counter を先に入れる（段階 0）」「`cache_dirty_bytes` が呼び出し前と同じ、という受け入れ条件は
採らない」「IMOD は実機測定まで動かさない」も正しい。

修正を求める点は 3 件、補足が 4 件。

| # | 箇所 | 要旨 | 重さ |
| --- | --- | --- | --- |
| F1 | §4.1 pool の要素数 | `min(CPU, 64, …)` は 4 CPU のノートで 4 本。5 本目の並行 I/O が **512 B stack path（旧最悪経路）へ落ちる** | 修正 |
| F2 | §6.2 / §13 | 新規ファイル・追記は allocation metadata が支配的で、この計画では **64 KiB あたり約 33 コマンドのまま**。`cp`・`tar`・install がこの経路 | 修正（次段の優先順位） |
| F3 | §10 | 自分が起こした reset 直後の `06/28` を無条件 REVALIDATE にすると、mounted root は永久に REVALIDATE = 停止 | 修正 |
| F4 | §5.3 / exec | `copy_segment_snapshot()` は PAGE_SIZE bounce のままなので、exec は 4 KiB read のまま | 補足 |
| F5 | §2 / §5.4 | `buf_reclaim()` には呼び出し元が無い（ヘッダのみ）。reclaim と cache が繋がっていない事実を段階 0 で記録 | 補足 |
| F6 | §8.2 | reservation は 64 KiB × 2 URB の物理連続 DMA。attach 時 1 回なら可。数量を counter に | 補足 |
| F7 | §12 | 段階 1 と 2 は独立、3 は 2 に依存、4 は独立。並行 Queue 可 | 補足 |

## I-1. F1: pool 要素数と fallback 先

§4.1 の式 `min(online CPU 数, 64, floor(RAM / 64 / 64 KiB))` は、Latitude 5320（4C/8T）で
4〜8 本になる。`file_io_begin()` 後に try-borrow して **空なら 512 B stack buffer** に落ちる設計なので、
並行 I/O が要素数を超えた瞬間から、その syscall はレポート2 §13 の「512 B ごとに FS 呼び出し」
に戻る。`cp -r` を 1 本、`tar` を 1 本、`getty`/`networkd`/`syslog` の小書き込みが数本、で普通に超える。

修正案:

- 要素数の下限を **16**（`max(16, 2 × CPU)`、上限は RAM 制約のまま）。64 KiB × 16 = 1 MiB。
- 空のときは **512 B ではなく 4 KiB の fixed-heap fallback**（固定ヒープは 8 KiB 未満を扱う）。
  これで最悪でも「4 KiB ごと」に留まり、旧経路の 8 倍増幅には戻らない。
- さらに、**有限時間の待ち**（例: 10 ms、waitq）を一段挟んでから fallback。
  §4.1 は「pool 待ちはしない」とするが、待たずに 8 倍遅い経路へ行くより、10 ms 待って
  64 KiB 経路を使う方が総遅延は小さい。inode lease を持ったまま待つことになるが、
  借り手は必ず有限時間で返すので deadlock にはならない（借り手は pool 以外を待たない）。
- counter: borrow 成功／待ち発生／fallback 発生を分ける（§3 にある）。**fallback 率が 1 % を超えたら
  要素数の既定値を上げる**、を受け入れ条件に入れる。

## I-2. F2: 新規ファイル・追記の経路は変わらない

Codex 案 §6.2 は「full block は `bmap_ensure()` を block ごとに行い、zero 初期化後に pointer を
永続化する順序を変えない」とし、§13 で「FAT/UFS metadata 全般の batching」を対象外にした。
その結果、**既存ファイルの上書き**は 64 KiB → USB コマンド 1 個になるが、**新規ファイル**は
block ごとに（レポート2 §15）

| 操作 | 8 KiB block あたり | 64 KiB あたり |
| --- | --- | --- |
| `load_cg_locked` 読み（cache hit） | 0 | 0 |
| `write_cg` 8 KiB | 1 run | 8 |
| `write_super_summaries` 8 KiB | 1 run | 8 |
| zero 書き 8 KiB | 1 run | 8 |
| `persist_inode` 8 KiB（bmap_ensure 内） | 1 run | 8 |
| data run | — | 1 |
| `persist_inode`（pwrite 末尾） | — | 1 |
| **計** | | **≈ 34 USB コマンド** |

しかも zero 書きは直後に data run で上書きされる。つまり **`cp`／`tar`／`zedinst`／エディタの保存
（一時ファイルへ新規書き）はこの計画の完了後も 30 倍以上遅い経路のまま**である。
「aligned 64 KiB overwrite が 1 コマンド」という完了像は、実用上もっとも多い書き込みを含まない。

これは Codex 案が間違っているのではなく、§13 の線引きの結果である。第 III 部 M0 を **Codex 案の
段階 3 の直後**に置くことで対処する（第 II 部 §II-1）。Codex 案側への修正は 1 点だけ:
§6.2 の「zero 初期化後に pointer を永続化する順序を変えない」は、**「pointer 公開の前に、
その block の全バイトが zero または data で書かれている」**と言い換える。これなら
「data で完全に覆う block は zero を省き、data run を先に書いてから pointer を公開する」が
契約の内側に入る（レポート4 §3.3）。これだけで表の zero 8 run が消える。

## I-3. F3: reset 直後の `06/28` と mounted root

§10 は `06/28` を REVALIDATE とし、「mounted root や open/claimed disk は generation replacement を
行わない。REVALIDATE/ABSENT のまま replug/reboot を要求する」とする。媒体交換の疑いがあるとき
古い namespace へ戻さない、という原則は正しい。

問題は、**USB スティックの一部は Bulk-Only Mass Storage Reset の直後に `06/29` ではなく `06/28` を
返す**ことで（実装依存。Linux の `usb-storage` が両方を同列に扱うのはこのため）、その場合
Codex 案 D の retry（`bot_reset` → 再発行）は `06/28` → REVALIDATE → mounted root は永久に
REVALIDATE、という経路になる。**自分の reset が root を止める。**

修正案: **ドライバ自身が発行した reset の直後、最初の 1 コマンド**に限り、`06/28` を `06/29` と
同じ「1 回だけ再試行」に分類する。ただし、その再試行の前に **軽量の同一性確認**（INQUIRY の
vendor/product/serial と READ CAPACITY の一致）を 1 回行い、一致しなければ REVALIDATE へ。
これは「INQUIRY/capacity の一致だけでは別媒体でないことの証明にならない」（§10）と矛盾しない。
証明ではなく、自分の reset の副作用を、自分が把握している範囲で弁別しているだけである。
それ以外の `06/28`（reset 直後でない、2 回目以降）は §10 のとおり REVALIDATE。

## I-4. F4〜F7（補足）

- **F4 exec**: `copy_segment_snapshot()`（`elf.c`）は 4 KiB bounce で `file_pread_internal` を呼ぶ。
  Codex 案 §6.1 の UFS read run は「要求範囲内」だけなので、exec の 4 KiB read は 4 KiB run のまま。
  §4 の pool を `elf.c` からも借りられるようにして chunk を 64 KiB にするだけで、
  300 KiB のバイナリの exec が 75 コマンド → 5 コマンドになる。段階 1 に含めてよい小改修。
- **F5 reclaim**: `buf_reclaim()` は `include/kern/buf.h:102` に宣言だけで、`src/` に呼び出し元が無い。
  現状 buffer cache は自分の cap（16 MiB）でしか縮まず、VM 側の memory pressure と無関係。
  Codex 案には直接関係しないが、第 III 部 W3 の前提なので、段階 0 の「事実」に記録する。
- **F6 reservation 容量**: 64 KiB × bulk 2 本 = 128 KiB の物理連続 coherent DMA を storage device ごとに
  attach 時に確保する。`alloc_ram` の 64 ページ連続要求が attach 時に走るのは許容範囲だが、
  複数 storage（root + 別スティック）で 2 倍になる。`hal_memory_get_stats` で見える形に。
- **F7 順序**: 段階 1（pool）と段階 2（cache run）は独立、3（UFS run）は 2 に依存、4（USB）は独立。
  1 と 4 は別 Queue で並行できる。効果が見えるのは 2+3+4 が揃ってから。

---

# 第 II 部 §13 の対象外機能は必要か

結論: **必要。ただし全部ではなく、順序がある。** Codex 案の完了像は「既存ファイルの aligned
64 KiB I/O が 1 コマンド」であり、それ自体は正しい到達点だが、USB-root の体感を決める
残りの経路が 3 つある。

| 経路 | Codex 案完了後の姿 | 必要な機能 |
| --- | --- | --- |
| 新規ファイル・追記（`cp`、`tar`、保存、ログ） | 64 KiB あたり ≈ 34 コマンド（F2） | **M: metadata batching／allocation cache** → **W: write-back** |
| 小さな書き込みの反復（journal 512 B、設定ファイル、`wifi-store` の一時ファイル + fsync × 3） | 1 回ごとに同期 3 URB + flush cascade | **W: write-back**（同一 line の N 回書きが 1 回に）、M2（fsync 重複） |
| exec、`PATH` 探索、`ls`、`cp` の読み側 | 4 KiB read（F4）、buffer cache 16 MiB を loop と物理で二重消費 | **P: page cache／先読み** |
| USB の帯域そのもの | 64 KiB / (3 URB × IMOD) ≈ 20 MB/s 上限、CPU 100 %（spin） | **S: async BIO**（syncer が device 時間と重なる）、IMOD |

各機能の必要性を個別に言うと:

**W（write-back、dirty watermark、syncer）** — 最も効く。理由は 2 つ。
(1) 同じ line が短時間に何度も書かれる: UFS1 の cg block・superblock・inode block は
1 ファイルを作るだけで各 4〜8 回、FAT の FAT sector・directory sector も同様。write-through では
それぞれが device 往復になる。write-back なら 1 回。
(2) 小さな syscall の列を大きな run に変える: 512 B の journal record、行単位のログ書き、
`printf` の flush が、それぞれ 3 URB + IMOD ではなく、syncer の 1 run になる。
これは M0 が減らせない残りの metadata コストを丸ごと吸収するので、**M と W は組で初めて
新規ファイル経路が既存ファイル経路と同じ速さになる**。

**P（page cache、先読み、user page 直接 I/O）** — 読み側の体感。exec のたびにバイナリを
loop → FAT → USB から読み直す構造（VM object は mmap 済みファイルにしか無い）が、
`sh` から `ls` を起動するたびに 4 KiB × 数十回の device 往復を生む。page cache があれば
2 回目以降は device 往復ゼロ、先読みで 1 回目も 64 KiB 単位。user page 直接 I/O は
USB 帯域では効かない（コピーはボトルネックでない）ので **最後**でよい。

**S（scatter/gather BIO、UAS、BOT 並列化）** — S/G は「それ自体が速い」のではなく、
**W の syncer が非連続な dirty line を 1 BIO で流すため**と、xHCI の bounce copy を消すために要る。
async completion（`bio.b_done` は既にある）は syncer と先読みが device 時間と重なるために要る。
UAS は接続先が UAS 対応のときだけ意味があり、多くの USB2 スティックは BOT なので **任意**。
BOT コマンド並列化は仕様（BOT §3.4）で不可 — Codex 案の判断どおり、やらない。

**M（FAT/UFS metadata batching、allocation bitmap／indirect block cache）** — F2 のとおり、
Codex 案の直後に **最初に**やる。W の前提でもある（write-back で dirty になる metadata の
書き順序を決めるのは M の仕事）。

順序（Codex 案の段階 0〜4 の後）:

```
M0 (UFS1 allocation) ─┐
M1 (FAT cache)        ├─→ W0 (dirty list, error seq) → W1 (syncer, opt-in) → W2 (watermark, reclaim)
M2 (fsync dedup)      ┘         │                                                   │
                                └──── S0 (S/G bio + async) ──── S1 (xHCI S/G) ───────┘
                                                                                     ↓
P0 (page cache for read) → P1 (readahead) → P2 (exec from cache) → [P3 direct user I/O, S3 UAS: 任意]
```

---

# 第 III 部 段階設計

原則:

- 各段階は **(a) インタフェースと no-op 実装 + counter + 「挙動不変」の fixture → (b) flag で有効化
  → (c) flag 付き fixture → (d) 既定 ON** の 4 歩で入れる。(a) だけを先に merge できる。
- Codex 案 §2 の契約（write-through 既定、失敗 line は dirty 残し、backing claim、媒体世代、
  timeout ownership）は各段階で維持し、変更するときは段階の中で明示する。
- 数値の既定値は全部 sysctl にし、計測後に決める。

## M. metadata batching と cache

### M0. UFS1 allocation と inode の書き回数

**目的**: 新規 64 KiB 書き込みの metadata run を ≈ 32 → ≈ 4 にする（F2 の表）。

**インタフェース（`ufs1-vfs.c` 内部、外部 API 変更なし）**

```c
/* 1 回の pwrite の間だけ生きる allocation transaction。 */
struct ufs1_alloc_txn {
        struct mount *mountp;
        uint32_t cg;              /* 現在 load 済みの cg（active_cg と一致すること） */
        unsigned cg_dirty;        /* write_cg を遅らせている */
        unsigned super_dirty;     /* write_super_summaries を遅らせている */
        unsigned inode_dirty;     /* persist_inode を遅らせている */
        uint32_t first, count;    /* 連続確保できた fragment 範囲 */
};
int  ufs1_alloc_begin(struct inode *, struct ufs1_alloc_txn *);
int  ufs1_alloc_blocks(struct ufs1_alloc_txn *, unsigned want, uint32_t *first, unsigned *got);
int  ufs1_alloc_commit(struct ufs1_alloc_txn *);   /* cg → super → inode の順に 1 回ずつ書く */
void ufs1_alloc_abort(struct ufs1_alloc_txn *);    /* bitmap を戻し cg を 1 回書く */
```

**段階**

1. **スタブ**: `ufs1_alloc_*` を追加し、`begin` は `load_cg_locked` を `active_cg != cg` のときだけ
   実行、`alloc_blocks` は既存 `allocate_block` を `want` 回呼ぶだけ、`commit` は no-op。
   counter: `ufs1.cg_reads/cg_writes/super_writes/inode_persists/zero_writes`。
   fixture: 既存 UFS fixture が全通過、counter は現状値と一致。
2. **cg/super の遅延**: `allocate_block` が `txn` を受け取り、`write_cg` と `write_super_summaries` を
   `cg_dirty/super_dirty` に置き換える。`commit` で各 1 回。**cg の bitmap を書く前に data が書かれる**
   順序は保たない（bitmap が先でも data が未書きの block は pointer 未公開なので参照されない。
   これは今の順序と同じ）。abort は bitmap を戻す。
3. **zero-fill の省略**: `pwrite_inode` が「この block を data で完全に覆う」と分かる block には
   `ufs1_alloc_blocks(..., ZERO_NOT_NEEDED)` を渡し、zero 書きを省く。**pointer の公開
   （`persist_inode`）は data run の後**に移す（I-2 の言い換え）。部分 block は従来どおり zero → data。
4. **inode の遅延**: `bmap_ensure` 内の `persist_inode` を `inode_dirty` にし、`commit` で 1 回。
   direct pointer 12 個までは inode 1 回、indirect は indirect block 1 回 + inode 1 回。
5. **連続確保**: `alloc_blocks` が cg の bitmap から `want` 個の連続 fragment を探す（rotor から先頭一致）。
   取れた分だけ返し、残りは次の呼び出し。これで data run が物理連続になり、Codex 案 §6.2 の
   run builder がそのまま 64 KiB run を作れる。

**契約**: 各 block は pointer 公開時点で zero または data で全バイト書かれている。
cg/super/inode は `commit` で必ず書かれ、`pwrite` が返る前に完了する（write-through のまま）。
**受け入れ**: 新規 64 KiB 書き込み = data run 1 + cg 1 + super 1 + inode 1（+ indirect 0〜1）。
fault injection: commit の途中失敗で bitmap と pointer が一致すること（既存の rollback fixture を流用）。

### M0'. indirect block と cg block の cache

`bmap()`／`bmap_ensure()` の `indirect_entry()` は毎回 8 KiB を読んで 4 byte を取り出す。
inode info に「最後に読んだ indirect block 番号 + 8 KiB の copy」を 1 段だけ持つ（`ui->ind_cache`）。
無効化: truncate、その block への書き込み、rollback。cg は `active_cg` 一致で `load_cg_locked` を
スキップ（M0 段階 1 に含める）。counter: `ufs1.indirect_reads`。96 KiB 超のファイルの
sequential read で `indirect_reads` が block 数 → 1 になることを受け入れに。

### M1. FAT

1. **sector cache の複数 slot 化**: `sector_cache[512]` を `FAT_SECTOR_SLOTS`（既定 8）の LRU に。
   `fat_engine_read_sector_result` は slot を探し、`fat_engine_flush` は dirty slot を全部書く。
   FAT table sector と directory sector と data sector の往復で毎回 flush していた分が消える。
   loop 経路は `fat_loop_transfer` が既に迂回しているので、効くのは boot 時と直接 FAT 利用。
2. **chain cursor の保持**: `fat_file_state` に `{index, cluster, generation}` を持ち、
   `fat_raw_write`／`fat_engine_read_chain` の seek をそこから始める。generation は
   truncate／別 open からの chain 変更／rollback で進める（回答書 C の要件）。
   全 chain 検証（書き側）は「generation が変わっていなければ前回の検証結果を再利用」で省く。
3. **ミラー FAT の遅延**: `fat_raw_set_cluster_copy(copy ≥ 1)` を dirty マークだけにし、
   `fat_sync_mount`／unmount／`fsync` で書く。**policy 化**（`vfs.fat.mirror_sync = immediate|deferred`、
   既定 immediate）。deferred は W1 の後に既定にする。

### M2. fsync の重複と overlay cascade

レポート1 §8: overlay の `fsync` は file → mount(journal + upper) で 3 回の 2 段 flush cascade。
各 mount と disk に **flush generation**（`d_flush_seq`、`m_sync_seq`）を持ち、
`bio_flush()` は「前回の flush 以降に write が無ければ no-op」、`overlay_regular_fsync` は
journal と upper mount の sync を 1 回ずつにまとめる。counter: `disk.flush_issued/flush_skipped`。
受け入れ: overlay 上の `write + fsync` 1 回で SYNCHRONIZE CACHE が最大 1 回。

## S. scatter/gather BIO と async 完了

### S0. インタフェース

```c
struct bio_segment { void *data; hal_physaddr_t paddr; size_t length; };

struct bio {
        ...
        void *b_data;                 /* 既存: 単一連続バッファ（互換） */
        struct bio_segment *b_segments;   /* NULL なら b_data を 1 segment とみなす */
        unsigned b_segment_count;
        void (*b_done)(struct bio *);     /* 既存。NULL なら bio_wait 用 */
};
#define DISK_CAP_SG       0x00000010U   /* driver が b_segments を直接扱える */
#define DISK_CAP_ASYNC    0x00000020U   /* submit が待たずに返り b_done を呼ぶ（現状 loop 以外は実質そう） */
int bio_submit_async(struct disk *, struct bio *);   /* = bio_submit。名前で意図を分ける */
```

**段階**

1. **スタブ**: `b_segments` を追加。`disk_transfer_direct` は `b_segments != NULL` なら
   segment ごとに従来の単一 bio を発行する（SG 非対応 driver への互換路）。挙動不変。
   counter: `disk.sg_bios/sg_segments/sg_fallback_splits`。
2. **buffer cache の run が SG を使う**: Codex 案 §5.2 の write run は caller pointer から
   1 segment で出す（変更なし）。**W1 の syncer が非連続 dirty line を流すとき**に
   `b_segments = 各 line の b_data` で 1 bio にする。ここが S0 の主目的。
3. **async**: `bio_submit` は既に非同期（`bio_wait` が待つ）。`b_done` 付き bio を
   `buf` 層から出せるようにし、`read_buffer`／run 書き込みの完了を `b_done` で受けて
   `b_io_state` を落とす経路を用意（W1 と P1 が使う）。**同期 API はそのまま**。

### S1. xHCI の S/G

`enqueue_normal()` は既に 64 KiB 境界で Normal TRB を分けている。segment ごとに TRB を
作る（chain bit で 1 TD）だけで、bounce copy 無しに cache line から直接 DMA できる。
条件: segment が 64 KiB 境界を跨がない（buf line は 4 KiB なので満たす）、coherent でない
メモリなら書き込み前に cache flush（amd64 は coherent）。`DRV_USB_URB_SG` flag を URB に足し、
storage が `DISK_CAP_SG` を公開。**reclaim-safe 経路は従来の bounce のまま**。
受け入れ: 64 KiB write で xHCI の memcpy が 0、TRB 数 = segment 数。

### S2. queue depth > 1（任意、NVMe 向け）

BOT は LUN あたり 1 コマンド直列（仕様）なので USB には効かない。xHCI ring は 255 TRB
あるので endpoint queue depth を上げられるが、利用者が BOT だけなら意味が無い。
**NVMe install target（master.md の次の north star）で必要になったとき**に、S0 の async を
前提に `nvme_io_execute` を複数 slot 同時に。

### S3. UAS（任意）

接続先が UAS（`bInterfaceProtocol 0x62`）のときだけ。command/status/data-in/data-out の 4 pipe と
stream ID。BOT と別 class driver。**実機に UAS デバイスが来るまで着手しない。**

## W. write-back

### W0. dirty 索引とエラー世代（write-through のまま）

**インタフェース（`buf.h`）**

```c
#define BUF_WRITE_THROUGH   0x0U        /* 既定。返る前に書き終わる */
#define BUF_WRITE_DELAYED   0x1U        /* dirty にして返る。syncer / fsync が書く */
int  buf_write_ex(struct disk *, uint64_t block, uint32_t count, const void *data, unsigned flags);
int  buf_flush_range(struct disk *, uint64_t block, uint64_t count);   /* dirty run を書き、bio_flush はしない */
int  buf_flush_disk(struct disk *);                                    /* = buf_sync の索引版 */
uint64_t buf_error_seq(struct disk *);   /* write-back 失敗のたびに +1 */

/* mount / inode 側 */
uint64_t m_error_seq_seen;   /* struct mount: 最後の sync で観測した seq */
uint64_t i_error_seq_seen;   /* struct inode: 最後の fsync で観測した seq */
```

**段階**

1. **dirty 索引**: `struct disk` に `d_dirty_list`（block 昇順の double link）と `d_dirty_bytes`。
   `buf_mark_dirty` で追加、clean になったら外す。`buf_sync` はこの索引から取る
   （レポート2 §11 の O(n²) 解消）。挙動不変（write-through ではリストは常に短命）。
   fixture: `buf_sync` の結果が既存と同じ、`d_dirty_bytes == cache_dirty_bytes` の総和。
2. **エラー世代**: `buf_writeback` の失敗で `d_error_seq++`。`fsync(2)` は
   `buf_flush_range` → `bio_flush` の後、`d_error_seq != i_error_seq_seen` なら EIO を返し
   `i_error_seq_seen` を更新（Linux の errseq と同じ。エラーは 1 回は必ず誰かに届く）。
   write-through では `buf_write` 自体が EIO を返すので、この段階では追加の観測だけ。
3. **`buf_write_ex` の追加**: `BUF_WRITE_DELAYED` は **この段階では THROUGH と同じ動作**。
   呼び出し側（UFS1 data、FAT data、loop backing）を `_ex` に切り替えて flag を渡し始める。
   counter: `bufcache.delayed_requests`（まだ全部 through）。

### W1. syncer と opt-in

```c
/* sysctl */
vfs.bufcache.writeback = 0|1         /* 既定 0 */
vfs.bufcache.sync_interval_ms = 1000 /* syncer 周期 */
vfs.bufcache.max_dirty_age_ms = 5000 /* これより古い dirty は次周期で必ず書く */
```

1. **syncer thread**: `kthread_create` で 1 本。周期ごとに各 disk の dirty 索引を舐め、
   `max_dirty_age_ms` を超えた line と、`d_dirty_bytes` が W2 の low を超えた分を、
   **連続 line を run にまとめて**（S0 の SG があれば非連続も 1 bio）書く。
   完了は同期でよい（syncer 自身が待つ）。書き終えた disk に `bio_flush` は **しない**
   （flush は fsync の仕事。ここは write-through の「デバイスに渡した」と同じ強さ）。
2. **`BUF_WRITE_DELAYED` を有効化**（`writeback=1` のとき）: `buf_write_ex` は line を dirty にして
   索引に入れ、即返る。**対象は data line だけ**: UFS1 の `write_block`（data）、FAT の
   `fat_loop_transfer`（write）、loop backing。**metadata（cg、super、inode、FAT table、directory）
   は THROUGH のまま**。この段階の順序契約は「metadata は同期、data は遅延」で、
   M0 の「pointer 公開の前に data が書かれている」と組み合わせると、**pointer 公開の直前に
   `buf_flush_range(data run)`** が必要になる。M0 の `commit` に 1 行足す（data → flush → inode）。
3. **fsync／close／unmount**: `ufs1_file_sync` → `buf_flush_range(file の全 dirty)` → `bio_flush`。
   `mount_sync` → `buf_flush_disk`。unmount／shutdown → `buf_flush_disk` 完了を待つ。
   loop の `BIO_FLUSH` → backing file の `fsync` → 親 disk の flush、という既存の cascade は
   そのまま（M2 で重複を消す）。
4. **受け入れ**: `writeback=1` で、512 B × 128 回の `write()` + `fsync` が USB コマンド
   「data run 1〜2 + metadata + SYNCHRONIZE CACHE 1」になる。電源断 fixture（QEMU の disposable
   image を flush 前に kill）で、fsync 済みデータは残り、未 fsync データは欠けてもよいが
   **metadata が壊れていない**（fsck 相当の check-ufs1-image.py が通る）こと。

### W2. watermark と reclaim

```c
vfs.bufcache.dirty_high_pct = 40   /* cache 容量比。超えたら buf_write_ex が待つ */
vfs.bufcache.dirty_low_pct  = 20   /* syncer はここまで書く */
```

1. `buf_write_ex(DELAYED)` は `d_dirty_bytes` が high を超えていたら syncer を起こして waitq で待つ
   （throttle）。write-through 呼び出しは待たない。
2. **reclaim との接続**（F5）: `vm_reclaim_one()` が page 確保に失敗したら
   `buf_reclaim(target, BUF_RECLAIM_WRITE)` を呼ぶ。`buf_reclaim` は clean line を LRU から落とし、
   足りなければ dirty を **run で** 書いてから落とす（既存 `writeback_one_reclaimable` を run 化）。
   reclaim-safe 経路（swap が USB 上）では `buf_reclaim` が USB I/O を起こすので、
   xHCI の 8 KiB emergency reserve の範囲で書く（1 line ずつ）。
3. **受け入れ**: dirty を high まで積んだ状態で `write()` の遅延が有界（syncer の 1 周期以内）、
   memory pressure fixture で `buf_reclaim` が呼ばれて cache が縮む。

### W3. metadata の遅延（ordered write-back）

data だけの遅延で新規ファイル経路は M0 と合わせて「data run 1 + metadata 3〜4（同期）」になる。
metadata も遅延するのはその先で、**順序契約を明示してから**:

| 依存 | 規則 |
| --- | --- |
| data → inode pointer | data の flush 後に inode を書く（W1-2 で導入済み） |
| cg bitmap → inode pointer | bitmap 確保が pointer より先にディスクに出る（未参照 block が「使用中」になる側に倒す） |
| inode pointer clear → bitmap free | 解放は pointer clear の flush 後（今の `detach_inode_block` と同じ） |
| superblock summary | いつでもよい（stale は fsck で再計算可能） |
| FAT table → directory entry | cluster chain が先、directory entry が後 |
| FAT mirror | primary の後、sync 時 |

実装は dependency graph ではなく **「pointer を書く関数が、依存先の range を `buf_flush_range` してから
自分を DELAYED で書く」**という局所規則にする（soft-updates の簡略版）。fixture は
「任意の時点で kill しても check-ufs1-image.py／FAT checker が通る」。

### W4. 既定 ON

W1〜W3 の fixture と、USB-root の disposable image で 100 回の kill/reboot を通した後に
`vfs.bufcache.writeback=1` を既定に。

## P. page cache と先読み

### P0. read() の page cache

現在 `vm_object` は mmap 時にしか作られず（`vmspace.c`）、`file_io_transfer` の coherent read は
「published object があるとき」だけ使う（`file.c:972`）。**mmap されていない regular file の
read() でも object を作って使う**のが P0。

```c
/* mount flag */
#define MOUNT_PAGECACHE 0x…          /* この mount の regular file は read で object を作る */
/* vm-object.c */
int vm_object_get_for_read(struct inode *, struct vm_object **);   /* 無ければ作る。mmap と共用 */
int vm_object_populate(struct vm_object *, off_t start, size_t len); /* 未 valid page を fs から読む */
```

1. **スタブ**: `MOUNT_PAGECACHE` を定義、どの mount にも付けない。`vm_object_get_for_read` は
   既存の `get_shared` を呼ぶだけ。挙動不変。counter: `pagecache.hits/misses/populated_bytes`。
2. **read path**: `file_io_transfer(READ)` で mount が `MOUNT_PAGECACHE` なら object を取得し、
   `vm_object_read_coherent` の既存経路で page から copy。miss なら `vm_object_populate` が
   **pool buffer に fs pread（64 KiB）してから page へ copy**（S/G 前）。
   write path は現状どおり fs へ write-through し、既存の content transaction で page を更新
   （MAP_SHARED coherence の仕組みをそのまま使う）。
3. **overlay 上で有効化**: object は overlay inode ではなく **実層の inode**（upper/lower）に付ける
   （`f_vm_inode` は既にそうなっている）。lower は read-only なので page は無効化されない。
4. **メモリ**: page は `vm_object_reclaim_one` の対象。buffer cache（16 MiB）と page cache が
   同じ data を持つ二重化は、P0 では許容し、**P2 の後に buffer cache を metadata 中心に縮める**。
5. **受け入れ**: 同じファイルの 2 回目の `cat` で `disk.read_bios == 0`。`MAP_SHARED` fixture が通る。

### P1. 先読み

```c
struct file { ... off_t f_ra_next; unsigned f_ra_window; };   /* sequential 検出 */
vfs.readahead_max_kib = 128
```

`file_io_transfer(READ)` で「前回の終端 == 今回の開始」なら window を倍（64 KiB → 128 KiB）、
外れたら 0 に。populate を `S0 の async bio` で投げて返る（S0 が無ければ同期で window 分読む —
それでも 4 KiB × N が 64 KiB × 1 になる）。exec（F4）は P2 まではこの同期 window で改善する。
受け入れ: 1 MiB の sequential read で `disk.read_bios ≈ 16`（64 KiB 単位）。

### P2. exec を page cache から

`copy_segment_snapshot()` の bounce copy を、object の page を **直接 map**（text は読み取り専用共有、
data は COW）に変える。`vmspace.c` の mmap 経路がそのまま使える（`MAP_PRIVATE` 相当）。
効果: 2 回目以降の exec は device 往復 0、同じバイナリの複数プロセスが text page を共有。
受け入れ: `sh -c 'for i in $(seq 100); do ls >/dev/null; done'` で 2 回目以降 `disk.read_bios == 0`。

### P3. user page からの直接 I/O（任意）

`uaccess_pin` は既にある。S0 の `bio_segment` に user page の paddr を並べて fs に渡すには
`file_ops` に page-vector 版（`preadv_pages`/`pwritev_pages`）が要り、対応 fs は UFS1 の data run
だけになる。**USB 帯域ではコピーが律速にならない**ので、P3 は tmpfs／NVMe で計測して
必要が出てから。amd64 の直接マップは 1 GiB（`AMD64_DIRECT_LIMIT`）なので、その上の page は
kernel からアクセスできず、vmap が先に要る。

---

## 依存とマイルストーン

| マイルストーン | 含む段階 | 期待される姿 |
| --- | --- | --- |
| **Codex 案 0–4** | pool、cache run、UFS run、USB 64 KiB | 既存ファイル 64 KiB overwrite = 1 コマンド |
| **M** | M0, M0', M1, M2 | 新規 64 KiB = data 1 + metadata 3〜4；fsync = SYNCHRONIZE CACHE 1 |
| **W-data** | S0, W0, W1, W2 | 小書き込みの列が run に；新規ファイル = data 1 + metadata 3〜4（同期） |
| **W-meta** | W3, W4 | 新規ファイル = 全部 run；metadata 連続書きが 1 回に |
| **P** | P0, P1, P2 | exec/PATH/ls の 2 回目以降 = device 往復 0；1 回目 = 64 KiB 単位 |
| **任意** | S1, S2, S3, P3, IMOD | S/G で copy 0；NVMe で depth>1；UAS 実機時 |

各段階の (a) スタブは単独で merge 可能で、既存 fixture が全通過することだけを条件にする。
(b)〜(d) は sysctl／mount flag で切り替え、既定を変えるのは各段階の受け入れの後。

## 対象外（この文書でも）

- BOT コマンド並列化（仕様上不可）。
- 媒体変更後の mounted filesystem の継続（Codex 案 §10 の判断を維持）。
- 測定前の IMOD 既定値変更（Codex 案 §11）。
