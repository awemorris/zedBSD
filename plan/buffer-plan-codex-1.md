# buffer-plan.md への対応方針

日付: 2026-09-07  
照合対象: `/home/awe/zedBSD` HEAD `dd4f31c` (`Fix syscall write`)  
状態: レビュー用設計案。Phase/Queue 未選択、実装未開始  
対象: syscall bounce buffer、buffer cache、UFS、loop、USB mass storage、xHCI、媒体状態、物理メモリアロケータ  
原案: [buffer-plan.md](buffer-plan.md)

## 1. 結論

原案の主張である次の三点を採用する。

1. syscall ごとの大きな物理連続領域の確保をやめる。
2. write-through の契約を維持したまま、buffer cache が連続ラインを一つの要求として発行する。
3. 通常のストレージ I/O 上限を各層で揃え、結合した要求を USB HCD まで分解せず渡す。

ただし、実装は syscall、`buf_write()`、USB 上限の三箇所だけでは完結しない。UFS は現在
8 KiB の filesystem block ごとに `disk_write()` を呼ぶため、buffer cache だけを変更しても
通常ファイルの 64 KiB 要求は八つに分かれる。また、USB core の同期用 staging buffer と
xHCI の DMA bounce buffer は別物であり、双方の寿命を固定しなければ HCD 内の動的確保が残る。

この計画では、通常 I/O の共通上限を `KERN_IO_BATCH_MAX = 64 KiB` とする。これは一回で必ず
64 KiB を物理媒体へ転送するという意味ではない。ファイルの物理的不連続、device の
`d_max_transfer_blocks`、障害境界では短い run に分かれる。64 KiB は syscall、UFS、loop、
buffer cache、USB storage が保持できる最大の連続要求を揃えるための上限である。

q087 の 256 KiB 動的確保は、この設計の syscall buffer phase で置き換える。それまでは
既存動作として保持し、レビュー中に巻き戻さない。

## 2. 変更後も守る契約

以下を性能改善より優先する。

- `buf_write()` は返る前に対象データの同期 write を完了する。write-back、syncer、遅延書込みは導入しない。
- 書込みエラー時、完了を証明できない cache line は dirty のまま残し、後の `buf_sync()` で再送できる。
- 部分 block の前後データ、UFS の allocation zeroing、pointer publication、inode size、short I/O の返値を変えない。
- backing claim、file/VM content transaction、MAP_SHARED coherence の内側で処理する。
- timeout 後も HCD が所有する URB、core staging、DMA bounce を解放または再利用しない。
- swap の reclaim-safe 8 KiB reserve を通常 I/O の 64 KiB 化と混同しない。
- 媒体変更の疑いがある disk generation に、古い mount、cache、partition、claim を接続したまま復帰しない。
- xHCI IMOD の既定値は実機測定が終わるまで変更しない。

## 3. 実装前に追加する観測点

処理時間だけでは、どの層で要求が分割されたか判定できない。最初に次の累積 counter と、
テスト用 snapshot/reset API を追加する。公開 sysctl にする値と test-only checkpoint は実装時に
分けるが、意味は同じにする。

| 層 | 観測値 |
| --- | --- |
| syscall I/O pool | borrow 成功、fallback、同時使用最大数、pool 構築時の物理確保回数 |
| UFS data path | data run 数・bytes、partial block 数、hole 数。metadata I/O は別 counter |
| buffer cache | read/write run 数・bytes・最大 run、単一 line fallback、失敗 run |
| disk | driver へ実際に submit した BIO 数・bytes。buffer cache の run 数と混同しない |
| USB storage | READ(10)/WRITE(10) command 数・data bytes、reset/retry 数 |
| xHCI | request/DMA の動的確保、URB reservation 利用、IRQ 数、completion latency |
| physical allocator | allocation 回数、調べた bitmap page/word 数、失敗、IRQ-off 最大・累計時間 |

基準測定は、既存ファイルの aligned 64 KiB read、aligned 64 KiB overwrite、先頭・末尾 partial、
連続配置と断片配置で採る。`4 × 64 KiB overwrite + final fsync` は継続するが、data command と
metadata/flush command を分けて数える。時間は一回値ではなく、warm-up 後に十分長い反復を行い
median/p95 と分布を保存する。QEMU の 10 ms 刻みの一回値だけで既定値を決めない。

