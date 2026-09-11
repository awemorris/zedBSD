# 申し送り: syscall バッファ確保と buffer cache のライン結合

日付: 2026-09-06
照合対象: `/home/awe/zedBSD` HEAD `dd4f31c` (`Fix syscall write`)
宛先: 実装担当（Codex）
状態: 静的レビューに基づく申し送り。ビルド・実行・計測はしていない。
範囲: 機能性（性能・安定性）のみ。セキュリティは扱わない。

## 0. 要旨

q087 の「要求サイズの syscall I/O」は方向として正しく、`file_io_transfer` 1 回あたりの
固定費（gate、lock、`persist_inode`、`overlay_refresh`）の償却は実際に効いている。
ただし、そのままでは次の 2 点が残る。

| # | 問題 | 種類 | 対処 |
| --- | --- | --- | --- |
| A | **syscall ごとに物理連続メモリを確保している。** その allocator は IRQ-off で物理ページ bitmap を先頭から線形走査する | 新規に入った退行（稼働時間とともに悪化、QEMU 直後では観測できない） | per-thread のキャッシュバッファに置き換える。上限は 64 KiB で十分 |
| B | **`buf_write()` が 4 KiB ライン単位で同期 write-through し、隣接ラインを結合しない。** 256 KiB を渡しても物理デバイスには 4 KiB コマンドが 64 個直列に出る | 元からある構造。q087 でこれが最上位のボトルネックになった | 1 回の `buf_write`/`buf_read` 呼び出し内で連続する完全ラインを 1 BIO にまとめる。write-through の契約は変えない |

B が効くには USB 側の 1 コマンド上限（現在 8 KiB）も同時に上げる必要がある（§4）。
IMOD の既定値（§5）と `media_error` の永続ラッチ（§6）も併記する。

なぜ見落とされやすいか: A は「`kern_malloc` が成功する」ことしか見えない。
コストは `hal_pmem_alloc` → `alloc_ram` の中にあり、起動直後の QEMU では走査が短い。
B は「`file_io_transfer` に 256 KiB が届いた」で止まると見えない。粒度は 2 層下で 4 KiB に戻る。

---

## 1. 現在の粒度（write の場合）

| 層 | 粒度 | 根拠 |
| --- | --- | --- |
| `sys_write_call` → `file_io_transfer` | min(要求, 256 KiB) | [syscall.c:84](../../src/kern/syscall.c:84), [syscall.c:2479](../../src/kern/syscall.c:2479) |
| overlay → UFS1 `pwrite_inode` | 8 KiB block × ループ（完全 block は先読み無し） | `ufs1-vfs.c` `pwrite_inode` |
| **`buf_write`（loop disk）** | **4 KiB ライン × ループ、各ラインを `buf_writeback` で同期書き込み** | [buf.c:344](../../src/kern/buf.c:344), [buf.c:403](../../src/kern/buf.c:403) |
| `disk_write_direct` → `loop_submit` | 4 KiB BIO | [loop.c:151](../../src/drivers/loop.c:151) |
| `file_pwrite_internal` → `fat_loop_transfer` | extent 単位のマルチセクタ（chain 走査・512 B 分割は解消済み） | [fat.c:4346](../../src/drivers/fs/fat.c:4346), [fat.c:4383](../../src/drivers/fs/fat.c:4383) |
| **`buf_write`（物理 disk）** | **再び 4 KiB ライン × ループ** | 同上 |
| `disk_transfer_direct` | `d_max_transfer_blocks` で分割 | [disk.c:1810](../../src/kern/disk.c:1810) |
| `usb-storage` | 1 コマンド ≤ `RECLAIM_SAFE_MAX_SIZE` = 8 KiB | [usb-storage.c:889](../../src/drivers/usb-storage.c:889), [usb.h:78](../../include/drivers/usb.h:78) |

結果: **256 KiB の write = loop BIO 64 個 = 物理 4 KiB BIO 64 個 = USB コマンド 64 個**、
すべて直列・同期・spin 待ち・IMOD 1 ms（3 URB/コマンド）。
概算で 256 KiB ≈ 200 ms、書き込み上限 ≈ 1.3 MB/s。syscall のチャンクを大きくしても
ここは動かない。read も同様（先読み無し、ライン単位のミス）。

---

## 2. 問題 A: syscall ごとの物理連続確保

### 2.1 事実

`syscall_regular_buffer()` は通常ファイルの 512 B 超の read/write/pread/pwrite/readv/writev
ごとに `kern_malloc(min(len, 256 KiB))` を呼ぶ（[syscall.c:2493](../../src/kern/syscall.c:2493)）。

