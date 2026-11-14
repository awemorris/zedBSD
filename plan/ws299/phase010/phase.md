<!-- awesome-plan project=zedbsd record=ws299-p010 -->

# ws299-p010 block device の O_DIRECT

- 親: [WS299](../ws.md)。Queue: q1640 / q1640-i01（担当 P1）。番号は P1 の仮（Q1 が表と依存を決める）。
- Status: in-progress（2026-11-13 q2044 P2: direct の書き 96%、読み 1 MiB 94〜95%・4 MiB 92〜93% が未達。下の「2026-11-13」）
- 出典: 2026-10-28 ユーザー（Q1 経由）「raw の冷えた読み 4 GiBは、zedBSDではpage cache経由なのか、directなのか。zedBSDでもどちらの経路でも読めるなら、両方でLinuxと同等以上の性能にチューニングしたいです。」、P1 の案 B に対しユーザーのクリック「O_DIRECT を UAPI に足す」。Q1: block device の open に O_DIRECT（Linux と同じ意味: cache を通らず、重なる dirty の line は先に書き戻して無効にする）、揃わない offset・長さの扱いも Linux に合わせて決めて記録、regular file は EINVAL か無視かを決めて記録、受け入れに direct の読み・書き（Debian の direct 2.1 GB/s 以上、書きも Debian の direct と比べる）。

## 今の事実

- 普通の open の raw の読み書きは buffer cache を通る（`src/kern/devfs.c` block_pread_data:1628 → disk_read → buf_read と p004 の先読み。書きは p005 の disk_write_delayed）。cache を通らない道は BLKRESERVE の排他の claim の open だけ（disk_read_direct、devfs.c:1677・1706）。UAPI に O_DIRECT は無い（`include/uapi/fcntl.h`:68-83）。
- syscall の層は user の buffer を先に pin し（`sys_read_call` syscall.c:2932、write :3037、vector :4825）、1 MiB の kern_malloc の bounce に読んでから写す（:2838-2878）。page 列でない BIO では NVMe の driver も bounce する。

## 設計

### UAPI と libc

- `O_DIRECT 0x100000`（fcntl.h、空いている次の bit）。libc は flag をそのまま渡すので変えない（`include/libc/fcntl.h`:15、`userland/base/libc/posix.c`:655・686・834-838）。

### 意味（P1 の決定）

- block device だけが受ける。regular file・directory・chr・FIFO は open で EINVAL（黙って無視すると cache を避けたつもりの測定やアプリが誤る。EINVAL ならアプリが fallback できる）。`/dev/fd/N` は元の description に無い O_DIRECT を求めたら EINVAL。
- F_SETFL の mask を `O_APPEND|O_NONBLOCK|O_DIRECT` に広げ、block 以外で O_DIRECT なら EINVAL。F_GETFL は変えない。
- offset と長さは logical block（`d_block_size`）の倍数でなければ EINVAL（Linux と同じ）。長さ 0 は 0。device の終わりは今の `devfs_block_io_range_prepare` のまま（読みは切り詰め、書きは ENOSPC）。
- buffer の address の揃いは求めない（P1 の決定。Linux は logical block の揃いを求めるが、zedBSD は揃わない buffer を staging で扱えるので、より緩い。揃った buffer だけが zero-copy になる）。
- dd の iflag/oflag=direct は今回は足さない（試験は道具 `plan/ws299/tests/directio.c`）。dd に足すかは Q1 へ。

### 経路

- `struct file_ops` に `pdirect(file, write, const struct uaccess_pin *, size_t pin_off, size_t len, off_t off)`、file.c に `file_io_transfer_pinned`。syscall の 4 箇所（read・write・positional・vector）は O_DIRECT の block の時だけ bounce の loop を飛ばしてこれを呼ぶ。
- A（zero-copy）: pin の先頭が page の頭、範囲の page が全部 `VMSPACE_PINNED_PRIVATE`、leaf が DISK_PAGE_VECTOR、長さが page の倍数 → paddr の配列で page 列の DMA。OBJECT（MAP_SHARED）・DEVICE の page は外す。
- B（staging）: kern_pmem の page の 1 MiB × 4 の pool（初めて使う時に確保して保つ）に page 列で DMA し、`copyout_pinned`・`copyin_pinned` で 1 回だけ写す。4 KiB の倍数でない端と page 列を持たない disk は `disk_transfer_direct`。kernel の buffer を渡す呼び手も O_DIRECT の時は B。
- disk.c: `disk_direct_enter`・`leave`（`disk_cache_enter` と同じ受け入れ: reload・admin の除外）、claim を引数に取る page 列の転送（BLKRESERVE の fd が自分の claim と衝突しないように）。