## 4. 対策 A: syscall bounce buffer の寿命

### 4.1 採用設計

原案の目的には賛成するが、64 KiB を各 thread が終了まで保持する方式は採用しない。
thread 数に比例して常駐領域が増え、I/O 待ちの thread が多数あるだけで物理連続領域を保持するためである。

代わりに、kernel 内に **64 KiB 固定要素の bounded shared I/O pool** を置く。

- pool は起動時に構築する。要素数は `min(online CPU 数, 64, floor(physical RAM / 64 / 64 KiB))`
  （最低候補一要素）とし、総量を RAM の 1/64 かつ 4 MiB 以下にする。確保できた要素だけ公開し、
  ゼロ個でも起動可能とする。この既定式は計測対象とし、将来は config で上書き可能にする。
- 各要素は一度だけ `kern_malloc(64 KiB)` し、その後の syscall 間で再利用する。
- syscall は `file_io_begin()` 後に一要素を try-borrow する。borrow は allocation も sleep も行わない。
- pool が空、初期確保失敗、early boot、再入時は既存の 512 B stack buffer を使う。pool 待ちはしない。
- `file_io_end()` まで buffer を保持し、その直後に全 exit path で返却する。所有 thread と貸出世代を
  debug/test build で記録し、二重返却と別 thread からの返却を検出する。
- 64 KiB を超える syscall は同じ要素を繰り返し使う。readv/writev は当面 iovec 境界を保ち、
  一つの iovec が 64 KiB 以内なら一回の `file_io_transfer()` とする。
- PIPE_BUF writev、stream/device の一回成功後終了、partial result、EFAULT、RLIMIT_FSIZE/SIGXFSZ の
  現行規則を変えない。

pool を `file_io_begin()` 後に借りるのは、inode/file lease を待つ thread が先に全要素を保持するのを
防ぐためである。pool borrow 自体は非待機なので、lock 内 allocation という元の問題は再発しない。
per-CPU 固定 buffer は、I/O 中の sleep と CPU migration のため採用しない。

### 4.2 物理連続性と容量

現在は kernel virtual address の連続領域を非連続物理 page から組み立てる一般機構がないため、pool の
初期構築時には 64 KiB の物理連続確保が残る。hot path から除外して回数を online CPU 数以下に固定する。
固定 heap の threshold を上げる案は、512 KiB heap の枯渇と断片化を招くため採用しない。

将来 kernel vmap または scatter/gather uaccess が利用可能になれば pool の backing を page vector に
変更できる。これは本計画の完了条件にはしない。

### 4.3 受け入れ条件

- pool warm-up 後、pool 利用時の 64 KiB read/write/pread/pwrite と単一 iovec readv/writev は、
  syscall bounce 用 `hal_pmem_alloc` を呼ばず、`file_io_transfer()` を各一回呼ぶ。
- pool 全要素を並行占有した試験で、追加 syscall は待たずに 512 B fallback で完了する。
- begin 失敗、最初／途中の user copy 失敗、backend short/error/EOF、signal/stop redispatch の全経路で
  pool 要素、pin、file/VM lease が残らない。
- q087 の境界、partial I/O、positional offset、vector、PIPE_BUF fixture を、64 KiB 上限の期待値に
  更新して ASan/UBSan で通す。

## 5. 対策 B: buffer cache の連続 run

### 5.1 追加 staging buffer は使わない

書込み時の `buf_write()` の入力、読込み時の `buf_read()` の出力は、既に呼出し中ずっと有効な連続領域である。
中央の完全 line run ではこの領域を直接 `disk_write_direct()` / `disk_read_direct()` に渡す。
cache line が散在していても、新たな連続 staging buffer は不要である。

これにより、loop disk の `buf_write()` が下位 FAT/物理 disk の `buf_write()` を再入しても、共有 staging
buffer を二重に借りて自己待ちする問題がない。scatter/gather BIO は初段では導入しない。

### 5.2 write run

一回の `buf_write()` を次の順で処理する。

1. 先頭 partial line があれば、現在の read/modify/write を一 line で実行する。
2. 中央の完全 line 群を、64 KiB、disk 終端、`d_max_transfer_blocks` を境界に run 化する。device 上限が
   line block 数の整数倍でなければ完全 line に切り下げ、上限が一 line より小さい場合は一 line を
   `disk_write_direct()` 内で再分割させる。