`kern_malloc` は `KERNEL_LARGE_THRESHOLD` = 2 ページ（[entry.c:44](../../src/kern/entry.c:44)）
以上で固定ヒープを使わず `hal_pmem_alloc` に落ちる。stdio の `BUFSIZ` は 4096 なので
4 KiB の `fwrite` 1 回はまだ固定ヒープだが、8 KiB 以上（`cat`、`cp`、`dd`、エディタの保存、
`wifi-store` の一時ファイル）はすべてこの経路に入る。

amd64 の `alloc_ram()`（[page.c:300](../../src/hal/amd64/page.c:300)）は

* `asm_cli()` で割り込みを止め（[page.c:332](../../src/hal/amd64/page.c:332)）、
* 物理ページ bitmap（1 GiB 直接マップ分 = 262 144 bit）を**ページ 1 から先頭一致で線形走査**
  （[page.c:333](../../src/hal/amd64/page.c:333)）、ヒント・rotor・空き run 索引は無く、
* `need` ページ（256 KiB なら 64 ページ）の**物理連続**な空きを要求する。

失敗すると `syscall_regular_buffer` はサイズを半分にして再試行する（最大 6 回、毎回先頭から走査）。
`kern_free` 側は生存中の large allocation を単方向リストで線形に辿る（[entry.c:246](../../src/kern/entry.c:246)）。

### 2.2 なぜ問題か

* コストは **使用済みページ数に比例し、稼働時間とともに増える**。カーネル、16 MiB のバッファ
  キャッシュ、プロセスページ、xHCI の DMA ページを毎回飛び越える。起動直後の QEMU では短い。
* 64 ページ連続の穴は断片化で遠くなる。半分化のたびに走査をやり直す。
* **IRQ-off で走る**ので、タイマ・スケジューラ・xHCI 割り込みの遅延ジッタになる。
  レポート2 §10（`mutation_reserve` の IRQ-off 線形走査）と同種の問題を、より高頻度の経路に足した形。
* `kern_malloc` 成功／失敗しか見えないため、fixture では検出できない。

### 2.3 対処（推奨順）

1. **確保をやめる。** per-thread のキャッシュバッファを 1 本持つ（初回使用時に確保、
   スレッド終了時に解放、または per-CPU プール）。syscall はそれを借りて返す。
   `struct thread` にポインタと容量を 1 つ足すだけで済む。
2. **上限を 64 KiB にする。** 下層の粒度が UFS1 8 KiB／buf 4 KiB／USB 8 KiB なので、
   64→256 KiB の差は `persist_inode` 償却分しかない。64 KiB なら固定ヒープ側の
   閾値を上げるだけでも `hal_pmem_alloc` を避けられる。
   q087 の 4×64 KiB ベンチ（70→20 ms）は 64 KiB 上限でも同じ値になるはず。これを確認セルにする。
3. `alloc_ram` 自体にも rotor（前回の割当位置から探し始める）と、IRQ-off 区間を
   「候補確定〜BIT_SET」だけに縮める修正を入れる。これは A の対処というより、
   xHCI の DMA ページ確保など他の利用者すべてに効く独立改善。

### 2.4 受け入れ

* 定常状態で read/write syscall 1 回あたりの `hal_pmem_alloc` 呼び出し回数が **0**。
  （カウンタを `hal_pmem_get_stats` に足すか、テスト用 hook で数える。）
* バッファキャッシュを満杯にし、さらに数百 MB のユーザプロセスを載せた状態で、
  64 KiB write の p95 が起動直後と同じ。
* 既存の q087 fixture（境界サイズ、部分失敗、EFAULT、PIPE_BUF、vector）はそのまま通る。

---

## 3. 問題 B: `buf_write`/`buf_read` のライン結合

### 3.1 事実

`buf_write()`（[buf.c:344](../../src/kern/buf.c:344)）は要求範囲をラインごとに回し、
各ラインで `acquire_line` → `memcpy` → `buf_mark_dirty` → **`buf_writeback`（同期 `disk_write_direct` 1 回）**
→ `buf_release` する（[buf.c:403](../../src/kern/buf.c:403)）。隣接する完全ラインでも結合しない。
`buf_read()`（[buf.c:290](../../src/kern/buf.c:290)）も同様で、ミスしたラインごとに
`read_buffer` が 1 BIO を出す（[buf.c:319](../../src/kern/buf.c:319)）。

### 3.2 これは write-back ではない