### cache との整合

- buf.c に `buf_direct_prepare(disk, blk, cnt, evict)`（範囲の line を hash で引き、busy・io_inflight は待ち、dirty は書き戻し、書きの時だけ evict）と `buf_direct_finish`（書いた後に clean の line をもう一度 evict）。leaf に cache の line が 0 なら素通り。`buf_invalidate` は disk 全体の sync と O(cache) の walk・使用中で EBUSY なので使わない。
- 読みは prepare（evict なし）、書きは `backing_mutation_begin_disk` → prepare(evict) → DMA → finish。p005 の遅らせた raw の書きの dirty は prepare が書き戻す。direct の道は `buf_readahead_observe` を呼ばない。buffered と direct の fd を同時に同じ範囲に使った時の結果は決めない（Linux と同じ）。

### 誤り・signal

- BIO の確かめた先頭（`completed`）を返し、0 なら -errno。BIO 1 本は待ち終えるまで割り込まない（DMA が止まるまで page を手放さない）。1 MiB を越える時は BIO の間で signal を見て短い数を返す。

## 受け入れ

1. host 試験（揃い・道の選び A/B/端の純関数、prepare）。
2. QEMU（NVMe 512 B・4 KiB の sector、virtio-blk）: `directio.c` で書いて読んで cksum、buffered の書きの直後の direct の読み、cache に載った範囲を direct で書いた後の buffered の読み、EINVAL（offset・長さ・regular file・F_SETFL）、揃わない buffer・MAP_SHARED の buffer での正しさ、fork と並べた読み。io_stats に A・B の道の数。
3. 5330: direct の読み 4 GiB が Debian の `dd iflag=direct bs=1M`（2.1 GB/s）以上、direct の書きが Debian の `oflag=direct`（同じ LBA・大きさ）の 95% 以上。乱読み 4 KiB の QD1・QD32 も direct で Debian の fio direct と比べる。
4. build の warning 0。

## 危険

- 1 MiB ごとの `uaccess_pin` の費用は未測定。O_DIRECT を `#ifdef` で使う移植物が regular file で EINVAL を受ける。HAL の API は変えない。

## レビュー（2026-10-28、design-reviewer）と反映の方針（実装の時に守る）

- H1 → 「block だけ」の検査は open で inode を決めた直後（`file.c` の作成・O_TRUNC の切り詰めより前、:340 の付近）に置く。`i_descriptor_alias`（`/dev/fd/N`）だけは除き、`file_substitute_descriptor` で元の description の O_DIRECT と比べる。flag の mask は :256 だけを広げる。
- M1 → open・F_SETFL・syscall の振り分けは `i_type` でなく `f_ops->pdirect != NULL` で判定する（tmpfs の mknod の block node は `i_fop` が NULL）。
- M2 → 書きは、journal の pin か参照の残る line に重なる時、短く待ってから EBUSY。読みは `buf_writeback_cached` と同じ規則。finish は誤りの道でも必ず走らせ、io_inflight の line も待ってから追い出す（先読みの worker が prepare の後に line を入れる窓）。
- M3 → O_SYNC・O_DSYNC の direct の書きは device の flush だけ（disk 全体の `buf_sync` をしない）。試験で確かめる。
- M4 → BLKRESERVE の claim の fd は今の block_pread と同じく `disk_admin_io_begin` を使い、prepare をしない。
- M5 → 1 MiB を越える要求は BIO を 2 本重ねる。受け入れに bs=4M を足す。B（staging）の道は受け入れの速さの対象から外す。
- M6 → 5330 の direct の書きは p7（`zedBSD-nvme-bench`、PARTLABEL と PARTUUID を確かめる既存の `p7_check`）だけ。
- 低: Linux 6.x の buffer の揃いは dma_alignment（STATX_DIOALIGN）で、上の「logical block の揃いを求める」は不正確。device の終わりを越える書きは Linux が短い数、zedBSD は ENOSPC（違いとして記録）。io_stats に道の数を足すなら末尾に足し VERSION を上げ userland の sysctl も直す。mutation の slot（上限 64）を書き 1 回で 2〜3 個使う。staging の pool の確保の失敗・全部使用中の fallback を書く。ws.md の表と依存は Q1。`directio.c` が O_DIRECT を見るには sysroot の header の更新が要るかもしれない（toolchain の範囲、Q1 の許可の要否を確かめる）。libc の O_RSYNC の comment（`include/libc/fcntl.h`:17）を直す。
- 足す試験: 既存の regular file に O_TRUNC|O_DIRECT で中身が残る、tmpfs の mknod の node、`/dev/fd/N`、root を mount したままの whole disk の direct、signal の短い数が block の倍数、転送中の munmap、誤りの注入、O_DSYNC の flush、BLKRESERVE の fd、F_SETFL の切り替え、512 B の sector の 4 KiB に揃わない offset、coherent でない arch（arm64・riscv64 の QEMU）の B の道、MAP_SHARED|MAP_ANONYMOUS の buffer。

