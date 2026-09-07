# I/O・cache の共通実装契約

日付: 2026-09-07。Parent: [WS025](ws.md)。詳細な採否・理由は [承認済み方針](../buffer-plan-codex-2.md) を参照。

本書は各 Phase が共有する実装上の境界である。各 P-book の成功条件はこの契約を弱めない。

## 1. 資源と状態の所有者

| 資源 | 所有・寿命 | 初期実装 |
| --- | --- | --- |
| I/O scratch | pool が保有し syscall/exec が借りる。全 exit path で返却 | 64 KiB と 4 KiB の固定要素。borrow は lease 内で非待機 |
| 通常 file 内容 | 実層 inode に対応する VM file object。mapping/op/cache reference を分離 | write-through read cache。その後同じ所有者を dirty 化 |
| FS metadata | FS の bounded な作業像・公開済み像。block cache の単なる dirty bit で順序を代用しない | 同期 operation batch、次に統合 UFS の journal |
| BIO | submitter が拒否を処理し、受理後は completion owner が一度だけ完了 | 同期 adapter、次に bounded worker |
| claim / generation | submission から最終 completion/取消しまで保持 | loop 再入・別 worker へ明示的 context で伝播 |
| DMA | mapping とその backing の内容を retirement まで保持 | bounded bounce/reservation。SG は後段 |
| dirty credit | 内容を dirty にする前に予約、clean/discard が正当化されたとき返す | lease より前の待機。内部 I/O は予約を継承 |

read/write/mmap と loop backing が同じ内容を独立に書戻さない。移行中に file cache と block cache が複製を持つ場合は、共通の内容世代と無効化規則、容量上限を適用する。claimed backing file に第二の file-cache writer を作らない。

## 2. API を導入する順序

各機能は adapter → 実効 capability の限定公開 → 正常・失敗 fixture → 既定化の順に入れる。

- allocation `commit` が no-op でよいのは保留した更新がない adapter の段階だけ。`abort` は結果を返す。
- delayed 要求を through で実行する場合は実効 policy を through と報告する。
- async は worker queue 等によって submit が device 待ちをしなくなってから公開する。queue full は別の明示的結果にする。
- SG の互換路は segment ごとに正しい LBA/byte 範囲へ分割する。非連続 LBA を一 command と偽らない。
- 未実装 flag、queue depth、runtime 設定は成功扱いで無視しない。設定 OFF/resize は drain 成功後にのみ確定する。

既存 API を残す段階でも private adapter の重複を無期限に残さない。各 P-book の終了時に唯一の production owner と次に削除する互換路を記録する。

## 3. 粒度とメモリ予算

初期 `KERN_IO_BATCH_MAX=64 KiB` を syscall/UFS/loop/buf/storage で揃える。device max、hole、物理 run、partial block、error 境界で短くできる。vector の複数要素を跨ぐ合成は p006 の条件に含めず、PIPE_BUF、stream、short result、EFAULT、positional offset を維持する。

pool 希望要素数は `max(16, 2 × online CPU)`、大小合計の実確保量は `min(RAM/64, 4 MiB)` 以下。4 KiB reserve はその最大 1/4 を候補とする。header/page rounding を含め、低 RAM では減数する。両方が枯渇した場合だけ既存 stack fallback を残す。fallback のたびの heap allocation や、lease 内での pool 待機は入れない。

USB 64 KiB bulk 2 本なら payload は core 128 KiB + HCD 128 KiB/device。8 KiB reclaim reserve、request、alignment、隔離資源は別に計上する。normal path が reclaim-safe reserve を奪わない。非対応 HCD は 8 KiB の実効上限を公開する。

p016 で `hal_pmem_get_total_size()` の新しい管理 RAM 合計を予算の基準にし、page/buf/metadata/pool/DMA の内訳と可回収量を分ける。仮想予約量を resident bytes に数えず、DMA 隔離や pinned/dirty page を即時回収可能量に数えない。

## 4. lock と I/O の順序

初期 cache run は管理領域を multi-line busy 前に準備し、line は try-acquire で揃える。失敗時は全 line を離して短縮/retry する。line を保持したまま次の allocation/reclaim が同じ run を待つ構造を禁止する。

dirty credit の待機は file/content lease 前。cache/VM global lock の下で下位 FS/USB I/O を呼ばない。worker に移しても backing claim、媒体世代、内容世代、I/O 目的を失わない。lock 順位だけでなく loop を含む待機関係を failure fixture で検証する。

async の callback が submit return 前に発火する場合を許容し、参照の引渡し規則を一意にする。waiter が BIO を解放できる状態を公開した後に BIO を読み直さない。未 retirement の DMA は request/page/map を保持し、再 dirty の世代管理とは別に DMA 中の内容を固定する。

## 5. 永続化と失敗

