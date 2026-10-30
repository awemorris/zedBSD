<!-- awesome-plan project=zedbsd record=ws299-p005 -->

# ws299-p005 UFS の file の連続の読み書き（先読みの窓・大きな request・書きの合流）

- 親: [WS299](../ws.md)。Queue: q1634 / q1634-i01（調べ）→ q1640 / q1640-i01（設計・実装、担当 P1）。前提: p002・p004（raw の先読み）。
- Status: in-progress（2026-10-31 P1 q1744: 書き手の CPU の直し 3a4e64353（LRU の回し・束の書き・1 MiB の run・memcpy）、QEMU で確かめ、5330 の測りは電源の戻り待ち。2026-10-28: raw の読み書き・file の読みは Debian 超え、file の書きは 55%）
- Disposition: normal

## 調べた事実（2026-10-28、読みだけの調べ、tree は agent/p1-ws299 36d0d7b5d）

- 読み（`dd if=/mnt/file bs=1m`）: `sys_read_call` が 1 MiB の bounce（`src/kern/syscall.c:93`・2851-2862）→ `file_io_transfer` → VM object の cache（`src/kern/file.c:1199-1209`・3037-3075、`vm_object_read_coherent_useful` `src/kern/vm/object-content.c:587`）。miss は 1 回 64 KiB（`object-cache.c:816` `KERN_IO_BATCH_MAX`、`VM_OBJECT_CACHE_PAGES` 16）で、`io_pool` の scratch へ読み（:856・962）、scratch → page（:987）、scratch → syscall の buffer（:1009）。
- UFS: `pread_inode`（`src/drivers/fs/ufs.c:6107`）、連続の run は `KERN_IO_BATCH_MAX / bsize` まで（:6047-6060）、block ごとに `bmap`（:6087、間接は 512 byte の metadata 読み :5292）。disk へは `disk_read` → `buf_read` → 64 KiB の page 列（buffer cache の line と VM の page の二重の cache）。1 MiB の read で 64 KiB の同期の request が 16 回、QD1、写しが 4 回。
- 既存の file の先読み（`src/kern/readahead.c`）は UFS にも効くが、1 回 64 KiB（`READAHEAD_REQUEST_MAX`）、demand の I/O の間は始めない（:773）、disk ごとに worker 1 本・slot 2（:319）、scratch に読んで写す（:819・832）。1 MiB の読みにはほぼ効かない。
- 書き（`dd of=/mnt/file bs=1m`）: 伸びる書きは毎回 UFS へ（`file.c:1810-1826`、`ufs.c:8655`）、新しい block の割当は 64 KiB ごと（`UFS_ALLOCATION_BYTES`、`ufs.c:165-166`・7315-7612、cylinder group・leaf・inode を毎回）。j3 の既定では content は `buf_write_context` で line を dirty にし（dirty 32 MiB まで、`buf.c` `BUF_DIRTY_LIMIT`）、それを越えると 64 KiB の同期の書き。flusher は 1 line（4 KiB）ずつ同期で書く（合流なし）。
- p004 の raw の先読み（`buf_readahead_observe`）は devfs の raw の open だけに効き、UFS の読みには効かない（mount された区画には先読みしない、`disk_readahead_private`）。
- 5330 の今の数（`evidence/zb-p004f-summary.md`）: UFS の書き 353・378 MB/s（Debian の ext4 の書き 1.6 GB/s、`evidence/deb1-summary.md`）。UFS の file の読みは zedBSD・Debian とも未測定。

- 2026-10-28 追記（P1、`zb-p004h` の io-stats を事象ごとに分けた）: UFS の 256 MiB の書きで `IO_BUF_WRITE` は 4440 回（1 回 約 60 KiB、遅らせる書き）、`IO_DRIVER_WRITE` は 57350 回 × 4 KiB。dirty が上限 32 MiB に達した後、`buf_write_context` は line ごとに「遅らせる → 印を付けた後に上限を越えた → その line だけを同期で書く」になり、4 KiB の同期の書きが 224 MiB / 4 KiB ≒ 57344 回並ぶ。flusher も line ごと（`flush_aged` → `buf_writeback` → `writeback_line`、4 KiB）に書く。`IO_BUF_SINGLE_GEOMETRY` 65357 回は書きではなく、4 KiB の書きごとの UFS の間接 block の窓の読み（`IO_UFS_INDIRECT_WINDOW` 64610 回 × 512 byte）。
- 2026-10-28 追記: 測り直し（fstrim、p6 の discard、休み、`plan/ws299/tests/bench-*.sh` の改訂、下の「測定の条件」）で、Debian の ext4 の 1 GiB の冷えた読み 1.5 GB/s、ext4 の 1 GiB の書き（fsync 込み）1.5 GB/s、raw の 1 MiB の direct の書き 1.6〜1.9 GB/s（256 MiB ずつ）、page cache 経由の 1 GiB（fsync 込み）1.5 GB/s。