## 実装（2026-10-28、P1、q1646、b809c70d1）

- 2026-10-28 Q1: file.c（open の検査・F_SETFL）・syscall.c（bounce の振り分け）・include/kern/file.h（pdirect）を p010 の範囲として触ってよい。
- UAPI: `O_DIRECT 0x100000`（include/uapi/fcntl.h）。libc は変えない（flag をそのまま渡す）。
- open（file.c）: mask に O_DIRECT。inode を決めた直後・O_TRUNC の前に `i_fop->pdirect` が無ければ EINVAL（regular・tmpfs の mknod の node も）。O_CREAT で新しく作る時も作る前に EINVAL（何も作らない）。`/dev/fd/N` は元の description に O_DIRECT が無いのに求めたら EINVAL。`file_open_resolved` は O_DIRECT を受けない（今の mask のまま EINVAL）。
- F_SETFL（syscall.c）: mask を `O_APPEND|O_NONBLOCK|O_DIRECT` に、pdirect の無い file に O_DIRECT は EINVAL。
- syscall（read・write・pread/pwrite・readv/writev）: `file_io_direct(file)` の時は bounce を取らず、`file_io_transfer_pinned`（file.c）で要求の全体を pdirect に渡す（vector は element ごと、短い element で止める）。O_SYNC・O_DSYNC の file_fsync（disk 全体の buf_sync）はしない（M3、pdirect が device の flush だけをする）。
- devfs `block_pdirect`: offset・長さが logical block の倍数でなければ EINVAL、長さ 0 は 0、範囲は今の `devfs_block_io_range_prepare`（読みは短く、書きは ENOSPC）、1 回は 0x7ffff000 byte まで（Linux と同じ上限、越える分は短い数）。claim の fd は `disk_admin_io_begin` で cache を見ない（M4）、他は `disk_direct_begin`（`disk_cache_enter` と同じ受け入れ）→ 書きは `backing_mutation_begin_disk` → `buf_direct_prepare` → 1 MiB ごとの piece → 書きは `buf_direct_prepare(…, 1)` をもう一度（finish、誤りの道でも）→ O_SYNC・O_DSYNC の書きは `bio_flush` だけ。piece の間で signal を見て、動いた数（block の倍数）を返す。
- A（その場）: piece の頭が利用者の page の頭で、page が `VMSPACE_PINNED_PRIVATE` の間、`disk_transfer_pages_claimed`（disk.c、`disk_transfer_pages_context` に claim を足した形）で page 列の DMA。読みは終わった page を `vm_private_page_mark_dirty`（copyout と同じ）。MAP_SHARED・device の page で piece を切る。
- B（staging）: page の頭でない buffer・page 列を持たない disk・shared の page は `kern_malloc` の staging（piece ごと、最大 1 MiB）と `disk_read_direct`・`disk_write_direct(_claimed)`、`copyin_pinned`・`copyout_pinned`。P1 の決定: 設計の「1 MiB × 4 の pool」は作らない（B は速さの受け入れの外、M5。pool の確保・使用中の fallback の扱いが要らない）。
- piece の切り方は純関数 `include/kern/buf-direct.h`（`buf_direct_misaligned`・`buf_direct_page_piece`・`buf_direct_staged_piece`）: 512 B の sector で buffer が page の途中なら次の page の頭までを staging、その先を A。
- cache との整合 `buf_direct_prepare(disk, block, count, invalidate)`（buf.c）: 範囲の line を `BUF_GATHER_HOLD`（64）ずつ cache_lock の 1 回の保持で hash から引き、1 本ずつ busy を取り、転送中（io_inflight）なら待ち、`buf_writeback` と `unjournaled_write_line`（`buf_writeback_cached` と同じ）で書き戻し、invalidate の時は dirty が残れば（journal の pin）EBUSY、無ければ `BUF_VALID` を外して generation を進める（view も無効になる）。line を持ったまま他の line を待たない。
- P1 の決定: 「BIO 2 本を重ねる」（M5）はしない。NVMe の submit は呼び手の thread で BIO を終わりまで流す同期の形で、1 本の thread から 2 本を同時に出せない。1 MiB の BIO は driver が 256 KiB × 4 を同時に出す（p004）。5330 の数で足りなければ見直す。
- io_stats に A・B の道の数は足さない（UAPI の VERSION を上げない）。