3. run の全 line を block 昇順に `acquire_line(..., read_data = 0)` で busy にする。最大保持数は
   `64 KiB / PAGE_SIZE`（現在 16）。全 line を取得するまでは cache data/dirty generation を変更しない。
4. 途中取得が失敗したら、取得済み line を逆順に解放し、先頭 line だけの既存経路へ縮退する。
   複数 run を同時に保持せず、cache lock または line lock を保持したまま allocation/I/O しない。
5. 全取得後、入力を各 `b_data` に copy し、dirty generation を進め、全 line に WRITING/inflight を公開する。
6. run 全体を、元の連続入力 pointer から一回の `disk_write_direct()` へ渡す。
7. 成功時は snapshot generation と一致する line の dirty/error を落とす。失敗または short BIO 時は
   run 全体を VALID|DIRTY|ERROR のまま残す。下位で前半だけ完了していても範囲別完了を推測せず、
   run 全体を再送対象とする。
8. 全 line の I/O state を戻して waiter を起こし、解放する。末尾 partial line を一 line で処理する。

busy を全 line に保持するため通常の cache writer はその generation を変更できないが、既存の generation
比較は残す。backing mutation guard は、外側の caller range と内側の direct run の現行 admission を
維持し、guard を省略する最適化はこの段階に含めない。

### 5.3 read run

- cache hit は従来どおり一 line ずつ caller buffer へ copy する。
- request 内で完全 line かつ連続して invalid/missing の範囲だけを最大 64 KiB の run にする。
- line を昇順に busy 取得し、READING/inflight を公開して、caller の連続出力領域へ一回で読む。
- 成功後だけ出力から各 cache line へ copy して VALID を公開する。失敗時は全 line を invalid+ERROR とし、
  caller の部分的に変更された領域を成功データとして返さない。
- 先頭・末尾 partial miss、run 中の valid/dirty line、取得縮退では既存の単一 `read_buffer()` を使う。
- 要求範囲外の line は読まない。先読みは導入しない。

### 5.4 counter と受け入れ条件

既存の `bufcache.write_bios/read_bios` は「cache が発行した run」と定義し直す場合でも、driver が受け取る
実 BIO 数とは別 counter にする。`disk_transfer_direct()` が `d_max_transfer_blocks` で再分割し得るためである。

- memory disk 上の aligned 64 KiB cold read/write は、cache run 一回、driver BIO 一回になる。
- hot read は driver BIO ゼロになる。
- partial、cache hit/miss 混在、disk 終端、4 KiB より大きい logical block を正しく処理する。
- 失敗 run の全 line が dirty+ERROR で残り、`buf_sync()` が再送して clean にできる。
- 成功時は対象 write generation が clean になる。`cache_dirty_bytes` が「呼出し前と同じ」という条件は
  採用しない。呼出し前に古い dirty line があれば、成功によって減ることが正しいためである。
- overlapping run の二 thread 試験、cache reclaim 注入、loop→FAT→物理 cache の再入試験で deadlock がない。

## 6. 対策 C: UFS の data run

buffer cache の run 化だけでは UFS の 8 KiB 呼出し境界を越えられない。`pread_inode()` と
`pwrite_inode()` に、logical block から physical fragment への連続 run builder を追加する。

### 6.1 読込み

- 先頭／末尾 partial block は従来の scratch と一 block read を使う。scratch は必要時に初めて確保する。
- aligned full block は `bmap()` で mapping し、次の fragment が `previous + super.frag` の間だけ結合する。
- hole は caller buffer を zero にし、I/O run には混ぜない。連続 hole はまとめて zero 化する。
- 物理的に連続する既存 block を最大 64 KiB まで一回の `disk_read()` に渡す。
- mapping error または data error の前までを POSIX partial result として返す。

### 6.2 書込み

- partial block は従来どおり read/zero、modify、write を行う。
- full block は `bmap_ensure()` を block ごとに行い、既存の「zero 初期化後に pointer を永続化する」順序を
  変えない。連続して確保できた／既存の physical fragment だけを最大 64 KiB の data run にする。
- run 構築途中の allocation/mapping error では、構築済み prefix を先に書いてその bytes を完了扱いにし、
  未書込み block を完了扱いしない。既に公開された allocation を黙って free しない。
