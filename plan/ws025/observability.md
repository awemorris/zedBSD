# WS025 p001 観測と現行契約

この資料は q088 の実装に対応する。write-through、syscall の 256 KiB 上限、
USB の実効転送上限、既存の lock/claim 方針は変更しない。

## 読取り API と単位

- `vfs.io.stats`: [公開 layout](../../include/uapi/zedbsd/io-stats.h) の
  version/count と event ごとの `calls`、`bytes`。起動以来の uint64_t 累積値。
  リセットはできない。各 field は HAL atomic、全体を一つの transaction として
  凍結するものではない。停止中の before/after 差分は正確、動作中は field 間にずれがある。
- `hw.memory.stats`: version 1。BSP が列挙できる range 数、usable 合計、
  全種別の最大 end、usable の最大 end、直接マップの address span、
  初期 allocator 公開 bytes、現在の物理 span/reserved/allocated/free を分ける。
  amd64 以外の boot-range 拡張は validity=0。0 を「RAM がない」と解釈しない。
- `vfs.bufcache.stats` と既存の max/current/dirty bytes は従来の API を維持する。
  source/config と合わせて測定条件を特定する。
- `sysctl vfs.io.stats` / `sysctl hw.memory.stats` からも参照できる。
  event 番号の定義は上記公開 header。bytes は層をまたいで合計しない。

全カウンタは machine-wide。別 thread の I/O、overlay の委譲、loop の backing
I/O を含む。要求 bytes は実転送 bytes や永続化 bytes ではない。

| Event | 計数点と意味 |
| --- | --- |
| SYSCALL_READ/WRITE | 六 syscall 内の file_io_transfer 呼出し直前。user copy で失敗して呼ばなかったものを除く。regular file 以外の同経路も含む。 |
| FILE_READ/WRITE | file_io_transfer の基本引数検証後。overlay 再入、loop backing もそれぞれ計数。以降の拒否・EOF・short を含む要求量。 |
| UFS1/2_READ/WRITE | 各 UFS VFS から disk_read/write を呼ぶ直前。super/CG/inode/indirect、zero、directory、journal/snapshot callback を含む。 |
| UFS1/2_CONTENT_READ/WRITE | pread/pwrite および truncate tail が有効な content block に発行する block 要求。hole のメモリ上ゼロ生成は除く。regular file に限定せず、この経路を通る content を数える。 |
| BUF_READ/WRITE | range 解決後の cache request。hit/miss は既存 bufcache stats、実分割は DRIVER を参照。 |
| DRIVER_READ/WRITE/FLUSH | partition を解決した leaf の submit callback を呼ぶ直前。loop も leaf driver なので含む。callback の即時拒否も「呼出し」に含む。 |
| COMPLETE_READ/WRITE/FLUSH | SUBMITTED BIO の最初の bio_complete。bytes は driver の transferred。error 完了を含み、ERROR はその部分集合。 |
| LOOP_READ/WRITE/FLUSH | 有効な loop 要求が backing file 操作へ進む時点。flush は bytes=0。write の read-only 拒否も request に含む。 |
| USB_READ10/WRITE10/SYNC_CACHE/OTHER | BOT CBW を送る直前。再試行、CBW 失敗も一回ずつ。bytes は CDB のデータフェーズ要求量。 |
| DMA_REQUEST | coherent allocation の基本引数検証後。requested bytes。拒否・確保失敗を含む。 |
| DMA_ALLOC/FREE | 呼出し元へ公開する coherent allocation / HAL が成功を返した解放。bytes は丸め済み HAL descriptor size。途中で確保後に取り消したものは公開に含めない。 |
| XHCI_QUARANTINE | controller が初めて quarantine に入った回数。bytes=0、保持容量を推測しない。 |
| USB_BUFFER_ALLOC/FREE | USB core の同期 staging buffer の確保・置換/最終解放。容量 bytes。DMA coherent のカウンタと別。 |
| USB_BUFFER_RETAINED | drain が失敗し、独立 staging buffer を保持したまま caller が離れる事象。capacity bytes。現在の隔離残量を表す gauge ではない。 |