## 測定の条件（2026-10-28 改訂、ユーザー「P1はfstrimを使った上で、SSDの消去の影響をなるべく排除して、再度目標達成まで継続してください。」）

- 以前の読みの測定の位置（disk の 100 GiB の辺り）は Debian の ext4 の空き（block の 97% が未使用）で、週ごとの `fstrim.timer`（2026-10-27 00:31 に走った）で trim 済みだった。trim 済みの block は drive が flash を読まずに返すので、読みの数は実際の flash の読みではなかった（Debian・zedBSD とも）。
- 読みの位置を、Debian の ext4 の上の 12 GiB の乱数の file `/home/awe/ws299/data.bin`（2026-10-28 作成、消さない）の extent が並ぶ範囲にした: disk の 25681 MiB から 8064 MiB（p2 の block 6324224〜8388607。間の 128 MiB の隙間 3 つと 8 MiB の 2 つは ext4 の inode の表などの使用中の block）。`R0_MIB`・`R_MIB`。
- 各 mode の前に Debian で `fstrim -av` と 60 秒の休み。書きの各段の前に p6（`zedBSD-nvme-swap`、PARTLABEL・番号・Debian の swap・mount でないことを確かめる）を `blkdiscard` し、swap の頭 4 KiB だけを書き戻す（`bench-debian.sh p6fresh`）、30 秒の休み。p1〜p5 は discard しない。zedBSD の UFS は、同じ file を上書き（`ufsover`、同じ LBA）と、消して新しく書く（`ufsnew`、UFS は TRIM を送らないので、以前に書かれた空きの LBA に書く）の両方を測る。
- 乱読みは `plan/ws299/tests/randread.c`（Linux と zedBSD の両方で build、thread の数 = 深さ、各 thread は pread を 1 つずつ）。Debian は direct（`O_DIRECT`、fio と同じ数になる事を確かめた）を基準にする。Debian の page cache 経由（`cached`）は 8 GiB の範囲の大半が RAM に載るので比べない（参考）。zedBSD は buffer cache（1 GiB）を通るので、その hit の数を snap で記録する。

## 設計（2026-10-28、P1）

### D1. dirty の上限を cache の大きさに合わせる（`src/kern/buf.c`）

- `BUF_DIRTY_LIMIT`（32 MiB 固定）を、cache の上限の 1/4 にする（`dirty_limit`、`buf_init` と `buf_set_max_bytes` で計算、`cache_lock` の下で書き、読みは atomic）。5330（cache 約 985 MiB）で約 246 MiB。cache が 32 MiB の機械（RAM 256 MiB）で 8 MiB になり、dirty が cache を埋め尽くして新しい line が取れなくなる事が無くなる（p004 の 32 MiB の cache の書きが止まった件）。
- 背景の書き戻しの閾値 `dirty_background` = `dirty_limit / 2`。

### D2. 隣り合う dirty の line をまとめて書く（cluster）