## 試験（2026-10-28、P1）

- host: `plan/ws299/tests/host/buf-direct-test.c` PASS（揃いの判定、aligned 4 MiB は 1 MiB × 4 をその場、512 B ずれは頭と尾を staging、奇数の address は全部 staging、4 KiB の disk で 512 B ずれは staging）。
- build: config-5330（`build/p1-p010b`）・pcat・visionfive2・rpi4・m68k-virt で rc 0・warning 0。
- QEMU `plan/ws299/tests/qemu-directio.sh`（serial の image に `directio`、NVMe 512 B の disk と AHCI の disk）: PASS。`directio check` の全項目が両方で ok: regular file の EINVAL、O_TRUNC|O_DIRECT で中身が残る、O_CREAT|O_DIRECT で作られない、F_SETFL の EINVAL と切り替え、揃わない offset・長さの EINVAL、長さ 0、aligned 1 MiB・4 MiB、buffered の dirty の後の direct の読み、cache に載った範囲の direct の書きの後の buffered の読み、dirty の上の direct の書き、512 B ずれ・奇数の address の buffer、MAP_SHARED|MAP_ANONYMOUS、readv/writev と offset、O_DSYNC、`/dev/fd/N`（足すのは EINVAL、同じ description は ok）、device の終わり（読みは短く、書きは ENOSPC）、fork と並べた読み。QEMU の direct の 256 MiB: 書き 133 MB/s、読み 4 MiB 3016 MB/s・1 MiB 3158 MB/s（file を背にした QEMU の数で比べない）。
- 回帰（O_DIRECT の無い道）: `boot-test.sh` PASS（`build/tmp/boot-test-p010/login.png`）、`qemu-nvme-shapes.sh` PASS（5 checksums equal）。
- 未実施: 転送中の munmap、signal の短い数、誤りの注入、BLKRESERVE の fd の direct、root を mount したままの whole disk の direct、coherent でない arch（arm64・riscv64 の QEMU）の A の道。

## 5330 の測定（2026-10-28、P1、kernel `build/p1-p010b/vmunix` = b809c70d1、p7 は測る前に Debian から blkdiscard、fstrim・休みは今までどおり）

| 測る物 | Debian（`deb-d-p7direct`、directio） | zedBSD（`zb-d1`・`zb-d2`） | 比 |
| --- | --- | --- | --- |
| p7 direct 書き 4 GiB、1 MiB の要求＋fsync | 1434 MB/s | 849（zb-d2。zb-d1 は directio の出力の flush 漏れで数が無い） | 59% |
| p7 direct 読み 4 GiB、1 MiB | 2694 | 2558 | 95% |
| p7 direct 読み 4 GiB、4 MiB | 3039 | 2581 | 85% |
| data 範囲の direct 読み 4 GiB、1 MiB | 1555 | 1564 | 101% |
| 乱読み 4 KiB direct QD1（randread direct） | 57.8（read の基準、HMB on） | 57.9・57.0（fdeach 59.8・58.1） | 99〜100% |
| 乱読み 4 KiB direct QD32 | 964 | 473.8・467.2（fdeach 463.9・446.8） | 48% |