UFS の total − content は「content helper 以外」の要求であり、新規割当てのゼロ
初期化も含む。UFS2 では journal/snapshot の増幅も含むので、metadata の論理 block 数
に読み替えない。CG/super/inode/zero の transaction 別内訳は p008/p010/p011 の
adapter と一緒に細分化する。p001 で IO02 全体を最終 PASS にしない。

## 受理・完了・永続化の境界

| Owner | 現行状態と境界 | 後段の責務 |
| --- | --- | --- |
| syscall / file_io | user pin、file position、content/resize lease を保有して transfer。返値は受理された file bytes。 | p006 で lease 前の bounded buffer acquisition。 |
| buf | b_busy、b_generation、b_dirty_generation と I/O state。write 完了時も同じ dirty 世代だけ clean にする。 | p007 の複数 line 取得、p014 の stable 世代。 |
| disk / bio | NEW → SUBMITTED → COMPLETED、leaf pin/inflight、callback。submit は inline completion が可能。 | p019 で worker、cancel、callback lifetime を統合。計数のために submit 後の BIO を新たに参照しない。 |
| loop | backing claim、backing file と loop I/O の保持。flush は file_fsync を経由する。 | p014/p018 で下向き drain と再 dirty 化を分離。 |
| vm_object | content/resize/dirty/write 世代と shared mapping の所有権。mapped backing との整合性を保持。 | p015–p018 の単一 content owner と dirty/error retention。 |
| backing_claim | claim token / mutation generation と owner の区間予約。media identity の代用ではない。 | p025 の media generation admission。 |
| USB core / xHCI | URB caller/HCD references。timeout は DMA retirement ではない。sync staging は HCD reference で保持。 | p009 予約容量、p019 非同期実行、p024 SG、p025 media 回復。 |

`BIO_COMPLETE` は永続化の証明ではない。`USB_SYNC_CACHE` もコマンド発行回数であり、
成功数でも媒体への永続化 oracle でもない。現行 fsync/driver の返値と、故障注入・
再起動の内容照合を別に使う。claim/content 世代を stable/media 世代に流用しない。
新しい worker や世代状態機械は p001 では導入しない。

## 再利用する実行経路

- FS50: [run-storage-acceptance.py](../ws018/tests/run-storage-acceptance.py)。
  host 部分は 39/50。native 依存の 11 項目を未実行のまま残すと runner は非ゼロ終了する。
- 六 syscall / short / copy fault / PIPE_BUF:
  [run-storage-syscall-stories.py](../ws018/tests/run-storage-syscall-stories.py)。
  本体を verbatim 抽出し、counter 実装も production module をリンクする。
- UFS publication / rollback:
  [run-ufs-metadata-audit.sh](../ws018/tests/run-ufs-metadata-audit.sh)。
- Wi-Fi30: [run-wifi-stories.sh](../ws005/tests/run-wifi-stories.sh)。
  FS50 host runner が ordinary/sanitize を実行する。
- 新規の観測 oracle:
  [run-io-observation-host.py](tests/run-io-observation-host.py) は concurrent uint64_t
  counters と UFS1/2 の full/partial/error 時の実呼出し数・bytes を照合する。
- [run-io-baseline.py](tests/run-io-baseline.py) は QEMU xHCI USB root、
  SMP4、指定 RAM、disposable GPT/FAT/UFS image で動かす。
  同一 offset の反復上書きと連続 256 KiB 上書きを各101回。
  一回は 4 × 64 KiB + 最終 fsync。内容照合と出力は測定区間外。
  quantile は nearest-rank、guest の時計粒度も結果に記す。

実行結果は [p001 results](phase001/results.md) に保存する。