- `buf_writeback(buffer)`（遅らせた書きを書き手の代わりに書く口。flusher・`buf_sync`・`buf_writeback_range`・`buf_writeback_cached`・reclaim が使う）を `writeback_cluster` にする。呼び手は buffer の busy と参照を持つ。
- 条件（全部を満たす時だけまとめる。満たさなければ今の `writeback_line(buffer, NULL, 1)`）: leaf が page 列を受ける（`disk_page_vector_supported`）、block の大きさが page 以下、line が page の大きさ（`b_memory.size == KERN_PAGE_SIZE`、`b_block_count` が 1 page）、dirty・valid・journal の pin が無い。
- 前と後ろへ、`hash_find_locked` で同じ leaf の隣の line を `cache_lock` の 1 回の保持で参照（`refcount_get`・`lru_remove_locked`）し、各 line の `b_lock` の下で「busy でない・inflight でない・dirty・valid・pin なし・page の大きさ」なら busy を取る（待たない。だめな line で止まる）。最大 `BUF_CLUSTER_LINES` = 256（1 MiB、NVMe の BIO の上限と同じ）。
- 範囲全体に他の claim（swap file・loop の image）の block があれば（`backing_claim_find_extent_owner`）まとめず、buffer だけを今の道で書く（BUG-073 の扱いを保つ）。
- 書き: 各 line の `b_dirty_generation` を記録して `BUF_IO_WRITING`・inflight、`backing_mutation_begin_disk_filesystem`（範囲全体、今の `writeback_line_whole` と同じ guard）→ `disk_transfer_pages_context(BIO_WRITE)` → guard の終わり。完了した prefix の line は `finish_run_line`（世代が同じなら clean、`b_unjournaled = 0`）、残りは誤り（dirty のまま、今の失敗と同じ）。guard の失敗・driver の `EOPNOTSUPP`（何も送っていない）は、line を idle に戻して buffer だけを今の道で書く。
- 後ろへ広げるので、古い line が範囲の中ほどでも run の頭から書ける。他の line の busy は取れなければ待たないので、「line を持ったまま他を待たない」規則を保つ。
- `IO_BUF_RUN_WRITE` に数える（cluster の書き。今の write-through の run と同じ事象）。

### D3. 背景の書き戻しと書き手の手伝い

- dirty が `dirty_background` を越えたら、書き戻しの worker を起こす。worker は p004 の先読みの worker（`BUF_READAHEAD_WORKERS` = 4）と同じ thread を使い、先読みの仕事が無い時に「一番古い、pin の無い、busy でない dirty の line」（`dirty_head` から try-busy、待たない）を cluster で書く。dirty が `dirty_background / 2` 未満になるか、書ける line が無くなるまで続ける。書いた leaf は記録し、仕事が尽きた時に `bio_flush`（今の flusher の pass と同じ耐久の扱い）。
- `buf_write_context` の遅らせる書き（`DISK_WRITE_CACHED` の disk）で dirty が `dirty_limit` を越えたら、書き手は line を持つ前に、古い dirty の cluster を自分で（try-busy、待たない）書いて `dirty_limit` 未満に戻す（手伝い）。書ける line が無ければ今の形（その line を同期で書く）に落ちる。眠って待つ事はしない（UFS の lock を持った書き手が、worker の進みを待って止まらない）。
- 今の「上限の後は line ごとに同期で書く」形は、手伝いが何も書けない時の最後の道として残る。

### D4. raw の write-through の run を大きく、NVMe で並べる

- page 列の run の最大（`BUF_RUN_PAGE_LINES`）を 64（256 KiB）から 256（1 MiB）に。raw の 1 MiB の write は 1 つの BIO になる。先読みの仕事の大きさは `BUF_READAHEAD_RUN_BYTES`（256 KiB）のまま分ける。
- NVMe: page 列の 1 command を `NVME_IO_PAGE_COMMAND_BYTES` = 256 KiB までにし、1 MiB の BIO を同時に 4 command（`NVME_IO_PIPELINE_DEPTH` = 4）で流す（Linux の 5330 は `max_sectors_kb` 128）。copy と device の書きの重なりはこの段では作らない（測って足りなければ D4b: write-through の run を worker に渡して次の run の copy と重ねる）。

### D5. UFS の file の先読み（buffer cache へ、`src/drivers/fs/ufs.c`・`src/kern/buf.c`）

- buf.c に `buf_prefetch(disk, block, count)`: 呼び手（file system）が自分の data の block と知っている範囲を、先読みの worker の仕事にする（流れの検出・`disk_readahead_private` は通さない。file system の demand の読みと同じ line を作るだけなので、direct の書きで古くなる危険は demand の読みと同じ）。止め（D3 of p004）・admission・memory の扱いは p004 の仕事と同じ。queue が満ちていれば頼まない。
- UFS: inode ごとの流れ（`next`・`issued`・`sequential`、inode の私有の構造、更新は spinlock）。`pread_inode` の読みが前の読みの終わりから窓（2 MiB）の内なら連続と数え、2 回続いたら `[max(issued, 読みの終わり), 読みの終わり + 窓)` を `bmap` で disk の run（最大 256 KiB）に分けて `buf_prefetch`。VM の先読み（`readahead.c`、64 KiB、demand より少し先）の読みも同じ inode の読みとして来るので、「窓の内」で連続とみなし、`issued` は単調に進める。
- 穴・fragment の末尾・間接 block の境は run を切る。bmap の誤りは先読みをやめるだけ。