回答書 C は「write-through は意図的な仕様。write-back は初期スコープ外」とし、その判断は維持する。
ここで提案するのは **1 回の `buf_write` 呼び出しの中で、連続する完全ラインを 1 つの BIO に
まとめて出す**ことで、呼び出しが戻る時点で全データがデバイスに書き終わっている点は変わらない。
dirty を残さない。syncer も要らない。契約は同じで、BIO の数だけが減る。

### 3.3 設計

`buf_write` の新しいループ:

1. 要求範囲を「run」に分ける。run = 先頭 partial ライン（あれば）／中央の連続完全ライン群／
   末尾 partial ライン（あれば）。中央の run は `leaf->d_max_transfer_blocks` を超えない長さで切る
   （USB なら現在 16 block = 8 KiB、§4 で拡大）。
2. run 内の全ラインを `acquire_line`（完全ラインは `read_data = 0`）して busy にし、
   `memcpy` と `buf_mark_dirty` を行う。
3. run 全体に対して **`disk_write_direct(leaf, run_start, run_blocks, …)` を 1 回**呼ぶ。
   データは各ラインの `b_data` に散っているので、
   (a) `struct bio` に scatter/gather を足す（`b_data` の配列）、または
   (b) `disk_write_direct` の前に run 用の連続バッファへ寄せる（コピー 1 回増えるが最小変更）。
   初段は (b) でよい。バッファは §2 の per-thread バッファを流用できる（write 側の syscall
   バッファは copyin 後に空くわけではないので別枠が要る。`d_max_transfer_blocks` × block size
   分を per-thread にもう 1 本持つか、バッファキャッシュが reserve を 1 つ持つ）。
4. 成功したら run の全ラインの `BUF_DIRTY` を落とし、失敗したら **その run の全ラインを
   dirty のまま `BUF_ERROR`** にする（現在の `buf_writeback` の単ライン版と同じ規則を run に拡張）。
   `b_dirty_generation` の比較も run の各ラインで行う。
5. `mutation_reserve`（backing claim）は run ごとに 1 回でよい。`disk_transfer_direct` が
   `backing_mutation_begin_disk` を range で取っているので、range を run に広げるだけ。
   これで レポート1 §10 の呼び出し回数も 1/16〜1/64 になる。

`buf_read` も同じ形: 要求範囲のうち **未 valid なラインの連続 run を 1 BIO で読む**
（要求範囲内だけ。先読みはしない。先読みは別課題）。読み込み中は run の全ラインを busy にして
`BUF_IO_READING` を立て、完了後に `BUF_VALID` を配る。

### 3.4 loop 経路での効果

loop disk の `buf_write` が 64 KiB run を出す → `loop_submit` が 64 KiB BIO を受ける
（`LOOP_MAX_TRANSFER_BLOCKS` = 128 = 64 KiB、[loop.c:26](../../src/drivers/loop.c:26)）→
`file_pwrite_internal(64 KiB)` → `fat_loop_transfer` が extent 単位で
`disk_write_filesystem(extent)` → 物理側 `buf_write` が再び run を作って 1 BIO → USB。
loop 側と物理側の両方で結合が効くので、`buf_write` の修正 1 箇所で両段が改善する。

### 3.5 注意点

* `acquire_line` は line ごとに `cache_lock` を取る。run 分を一度に busy にするとき、
  途中で `ENOMEM`／`EBUSY` になったら確保済みラインを解放して run を短くして続ける。
  デッドロックを避けるため、ライン確保はブロック番号昇順で行い、複数 run を同時に持たない。
* `buf_sync()`／`buf_invalidate()` は既存のまま動く（ラインの dirty/valid 状態の意味は不変）。
* `d_block_size > PAGE_SIZE` のディスク（ライン = 1 block）でも run の考え方は同じ。
* 物理ディスク側の `disk_read_direct` を `read_buffer` が使う箇所は、run 版を別関数にして
  既存の単ライン `read_buffer` は残す（`acquire_line` のヒット後の再読みが使っている）。

### 3.6 受け入れ

* 64 KiB の write で、loop disk の `stat_write_bios` が 16 → 1（USB 上限を上げた後）、
  物理 disk の `stat_write_bios` が 16 → `ceil(64 KiB / USB 上限)`。
* 途中 BIO 失敗の注入で、失敗 run のラインだけが dirty+ERROR で残り、成功 run は clean。
  その後の `buf_sync` が失敗 run を再送すること。
* `buf_write` が戻った時点で `cache_dirty_bytes` が呼び出し前と同じ（write-through 維持の証拠）。
* 既存の `run-fat-native-vfs-host-test.sh`、storage foundation、UFS consistency fixture が通る。

---

## 4. USB の 1 コマンド上限