write は「受理」「転送完了」「永続化」を区別する。fsync はその開始時に固定した内容/metadata 世代と既存依存を下位まで drain し、必要な device flush/FUA を成功させる。後続の write を既存 flush の成功範囲へ含めない。

leaf の accepted/completed/stable frontier だけで上位 dirty の有無を推測しない。FS/mount/overlay も論理 dirty 世代を持つ。複数 flush を合流できるのは同じ順序境界だけ。journal の異なる commit、temp fsync → rename → directory fsync を一つの barrier に潰さない。

allocation は「予約 → 全 bytes 初期化 → allocation/data の必要な永続化 → pointer/size 公開」。完全 block の user data による初期化は zero を代替できる。pointer が公開された可能性のある block を free に戻す場合は、pointer clear の永続化を先に証明する。結果不確定なら保留・診断・必要な書込み停止を行う。

writeback error の内容/errno/世代を保持し、観測 cursor は open file description ごとに持つ。独立 open 同士でエラーを消費し合わず、dup は同じ cursor を共有する。未解消の dirty 書戻し失敗は、過去エラーを報告済みでも fsync 成功にしない。

## 6. 有効化の境界

最初に遅延するのは既存割当済み・既存 size 内の通常 file data。新規 allocation、pointer、directory、FAT mirror、journal は同期順序を維持する。下位 FAT の「file data」が上位 UFS metadata である場合を含め、意味を示す内部 I/O context で下位の再遅延を止める。

p021 は WS024 の単一 UFS とその journal を拡張する。基本方式は ordered data + metadata journal。既存 journal が十分とは仮定せず、commit/replay/checksum/flush と snapshot/quota/overlay journal の相互作用を検証する。FAT は p013 の同期 batch までを必須成果とし、操作をまたぐ metadata/mirror 遅延は別に recovery 根拠が得られた場合だけ有効化する。

read cache は最後の close 後も予算内で保持する。exec の初期共有は実行中に内容が固定される inode/backing に限定し、実行 pin と MAP_PRIVATE/COW を使う。read-only mount の可変化も pin の寿命で制御する。通常の writable file は既存 copy snapshot を維持し、一律 ETXTBSY を追加しない。変更可能 file の共有へ拡張する場合は内容世代別 snapshot と writer COW/reclaim を別に設計する。unlink/rename は inode lifetime を保ったまま扱う。

## 7. 検証と記録

各 Phase は source revision/hash、build config、fixture command、counter snapshot、異常終了地点、実効 policy を results に記録する。結果が出る前に results の PASS 欄を作らない。

時間は warm-up 後の分布を測り、guest の単発 10 ms 値で改善を断定しない。data command と metadata/flush/retry を分ける。成功した fsync の対象保持は、volatile device cache/reorder/torn write の fault model で検証する。QEMU kill は補助であり、その数だけを永続化の証明としない。

q086 FS 50、q087 syscall、VM content lease、USB retirement、Wi-Fi30 の既存証拠と fixture を必要範囲で使う。[受け入れ行列](acceptance.md) の未実行セルは明示的に残す。既定化に必要な実機セルが未確認なら p026 を全面完了にはしない。

2026-09-07 更新: ユーザー指示により本件 p005/p026 の実機 gate は user-accepted とする。agent による実機 runtime は未実施と区別し、上記の実機待ちによる停止条件を本件では適用しない。

## Implemented p019 asynchronous boundary

`bio_async_enable` provisions one leaf endpoint (up to four) and four private
64 KiB slots plus page-rounded control records. `bio_async_prepare` retains the
request's disk/cache token, originating inode and context/authorization claims,
and snapshots write bytes. A distinct authorization claim is checked through
the existing mutation registry; context provenance never authorizes a write.
Preparing or enabling may fail; no unbounded fallback is used. Submission
accepts at most two waiting requests in addition to an active worker request;
a full queue returns EAGAIN without callback and preserves the prepared handle.

Admission registers the BIO and write frontier before return. FIFO workers call
existing synchronous drivers independently per leaf. Loop workers use ordinary
synchronous lower calls, requiring no second queue slot. Accepted dispatch
failure, queued cancellation and normal completion converge exactly once.
Running cancellation returns EBUSY, without releasing request or DMA ownership.
A callback may run before submit returns and may drop the caller reference;
worker ownership persists through callback return. Callbacks must not block.
Final handle cleanup runs in process context and releases exclusion and owners.
Read bytes are exposed only after successful full completion. Short reads/writes
report EIO; media/reset epoch mismatch reports stale failure instead of publishing
bytes as a result for a later device lifetime.

The media/reset epoch is independent of the persistence proof epoch. Ordinary
write error or queued cancellation can invalidate durability proof without
changing device identity. Explicit reset/replacement/detach invalidates both.
At media-epoch saturation new asynchronous preparation refuses safely.
Endpoint disable requires all handles retired and released. It frees charged
payload/control storage and device pins; the bounded thread stays idle for reuse.
A failed physical free keeps the endpoint and charge available for a later retry.