### D6. VM の miss の run を大きく（`src/kern/vm/object-cache.c`、`KERN_IO_BATCH_MAX` の使い方）

- VM の object の miss の run（今 64 KiB = `KERN_IO_BATCH_MAX`、16 page）と UFS の連続の run（`content_run_bytes`、64 KiB）を、それぞれの定数（`VM_OBJECT_CACHE_RUN_MAX`・`UFS_CONTENT_RUN_MAX`）に分けて 256 KiB にする。`io_pool` の scratch が 256 KiB を貸せない時は今の 64 KiB に落ちる。D5 の後に測って、効く時だけ入れる（2026-10-28 Q1 の範囲の決定: VM と UFS が共有する 64 KiB の上限を変える事は WS299 の中で P1 が決める）。

### D7. 書きの metadata の読み

- 4 KiB の書きごとの間接 block の窓の読み（`IO_UFS_INDIRECT_WINDOW`）を、D1〜D3 の後の測定で sys の時間が目立つ時だけ減らす。

### レビュー（2026-10-28、design-reviewer）の反映

- H1（書けない line で worker が回る・他の device に縛られる）→ 書き手の手伝いは自分の leaf の line だけ（`d_dirty_buffers`）。worker と手伝いは `BUF_ERROR` の line を選ばない（flusher が今どおり次の間隔に試す）、worker は `d_media_revoked`・止め（`d_readahead_stops`）の leaf を選ばない。書きに失敗したら worker の pass を終え、`writeback_wanted` を下げる（次の遅らせる書きが上限の半分を越えた時にまた頼む）。手伝いの誤りは書き手に返さない（line は dirty・error のまま）。
- H2（管理の操作の偽 EBUSY）→ worker は書く間 `worker->running = leaf` を立て（止める側の wait=1 は待てる、wait=0 は今どおり EBUSY で再試行）、line を取った後に readahead_lock の下で止めを見直し、止められていれば line を返して書かない。`buf_invalidate*` には今どおり止めを入れない（flusher も同じ窓を作る。試験は下）。
- M1 → `mark_dirty` の後に上限を越えた時も、まず手伝い、それでも越えていればその line を今どおり同期で書く（最後の道）。行の先頭の write-through（上限越えの `transfer_run`）の前にも手伝う。
- M2 → worker は最初の先読みか、遅らせる書きが上限の半分を越えた時（`writeback_kick`、lock を持たず line も持たない所、`buf_release` の後）に作る。`writeback_wanted` は lock の外で先に見て、既に頼んであれば lock を取らない。
- M7 → worker は device を flush しない。書いた leaf を（`disk_ref` を持って）記録し、flusher が次の間隔（1 秒）に `bio_flush` して release する（耐久の遅れは今の flusher と同じ 1〜3 秒）。
- M8 → worker の探しは古い方から 1024 line（`BUF_WRITEBACK_SCAN`）まで見る（他の worker が書いている 256 line の cluster を越えて次の run を見つける）。cluster の集めは一人ずつ（`cluster_picking`、集める者は待たないので待ちは短い）なので、二人が一つの run を分けない。
- M9 → cluster の参照は cache_lock の 1 回の保持で 64 個まで（`BUF_GATHER_HOLD`）、dirty でない line で止める（印は hint、`cluster_take` が b_lock の下で見直す）。
- M10 → 上限を cache/4 にするのは page 列を受ける disk への書きだけ。他の disk（SD・USB・virtio 等）への遅らせる書きは今の 32 MiB（`BUF_DIRTY_SMALL`、cache/4 の方が小さければそれ）を保つ。
- M11 → 受け入れ 1 に raw の連続の読み 4 GiB と reread の回帰なしを足す。
- M3〜M6（D5・D6）→ 読みの設計は p7 の区画での測定（下）の後に書き直す。io_pool の slot が 64 KiB 固定なので D6 の「256 KiB の scratch」は成り立たない。代わりに大きな読み（残り 256 KiB 以上）は VM の page に載せず caller の buffer に直に読む（既存の非公開の道、写しが 2 回減る）案と、UFS が自分の data の block の先読みを buffer cache に頼む口（owner 無しの仕事、止めだけを見る）を検討する。
- 低 1・2・4・6 は実装で守った（呼び手の line は release しない、b_lock を dirty_index_lock の下で取らない、claim は範囲全体を `backing_claim_find_extent_owner(…, NULL, …)` で見る、cluster の数は `IO_BUF_RUN_WRITE`）。低 3（範囲の呼び手の cluster が範囲の外へ広がる）は正しさを変えないので受け入れ、fsync の遅れは測る。低 8 の開発の管理情報は p008 で消す。
- 欠けている試験 → 32 MiB の cache の書き（`qemu-cache32-write.sh`、元の kernel 73135a9ec でも止まる事を確かめた）、書きの最中の reload・admin open（`qemu-cache-stress.sh` に UFS の書きと reload が入っている）、QEMU の boot・nvme-shapes・grow-reboot・readahead。誤りの注入・crash の整合・claim の隣・NVMe 以外の機種の書きの時間は未実施（下の結果に書く）。