- 以前の受け入れの基準「Debian の dd iflag=direct 2.1 GB/s」は p7raw の後の数。同じ回の directio の Debian の数（上）と比べる。
- QD32 は direct でも cached（p006 の 424〜445）と同じ約 11 万 IOPS で止まる。CPU 12 個は約 8 割 idle、nvme の post→完了 平均 80 µs。全ての read/write の syscall が `uaccess_pin` → `vmspace_pin_user_pages`（src/kern/vm/vmspace-copy.c）で大域の mutex `metadata_lock`（src/kern/vm/metadata-lock.c）を取るのが有力な原因（読みで見つけた、数での確かめは WS308）。
- 5330 の停止: zb-d2 の測定（bench done・dmesg の回収まで）の後の reboot の命令の後、zedBSD が desktop を表示したまま入力を受け付けずに止まった（ユーザーの観察「入力受け付けない状態でデスクトップが表示されていました。」、ユーザーが電源を入れ直した）。同じ kernel の zb-d1 は reboot できた。QEMU（serial の image、8 CPU、NVMe）で同じ手順＋reboot を繰り返す `plan/ws299/tests/qemu-direct-reboot.sh` で 2 回とも正常に reboot（3 回目は途中で止めた）。途中で止まって見えた時の gdbstub では全 CPU が idle、thread は全て普通の待ち（serial の印の取りこぼしで guest は止まっていなかった）。原因は不明。2026-10-28 ユーザー「フリーズは再現したときに対策を考え直します。」

## 再開点（2026-10-28、P1、q1646 paused）

- 2026-10-28 ユーザー「仮想メモリのロックの変更は、P1の担当にして、一時的にそれに対応する別なWSを処理して、そのあとでNVMeのWSに戻りましょう。」→ q1646 は止め、WS308（VM の lock）の後に戻る。
- 残り: direct の書き 59% の切り分け（bs=4 MiB の書き、NVMe の command 128 KiB×8 で比べる。原因の分からない性能の問題は Fable 5.1 High に助言を求めてよい、2026-10-28 ユーザー）、4 MiB の読み 85%、QD32（WS308 の後に測り直す）、試験の残り（上の「未実施」）。5330 の停止は再現した時に考え直す。
- 道具: `randread` に `procs`・`notime`・`nullcall`（thread と process、syscall だけの比較）、`bench-zedbsd.sh qdprobe`、`qemu-qdprobe.sh`（kern.perf=1 の image で metadata lock の takes・waits）を足した（未実行）。

## 再開（2026-10-28 18:50、P1、q1646 を WS308 の後に再開）

- QD32 は WS308 で直った: 5330（b8ce0cdc3 の config-5330、`nvme-bench-5330.sh zedbsd w308c-q - qdprobe`、同じ時間帯の Debian は `plan/tools/kstress/debian-qdstat.sh`）で乱読み 4 KiB QD32 direct 224.2k IOPS（918 MB/s）＝Debian 225.8k の 99.3%、process 99.7%、cached 241.2k（988 MB/s）、QD1 direct 13.9k（Debian 13.6k）。記録 `build/hw5330/ws308c/`。→ 受け入れの乱読みは満たす。
- direct の書き 59% の切り分け: 今の kernel で p7direct の基準を取り直そうとした（`nvme-bench-5330.sh zedbsd w299c-z - p7direct`）が、zedBSD の起動の後 5 分答えず、5330 は ping・SSH とも返らない（18:46、Q1 に電源の入れ直しを頼んだ）。同じ kernel で今日 3 回（read・qdprobe）は正常に起動した。止まった所（起動の途中か、p7 の discard・書きの最中か）は未確認。
- 書きの仮説（未確認）: 1 MiB の要求は NVMe の 256 KiB の command 4 つを一度に出す（`NVME_IO_PAGE_COMMAND_BYTES`×`NVME_IO_PIPELINE_DEPTH` 4 = 1 MiB）ので、同時の数は Linux と同じ見込み。差の 1 MiB あたり約 0.5 ms（zedBSD 1.23 ms、Debian 0.73 ms）は、(a) command の大きさ（Linux は MDTS まで、BG5 の DRAM-less の書きは大きい command が有利かもしれない）、(b) `buf_direct_prepare` と finish の line の 2 回の walk と `backing_mutation_begin_disk`、(c) 1 MiB の piece の間の直列（user の QD1 は同じ）、(d) 書きの完了の割り込みと起床。次は 5330 で bs 4 MiB の書きと、command の大きさ 128 KiB・256 KiB・512 KiB（MDTS まで）で比べ、post→CQ の時間を取る。