- data run が失敗した場合、その run より前だけを完了 bytes とする。inode size は完了 bytes まで進める。
  cache に残る失敗 run の dirty data は再試行可能だが、syscall 成功の根拠にはしない。
- `persist_inode()`、allocation bitmap、indirect pointer の metadata I/O は data run counter と分離する。
  metadata batching は別課題である。

### 6.3 受け入れ条件

- preallocated、物理連続、aligned 64 KiB の overwrite/read は UFS data call 一回になる。
- 新規 64 KiB 書込みでは zero/publication 順序と inode size を fault injection で確認する。metadata command
  を含めた総 USB command 一回とは主張しない。
- 物理断片化した UFS file は連続 fragment ごとに分かれ、内容と partial result が一致する。
- direct/indirect 境界、hole、partial head/tail、ENOSPC、metadata/data short write、rollback failure 後 readonly
  を既存 UFS fixture で確認する。

## 7. 対策 D: loop と FAT の境界

loop の上限 64 KiB と q086 の retained logical extent map は維持する。UFS と buffer cache から届いた
64 KiB BIO を `file_pwrite_internal()` に一回で渡す。FAT backing file が断片化している場合、
`fat_loop_transfer()` は extent 境界で分割する。この分割は必要であり、結合数の失敗とは扱わない。

受け入れでは二種類を区別する。

- 連続 FAT backing: loop BIO 一回、FAT extent 一つ、物理 cache run 一回。
- 断片 FAT backing: loop BIO は一回のまま、物理 run/USB command は実 extent 数と device 上限に従う。

既存の backing claim、file/VM transaction、FAT slot flush/invalidation、neighbor sector の read/modify/write
を全て維持する。

## 8. 対策 E: USB core と HCD の二段 reservation

### 8.1 二種類の buffer を明示する

USB storage の data path には少なくとも次の二つがある。

1. USB core の同期 staging buffer: caller timeout 後も caller memory に触れないため URB が所有する。
2. HCD の DMA bounce/request: xHCI が hardware に渡し、completion/cancel の所有権が解けるまで保持する。

`drv_usb_urb_reserve_sync()` は 1 の契約である。2 の予約を同じ成功値に暗黙に含めず、HCD に対して
**idle URB 用 request/DMA reservation API** を追加する。xHCI は URB ごとに request metadata と coherent
DMA buffer を予約し、enqueue から completion までその object を busy にする。

- reservation の置換・解放は URB が idle で HCD ownership がゼロの時だけ許す。
- final URB reference の解放時に、HCD callback で reservation を解放する。timeout/cancel failure 中は解放しない。
- active request pointer と reserved object pointer を別 field にし、現在の `hcd_private[0]` を多目的利用しない。
- xHCI ring は 64 KiB境界を跨ぐ DMA address を複数 Normal TRB に分ける現行処理を使う。ring capacity と
  short/residual completion を 64 KiB で再検証する。
- EHCI/UHCI/未対応 HCD は reservation unsupported を返せる。USB storage attach 自体は失敗させず、
  従来の 8 KiB 上限へ戻す。

### 8.2 USB storage の上限決定

- bulk-in/out URB の core staging と HCD reservation が双方 64 KiB を満たした場合だけ、通常 data 上限を
  64 KiB として `d_max_transfer_blocks = 64 KiB / logical_block_size` にする。
- logical block size は 512 B 以上 64 KiB 以下の power-of-two をこの実装の対応範囲とする。範囲外は attach 時に
  明示的な unsupported error とし、device の申告値による無制限確保をしない。
- 64 KiB reservation の一方でも失敗し、logical block が 8 KiB 以下なら、既存の 8 KiB core/HCD reclaim
  reserve が成立する範囲で `d_max_transfer_blocks` を 8 KiB 相当にして動作を継続する。logical block が
  8 KiB を超える場合は一 block 分の core staging と HCD 通常動的経路が成立すれば一 block 転送として公開し、
  それも成立しなければ attach を unsupported とする。この経路は reclaim-safe とは表示しない。
- control URB は 64 KiB 化せず、既存の小さい command/sense/control 要求に必要な reserve だけを持つ。
- `DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE = 8 KiB` と controller 共通 emergency reserve は維持する。現在の swap I/O
  は一 page（4 KiB）なので、この境界内にある。通常 64 KiB と reclaim-safe の意味を一つの定数にしない。