## 測定の区画（2026-10-28、ユーザー「P1は、UFSのパーティションが非常に小さいため、そこへの読み書きでは正確な計測ができないです。32GBくらいの空のパーティションを作って、それに対する読み書きをDebianとzedBSDで比較する必要がありそうです。」「USBメモリを挿入しましたので自由に書き換えて起動していいです。ext4を縮小して32GBの空きパーティションを作ってください。容量に余裕があれば、zedBSDのUFSも32GBで作り直すのがいいと思います。」）

- SystemRescue の live USB（`5330-live-usb.sh`、stick に `~/ws245/live-sysrescue.img` を書き直した）で: `e2fsck -f -p` → `resize2fs 148480M` → p2 を 314214400 sectors（149.8 GiB、start・UUID 不変）→ `resize2fs`（満たす）→ `e2fsck -f -n` clean。p7 `zedBSD-nvme-bench`（start 316215296、67108864 sectors = 32 GiB、Linux filesystem 型、PARTUUID `1BEB91C6-DEDF-4AF0-B132-4770CCF2954D`）を足し、p5 `zedBSD-nvme-root` を同じ番号・PARTUUID・名前のまま start 383324160・32 GiB に移した（前の 14.75 GiB の場所 450957312〜481890303 は空き）。p1・p3・p4・p6 は不変。`sfdisk -V` No errors。Debian は起動し `/` rw・147G・p3 の swap・running。前後の表は worktree の `build/hw5330/part/`（git の外）。
- `5330-run.sh --nvme-image build/p1-5330img/hdd-image.img p1-q1640-p5-32g` で zedBSD を新しい p5 に書き、NVMe から起動して `5330-run: PASS`（ssh まで 20 秒、i915 の survey PASS）。
- 測定（`nvme-bench-5330.sh … p7raw p7fs`・`p7rawwrite p7rawread p7fswrite p7fsread`）: 書きの前に Debian から p7 を `blkdiscard`。`patwrite.c`（両方の OS で同じ、1 MiB の write で 4 KiB ごとに番号を入れた疑似乱数、最後に fsync）で P7_MIB（4096）MiB を raw に書き、次に（zedBSD は次の起動で）冷えた読み。file system は Debian が ext4（`mkfs.ext4 -E nodiscard,lazy_itable_init=0,lazy_journal_init=0`）、zedBSD が UFS（`mkfs -t ufs --profile=native`）を p7 に作り、4 GiB の file を書いて（fsync）冷えた読み。

## 結果（2026-10-28、P1、5330 の p7、Debian の同じ条件の数と比べる）

| 測る物 | Debian | zedBSD main（`zb7-base`） | zedBSD この Phase（kernel） | 比 |
| --- | --- | --- | --- | --- |
| raw 4 GiB 書き（page cache・遅らせる書き＋fsync、`patwrite`） | 1549 MB/s | 1302 | 1837（p006c、遅らせる raw の書き 114f6d405 の後） | 119% |
| raw 4 GiB 冷えた読み（cache 経由） | 1.2 GB/s | 1.8 GB/s | 1.80 GB/s（p006c） | 150% |
| file 4 GiB 書き（fsync 込み） | ext4 1775 | UFS native（tail journal）7 | UFS（root と同じ batched journal、p7ufs）983（p006a・p006c） | 55% |
| file 4 GiB 冷えた読み | ext4 1.2 GB/s | 330 | 1518（p006c、bmap の memo の後。memo の前の p006a 1060、p005d 956） | 126% |