## 2026-10-28 夜（q1657、P1）

- direct の書き 59% の差は、道具 directio の fill（長さを変数にした memcpy を 8 byte ごとに呼び、zedBSD の libc の memcpy が遅い）だった。kernel の 1 MiB の書きは約 0.47 ms で、I/O の時間は Debian と同じ程度。fill を直し、NVMe の 1 request を 4 MiB・同時 16 command にした（c9468203a）。5330: direct の書き 1384 / 1579 MB/s（88%）、読み 1 MiB 2554 / 2486（103%）、4 MiB 2809 / 3101（91%）。表と見立ては [ws.md](../ws.md) の「結果（2026-10-28 21:50）」。

## 2026-11-13（q2044、P2）: 電力制御の後の測りと読みの差の切り分け

- main fe6ea0f967 の kernel、5330（q2044a、[ws.md](../ws.md) の「2026-11-13」）: direct の書き 1 MiB 1512 / Debian 1575 = 96%、読み 1 MiB 2603 / 2750 = 95%、4 MiB 2863 / 3112 = 92%、data 範囲の読み 1 MiB 1562 / 1586 = 98%。
- 4 MiB の読みの差は 1 request（4 MiB）あたり約 100 µs、1 MiB では約 25 µs で、大きさに比例する。試した物（各回 Debian を同じ時間帯に測り直した）:

| 試した形 | kernel | zedBSD 4 MiB / 1 MiB | Debian 4 MiB / 1 MiB | 結果 |
| --- | --- | --- | --- | --- |
| command を 128 KiB ×32（`NVME_IO_PAGE_COMMAND_BYTES`、深さの上限 32、試験だけ） | 1be3b959fdb9f1c0（q2044c） | 2807 / 2572 | 3046 / 2721 | 効かない（92% / 95%）。この回は読み手が E-core に載った（pcore 0/1024） |
| ordinary idle を HLT だけ（`IDLE_WAIT_DEEPEST_CSTATE=1`、試験だけ） | 399d9300fa0e9da1（q2044d） | 2826 / 2572 | 3026 / 2742 | 効かない（93% / 94%） |
| pin が page ごとに region の list を頭から歩くのを、前の page の region から（`vmspace_pin_user_pages`、試験だけ、戻した） | 20ac3a4020731369（q2044e） | 2826 / 1900 | 3074 / 2592 | 4 MiB は効かない（92%）。1 MiB の 1900 は直前の書きの後の drive の状態と見る（未確認） |

- 残る候補（未確認）: 1 request の 16 command の完了ごとの割り込み（VMD の 1 vector）と待つ thread の起床の回数、request の間の隙間（完了 → user → 次の pin・post）。次は TSC で request の中の区間（pin・post・最後の完了・起床・unpin）を測る。
- 乱読み direct QD1 104%・QD32 97%（[phase006](../phase006/phase.md)）。

## 2026-11-15（q2108、P4）: QEMU の確かめ

- main ba1482f25d で `qemu-directio.sh`（今の build に直した suite、[phase007](../phase007/phase.md)）PASS: `qemu-directio: PASS (2 checks, 3 transfers)`（NVMe 512 B と AHCI の両方で `directio check` の全項目、direct の 256 MiB の書きと 4 MiB・1 MiB の読み）。host `buf-direct-test` PASS。x86_64・rpi5・vf2 の kernel の build warning 0。
- 受け入れ 2 の「NVMe 4 KiB の sector、virtio-blk」の disk は `qemu-directio.sh` に無い（NVMe 512 B と AHCI）。未実施のまま。受け入れ 3（5330 の direct の読み 4 MiB 92〜93%・1 MiB 94〜95%）が残る。Status は変えない（in-progress、5330 の段）。