- xHCI は URB 固有 reservation を controller 共通 reserve より先に使い、通常 storage I/O が他 endpoint の
  emergency reserve を占有しない。

### 8.3 受け入れ条件

- reservation 成功後の 64 KiB READ/WRITE は、USB core と xHCI の request/DMA 動的確保が各 command でゼロ、
  READ(10)/WRITE(10) data command が各一回になる。
- BOT の CBW/data/CSW、CSW STALL 再読、reset/reissue、timeout/cancel/late completion で、二段 buffer の
  owner と解放順を検証する。
- HCD reservation の attach 時失敗は 8 KiB に縮退し、I/O、reclaim-safe swap、detach が動く。
- 64 KiB boundary crossing、short transfer、endpoint recovery、concurrent HID/WLAN endpoint を実 xHCI code の
  fixture と QEMU USB-root で確認する。
- 連続 backing の preallocated 64 KiB overwrite では data WRITE(10) 一回を確認する。fsync の
  SYNCHRONIZE CACHE と UFS/FAT metadata command は別に数える。

## 9. 対策 F: physical allocator

syscall pool によって allocation scan は hot path から外れるが、xHCI、cache、VM などの利用者には残る。
allocator 修正は独立した Phase とし、buffer batching の成立条件にはしない。

第一段階では amd64/i386 の bitmap scan に rotor と word 単位の探索を導入し、毎回 page 1 から調べる動作を
なくす。単純に lock なしで bitmap を読む変更は行わない。現在の plain bitmap access を lock 外で読むと、
別 CPU の更新との整合性を説明できないためである。

IRQ-off 区間をさらに短くする場合は、bitmap word を atomic load/update 可能な表現へ変更し、次の二段階にする。

1. lock 外で atomic word snapshot から候補 run を探す。
2. `pmem_lock` 内で候補全体を再検証し、一括 claim する。競合時は rotor を進めて再試行する。

失敗時には一度だけ全範囲を覆い、同じ位置を無限に再試行しない。free、usable-range publication、fixed claim と
同じ bitmap generation/atomic 規則を共有する。測定で word scan + rotor の IRQ-off 最大値が十分小さければ、
二段階化は実装せず結果を記録する。

受け入れは full/fragmented bitmap、alignment、wrap、concurrent alloc/free、exhaustion、同一 page 二重割当なし、
accounting 一致とする。走査 page/word 数と IRQ-off 時間の分布を起動直後・cache full・user memory pressure で
比較する。「p95 が完全に同じ」は要求せず、退行の有無と残る最大値を数値で判断する。

## 10. 対策 G: `media_error` を状態機械へ置換する

現在の永続 `int media_error` を単純に解除する方法は採用しない。INQUIRY、capacity、任意 LBA の一致だけでは、
古い mounted filesystem/cache/claim を別媒体へ安全に接続できた証明にならないためである。

storage object に次の状態と、last sense、media generation を持たせる。

| 状態 | 意味と許可する操作 |
| --- | --- |
| ONLINE | 通常 BIO を許可 |
| RECONFIGURE | 同一媒体で変更可能な mode/cache/write-protect を再取得中。新規 BIO は保留または有限 EAGAIN |
| REVALIDATE | 媒体同一性に疑いがある。古い generation の BIO を拒否 |
| ABSENT | medium not present。古い generation の BIO を拒否 |
| FAILED | transport/revalidation 自体が有限回失敗。診断を保持し、明示的再試行または replug を待つ |

sense の扱いを次のように固定する。

- 自分が成功させた BOT reset 直後の `06/29/00` は、現在どおり一回だけ command retry を許す。
- `06/2A` の既知 parameter change は RECONFIGURE とし、MODE SENSE/read-only/flush policy を再取得する。
  block size/count が変われば REVALIDATE へ昇格する。
- `06/28` medium may have changed と `02/3A` medium absent は、それぞれ REVALIDATE/ABSENT とし、古い
  disk generation を ONLINE に戻さない。
- その他の `06/xx` は即座に永久 EIO を設定せず、current command を失敗させて bounded な分類・再取得を行う。
  未分類の UA は安全側の REVALIDATE とする。

revalidation は失敗 BIO の stack 内で mount/cache lock を持ったまま実行しない。storage worker/control path が
新規 submission を閉じ、inflight を drain して TUR、INQUIRY、READ CAPACITY、MODE SENSE、disk identity を取得する。