- allocation run を 1 MiB にした kernel（d173b958e、`zb7-g`）: UFS 書き 849 MB/s（64 KiB の 983 より遅い）、読み 1.55 GB/s。効かないので戻した（d847e644f、ufs.c は d173b958e の前と同じ）。
- tail journal の観測（Q1 が別に扱う）: zedBSD で `echo "FORMAT nvme0n1p7:8" | mkfs -t ufs --profile=native nvme0n1p7`（32 GiB）→ `mount -t ufs /dev/nvme0n1p7 /mnt/bench7` → `patwrite /mnt/bench7/f.bin 4096`: 566811 ms（7 MB/s）、`IO_DRIVER_FLUSH` 728057、`IO_UFS_ALLOCATE` 524811、`IO_UFS_WRITE` 1191860 回・13.2 GB（`build/hw5330/ws299/zb7-d-p7fswrite.txt`、main の kernel `zb7-basefs` も 567209 ms）。冷えた読みは main 330 MB/s、p005d 956 MB/s。
- 書きの経路の数（`zb7-e-p7ufswrite`）: driver の書き 10037 回・平均 430 KiB（cluster）、flush 30 回。書き手は CPU で律速（write 4024 ms、sys は約 1.4 CPU）。allocation run（64 KiB ごとの CG・inode の更新、`IO_UFS_ALLOC_BEGIN` 67922 回、UFS の metadata の buf_read 50 万回・buf_write 34 万回）が重い。→ run を 1 MiB にしても速くならなかった（上）。残りの差は未解決。
- `mkfs -t ufs --profile=native` の volume は tail journal（`journal_enabled`）で write-through になり（`write_cached_intended`、ufs.c）、block の割当ごとに `order_barrier` の flush（4 GiB で `IO_DRIVER_FLUSH` 728057 回）、書き 7 MB/s。zedBSD の root（image の UFS）は batched journal で遅らせる書き。mkfs の既定の形は userland の mkfs（所有の外）の事として Q1 に知らせた。
- raw の書きを遅らせる（114f6d405）: devfs の claim の無い open の書きは、開いた者だけが使う範囲（`disk_readahead_private`）なら `disk_write_delayed` → `buf_write_delayed` で cache に dirty で残し、cluster・worker・fsync・最後の close（`buf_sync`）で書く。共有の範囲は今どおり write-through。
- QEMU（`build/tmp/qemu-suite.sh` の形、2026-10-28）: boot-test rc 0、`qemu-readahead`・`qemu-nvme-shapes`・`qemu-grow-reboot`・`qemu-cache-stress`・`qemu-cache32-write` PASS（d173b958e の tree: 遅らせる raw の書き・HMB・bmap の memo・1 MiB の run まで全部 PASS。run を戻した d847e644f は build だけ）。build（d173b958e 以降）: config-5330・pcat・visionfive2・rpi4・m68k-virt で rc 0・warning 0。
- 未実施: 誤りの注入、crash の整合、claim の隣、NVMe 以外の機種の書きの時間（build だけ）。

## 2026-10-31（q1744、P1）: 書き手の CPU の直し