`d_max_transfer_blocks = RECLAIM_SAFE_MAX_SIZE / block_size` = 16（[usb-storage.c:889](../../src/drivers/usb-storage.c:889)）。
これが 8 KiB のままだと §3 の効果は 4 KiB → 8 KiB の 2 倍止まり。

* reclaim-safe（swap 経路）の予約サイズを大きくするか、
* **通常 I/O には reserve と別の、より大きい同期バッファ**を `drv_usb_urb_reserve_sync()`
  （[usb.c:3020](../../src/drivers/usb.c:3020)）で URB ごとに持たせ、`d_max_transfer_blocks` を
  そちらの容量から決める。reclaim 中の BIO だけ 8 KiB に落とす（`FILE_IO`/`BIO` に
  reclaim フラグが既にあるなら流用）。

目安は 64 KiB（`LOOP_MAX_TRANSFER_BLOCKS` と揃える）。BOT の転送長は 32 bit なので
プロトコル上の制約は無い。

期待値（64 KiB write、USB 上限 64 KiB、§2・§3・§4 適用後）:
USB コマンド数 16 → 1、URB 数 48 → 3、IMOD 待ち 48 ms → 3 ms。

---

## 5. IMOD の既定値

`ZEDBSD_XHCI_IMOD` は可変になったが既定は 4000（= 1 ms、[pci-xhci.c:20](../../src/drivers/pci-xhci.c:20)）。
q087 の結果に「IMOD 4000/0 セルが通る」とあるので、**実機で 0／160 を比較して既定を下げる**。
§3・§4 でコマンド数が減った後も、コマンドあたり 3 URB は残るので効果は消えない。

---

## 6. `media_error` の永続ラッチ

q086 で `bot_command_sense_locked` に retry が入ったのは正しい。ただし同時に、
sense が `06/xx`（`06/29` の 1 回再試行を除く）または `02/3A` のとき
`storage->media_error = EIO` を立て（[usb-storage.c:518](../../src/drivers/usb-storage.c:518)）、
以後の **全 BIO（read を含む）** を `media_error` で拒否する
（[usb-storage.c:708](../../src/drivers/usb-storage.c:708)）構造になっている。解除経路は無い。

回答書 D/E は「媒体変更を疑う sense では古い mount/cache/claim のまま書かない」とした。
それは正しいが、「書かない」と「以後永遠に読みも拒否する」は別で、後者は回答書自身が
退けた「一度失敗したら永遠」の形。少なくとも、

* `06/28`／`02/3A` → 媒体世代を上げ、**媒体同一性の再確認**（INQUIRY + READ CAPACITY +
  既知 LBA の内容比較など、E で定義する手順）に成功したらラッチを解除する経路、
* `06/xx` のそれ以外（例: `06/2A` parameters changed、`06/3F`）は媒体無効ではないので
  ラッチしない、

を入れる。USB-root では、このラッチ 1 回で読みも止まる = システム停止になる。

---

## 7. 実施順と計測

| 順 | 作業 | 計測セル |
| --- | --- | --- |
| 1 | §2: per-thread バッファ、上限 64 KiB | `hal_pmem_alloc`/syscall = 0；メモリ満杯状態での p95 |
| 2 | §3: `buf_write`/`buf_read` の run 結合（bio は連続バッファ方式で開始） | loop/物理 `stat_write_bios`；`cache_dirty_bytes` 不変 |
| 3 | §4: USB 通常 I/O の上限 64 KiB | USB コマンド数／64 KiB |
| 4 | §5: IMOD 既定値 | 割り込み間隔ヒストグラム、write 実時間 |
| 5 | §6: `media_error` の解除経路 | 媒体変更 sense 注入 → 再確認 → 復帰 |

各段で q086 の「4×64 KiB overwrite + fsync」を同一イメージで取り直し、
段ごとの差分を記録する。1 段目は時間がほぼ変わらないのが正しい結果
（A は遅延ジッタと長期劣化の対策であり、直後のスループットには出ない）。

## 8. 対象外

* write-back、dirty 上限、syncer（回答書 C の判断を維持）。
* 汎用 page cache／先読み（`buf_read` の run 結合は要求範囲内のみ）。
* UAS、深い queue、BOT コマンドの並列化（BOT §3.4 により不可）。
* `mutation_reserve` の索引化（レポート1 §10）は §3 で呼び出し回数が減るので後回しでよい。

## 9. 一言で

**確保は syscall ごとにしない。ラインは呼び出しごとにまとめて出す。USB の上限を合わせる。**
この 3 つで、q087 が `file_io_transfer` に届けた 256 KiB が、初めてデバイスまで届く。