- 変更が mode parameter だけで geometry/identity が不変なら同じ generation を ONLINE に戻せる。
- 媒体 absent/change は、disk が完全に idle になった場合だけ旧 physical disk を offline にし、cache と
  partition children を破棄して旧 disk object を destroy する。その後に SCSI probe と identity 読取りを行い、
  **新しい physical disk object/generation** を登録して partition scan をやり直す。既存の disk reload admission
  は旧 object の partition 整理に利用できるが、同じ physical disk object の identity bit を書き換えるだけで
  媒体交換を表現しない。
- mounted root や open/claimed disk は generation replacement を行わない。古い mount を付けたまま latch bit だけ
  clear する経路は設けない。この場合は診断可能な REVALIDATE/ABSENT のまま replug/reboot を要求する。

これにより、一時的 parameter change は永久停止にせず、実媒体変更は古い namespace へ復帰させない。
受け入れには全 sense 分類、reconfigure 成功/失敗、capacity 変更、idle generation replacement、mounted/claimed
replacement 拒否、replug による新 object、並行 BIO drain を含める。

## 11. 対策 H: xHCI IMOD

IMOD は batching と reservation の実装後に測る。QEMU の 0/4000 が同じだった結果は既定値変更の根拠にしない。
実機で 0、160、4000 を最低限比較し、USB 2/USB 3 storage の read/write/fsync、HID/WLAN 並行負荷について、
IRQ/s、completion p50/p95/p99、throughput、CPU time、timeout/recovery 数を保存する。

correctness failure、completion loss、timeout 増加がある値は除外する。残りから latency と IRQ/CPU cost を比較して
既定値を決める。測定前に 160 または 0 を既定値と決め打ちしない。device/controller 固有差が大きければ、単一の
global default ではなく controller quirk/configuration として別設計に戻す。

## 12. 実施順と停止点

この文書のレビュー後、MWP-Q の Phase/Queue は次の依存順で分ける。一つの巨大 Queue にはしない。

| 段階 | 内容 | 完了時の判断 |
| --- | --- | --- |
| 0 | 観測 counter と現行 baseline | 各層の実分割数を確定。予測と違えば後段設計を更新して停止 |
| 1 | 64 KiB syscall shared pool | q087 functional gate、allocation/fallback/並行試験、三構成 build |
| 2 | buffer cache full-line run | memory disk、failure generation、reclaim、loop 再入試験 |
| 3 | UFS contiguous data run | existing/new/fragmented/indirect/fault test、q086 FS 回帰 |
| 4 | USB core + xHCI URB reservation、通常 64 KiB | BOT ownership/recovery、8 KiB fallback、QEMU USB-root、Wi-Fi30 回帰 |
| 5 | allocator rotor/word scan | allocator concurrency/accounting/IRQ-off 測定。二段階 scan の要否を判断 |
| 6 | media state machine | sense/revalidation/generation tests。性能変更から独立してレビュー |
| 7 | 実機 IMOD 測定 | 既定値を維持または変更する根拠を記録 |

段階 1、段階 2–3、段階 4 は別 Queue 候補とする。段階 5–7 はそれぞれ独立 Queue とする。
各段階で該当する q086 50-story、q087 syscall test、FAT/UFS/storage foundation、USB ownership/recovery、
Wi-Fi30、amd64/pcat/pc98 build を必要範囲で再実行する。USB-root native test は disposable image を使い、
元 image の hash 不変を確認する。aggregate `make check` と commit は行わない。

## 13. この計画で意図的に扱わないもの

- write-back、dirty watermark、syncer
- 汎用 file page cache、先読み、user page からの直接 I/O
- scatter/gather BIO、UAS、BOT command 並列化
- FAT/UFS metadata 全般の batching、allocation bitmap/indirect block cache
- 媒体変更後の mounted filesystem をそのまま継続する機能
- 測定前の IMOD 既定値変更

本計画の完了像は、64 KiB の連続した既存ファイル data I/O が、syscall、UFS、loop、buffer cache、
USB storage、xHCI の各層でその形を保ち、物理的不連続または明示した device/error 境界でだけ分割されることである。
その過程で新しい動的物理確保を hot path に持ち込まず、write-through、媒体世代、timeout ownership の契約を維持する。