- 前提（2026-10-31 ユーザー: WS299 は 5330 が空いたらどの担当でもよい・不在の間は設計・実装・試験を進める、という趣旨）。5330 は最初の起動（main 9946e22c0 の kernel を WS291 の restore の後の古い userland に `--nvme-kernel`）で network が上がらず止まった（networkd と kernel の Wi-Fi の ioctl の形の違い、Q1 の見立て、電源の入れ直し待ち）。以下は QEMU（KVM）。
- 2026-10-28 の 5330 の counter の読み直し（`q57-Z-p7ufswrite`）: 4 GiB の file の書きの間に UFS の metadata の読み 2.87 GB（`IO_UFS_READ` 50 万回）・書き 2.3 GB、64 KiB の allocation run ごとに CG の読み直し（`IO_UFS_CG_MISS` 6.9 万回、hit 0）。cylinder group の free map の探しは run ごとに group の頭から。
- QEMU の測り（`plan/ws299/temp/qemu-prof.sh`、git の外。2 つ目の NVMe を /dev/shm の file、`patwrite` 2 GiB、guest 4 GiB で cache 512 MiB、`-mem-prealloc`。TSC の内訳の一時の patch `tprobe*.patch`）: main の形で `evict_one` が 1 回に平均 95 本の dirty・journal の pin の line を lock を取って歩く（10.2M 本 / 10.7 万回、半分が pin）。上書きの `acquire_line` が 2 GiB で約 2.7 s。64 KiB の run の新しい file では run の leaf の読みと metadata が約 0.35 s。
- 直し（3a4e64353）:
  - `buf.c`: `evict_one`（範囲の無い歩き）と `recycle_line_locked` が、取れない dirty・書き中の line を LRU の head へ回す（`evict_take_locked`）。歩きは始めの head で止める。範囲の有る evict は今のまま。
  - `buf.c`: P6 の `write_cached_lines`（agent/p6-ws299 の q1681、遅らせる書きの whole line を 16 本ずつ cache_lock の 1 回の保持で引き、1 回で返す）を取り込んだ。
  - `ufs.c`: allocation run を 1 MiB（128 block）に（10-28 の d173b958e と同じ定数。5330 で 849 MB/s と遅く出た件は下の 5330 で測り直す）。free map の探しを前の run の終わり（`alloc_hint_cg`・`alloc_hint_fragment`、無ければ group の頭から）から、8 fragment の block は byte で見る（`cg_free_blocks`）。
  - `kcrt`: `kern_memcpy_ram`（RAM どうしの写し、amd64 は 256 byte 以上を `hal_memcpy` の rep movsb、他の機種と host 試験は word の loop）。buffer cache・pinned の user page（`copyin_pinned`・`copyout_pinned`）・VM の page の写しに使う。device memory には使わない（uncached では byte ずつになる）。hal.h は変えていない。
- QEMU の数（2 GiB、MB/s、`tprobe` 入りの kernel、ぶれは ±10% ほど）: 64 KiB の run（hint・memcpy まで）新しい file 490・上書き 410・raw 743 → 1 MiB の run 715 → 回しを足して 854・738・800 → 束の書きを足して 851・852・835。main の kernel（probe なし）は新しい file 219〜409・上書き 733〜912（QEMU の I/O で頭打ち、CPU の差は 5330 で見る）。
- 確かめ: build config-5330・serial・cache128・cache32 warning 0。host: `buf-slab-test`・`buf-readahead-test`・`buf-direct-test`・`plan/ws299/tests/host/ufs-cgmap-test.sh`（新規、`cg_free_blocks` を素直な bit の歩きと 315 万通り比べる）・`run-kcrt-host-tests.sh`・`run-copy-sum-host-test.sh` PASS。QEMU（`plan/ws299/tests/qemu-suite.sh`、新規）: cache-stress・nvme-shapes・directio・cache32-write（400 MiB）・boot-test PASS。readahead は試験の disk が host の build/tmp（host の disk が満杯に近く遅い）だと main の kernel も NVMe の `I/O queue recovered` と `writeback_errors` で FAIL、disk を tmpfs（`WS299_RUNS`）に置くと PASS。ws299 の QEMU 試験は `WS299_RUNS` で run の置き場を選べる（suite の既定 /tmp/ws299-runs）。
- 訂正: 上の QEMU の suite の PASS は hint と memcpy までの kernel（回し・束・1 MiB の前）で流した物だった。最終の kernel で流し直した結果は下。
- 他の機種の build（3a4e64353）: pcat・visionfive2・rpi4・rpi5・m68k-virt の vmunix が rc 0・warning 0。
- コードの敵対的レビュー（design-reviewer、3a4e64353）: (1) **重大**: tail journal（v2、journal-snapshot の profile）の volume では run の中身を journal の 1 group（`UFS_JOURNAL_GROUP_SECTORS` 128 sector）で commit するので、1 MiB の run が `drv_ufs_journal_publishv` で EINVAL になり write が失敗する。QEMU で再現（host の `make-ufs-test-image.py --profile journal-snapshot` の 256 MiB の volume に 16 MiB の dd が "Invalid argument"、`plan/ws299/temp/qemu-v2-write.sh`）。→ v2 の volume では run を 128 sector までにした（4e7b2a827）。同じ試験で一致。(2) 束の書きが一度だめだと write の残り全部を 1 line ずつにしていた → 16 line ごとに試し直す。(3) 回しの comment の誤り（回した line は 2 周目を得る、と直した）。(4) wrap の 2 回目の探しを始めの hint までに。(5)〜(8) は軽い（`ms->lock` の保持が run の分だけ長い、j3 の hold の line の pin が run の大きさだけ増える、hit の数え方、`kern_memcpy_ram` の呼び手の buffer が RAM である事を buf.c は保証しない — 辿った呼び手は全部 RAM）。
- 他: P6 の q1681 の comment の WS の番号の削除（pci-nvme.c・dma.c/h・syscall.c・object-sync.c・ufs.c、p008 の一部）を取り込んだ（609295cd0）。
- 最終の kernel（4e7b2a827）の QEMU（disk は tmpfs、`WS299_RUNS=/tmp/...`）: `qemu-suite.sh` の cache-stress・readahead・nvme-shapes・directio・cache32-write（400 MiB）・boot-test 全部 PASS、`qemu-ufs-crash.sh` の 2・6・15 秒の切断 3 回とも kept・remount・check ok、v2 の volume の 16 MiB の書きが一致。
- 試験の追加: `plan/ws299/tests/qemu-ufs-crash.sh`（native の UFS に 72 MiB を書いて fsync、600 MiB の書きの途中で QEMU を kill、次の起動で journal の replay・72 MiB の cksum・新しい file の fsync と再 mount・host の `check-volume.py`）。

## 変えない物

- HAL の API、UAPI（io-stats の事象は増やさない）、UFS の形式・journal の順序（pin の line は書かない）、NVMe の recovery。

## 受け入れ

1. 5330: UFS の file の 1 GiB の書き（`ufsover`、sync 込み）と冷えた読み（`ufsread`）が、同じ条件の Debian の ext4（`ext4-w1m`・`ext4-r1m`）の 95% 以上。raw の書き（`raw0..3` と sync の和、`rawtotal`）が Debian の `b-w1m` の 95% 以上、各 256 MiB が `d-w*` の中央値の 95% 以上。`ufsnew` の数も記録する。
2. 32 MiB の cache の QEMU で UFS の 200 MiB の書きが止まらない（D1）。
3. QEMU: boot-test、`qemu-readahead.sh`・`qemu-nvme-shapes.sh`・`qemu-grow-reboot.sh`・`qemu-cache-stress.sh`、加えて UFS の書きと sync・reboot の後の cksum。
4. build の warning 0（config-5330・pcat・visionfive2・rpi4・m68k-virt）。host 試験（cluster の範囲の計算、UFS の流れの検出）。

## 計画の案（2026-10-28 q1634 の時点の案、上の設計で置き換えた）（設計の前の案。design-reviewer と Q1 の判断の前）

1. 測る: `bench-zedbsd.sh` に UFS の file の冷えた読み（write の段で書いた file を、次の起動の read の段の最初に読む、2 GiB）を、`bench-debian.sh` に ext4 の冷えた読み（`echo 3 > /proc/sys/vm/drop_caches` の後）を足す。Debian の 95% の基準をここで決める。
2. 読み: (a) VM object の miss の run を 64 KiB から 256 KiB〜1 MiB に（`KERN_IO_BATCH_MAX` を使う所を分ける、scratch を経ずに page 列で読む口を検討）、(b) file の先読みの窓を 1〜2 MiB にし、demand の I/O と重ねられるように（`readahead_demand_begin` の扱い）、worker を disk ごとに 1 本の制限から外す。raw の先読み（p004）と同じく、line の確保の cache_lock の往復を減らす（`recycle_line_locked`・`copy_cached_lines` を使う）。
3. 書き: (a) flusher と `buf_sync` が隣り合う dirty の line を page 列の run（256 KiB まで）にまとめて書く、(b) UFS の割当を 64 KiB より大きく（cylinder group・inode の更新を run ごとに 1 回）、(c) `BUF_DIRTY_LIMIT` を cache の大きさに合わせる。
4. 試験: host（合流の計画の純粋な関数）、QEMU（boot・nvme-shapes・grow-reboot・UFS の cksum と fsync の後の reboot）、5330（`nvme-bench-5330.sh … read write`）。

## 判断が要る点

- 2(a) は VM と UFS の共通の口（`KERN_IO_BATCH_MAX` の意味）を変える。範囲が WS299 の「UFS の cluster」に入るか、VM の WS を別に立てるかを Q1 に聞く。
- HAL の API は変えない見込み。

## 再開点（2026-10-28、P1）

- p004 の後に P1 の context が少なくなったので、p005 はこの調べと案で止めた。次: 1（測る）→ 案の design-reviewer → 実装。
