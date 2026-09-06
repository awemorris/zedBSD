# ファイルシステム・USBレポートへの回答と対応方針

日付: 2026-09-06
照合対象: `/home/awe/zedBSD`、HEAD `02eeed5` (`Fix for WiFi audit`)
状態: 以下は初回回答時の判断。report-4を受けた更新とq086実装は
[実装計画](fs-report-implementation-1.md)・
[受け入れ結果](ws018-kernel-architecture/phase019-storage-acceptance/results.md)を参照。
初回回答の「まだ実装・Queue・測定していない」という状態は更新済み。

対象レポート:

- [fs-report-1.md](/home/awe/claude/zedBSD/plan/fs-report-1.md): 書き込み経路、指摘1–12
- [fs-report-2.md](/home/awe/claude/zedBSD/plan/fs-report-2.md): syscall、UFS、名前解決など、指摘13–26・F1・F2
- [fs-report-3.md](/home/awe/claude/zedBSD/plan/fs-report-3.md): USB、指摘U1–U15

## 1. 結論

**関連問題として対応する価値が高く、多くは修正可能である。**
特に、512バイト単位のsyscall転送、UFSの呼び出しごとの大きなブロック更新、
loopからFATへの再入、FATのセクター単位write-throughが重なる構造は現行コードでも確認した。
USBでは、CSW STALLの限定的回復の欠落、flush失敗後に回復用flushまで拒否する構造、
長いポーリングと回復失敗後の進行経路不足が優先対象になる。

ただし、レポートの **CRITICALは障害・遅延への影響を示す評価であり、
記載された修正案がそのまま安全に適用できることを意味しない。**
「検証を途中で打ち切る」「dirtyにするだけで戻る」「エラー保持を消す」だけでは、
現在守っている整合性・永続化・DMA所有の契約を失う。
採用する方向は、不要な繰り返しを減らし、失敗後も確認付きで回復を進めることである。

また、1回のwriteが0.25–1秒かかる、常に256コマンドになる、IMODだけで3msの下限がある、
という数値は測定結果ではない。条件付きのI/O増幅モデルとしては有用だが、
実機の根本原因と改善率は、下記のカウンターと同一条件での比較で確定させる。

## 2. net / networkdとの関係

関係は具体的にある。ただし、以前のWi-Fi状態遷移バグすべての原因をFSに帰すことはできない。

| 経路 | 想定される影響と確認点 |
| --- | --- |
| `net wifi set-key` | `wifi-store.c`の一時ファイル書き込み、ファイルfsync、renameat、親ディレクトリfsyncが永続FSを通る。遅延・失敗は保存完了と、その後の通知に影響する。公開後のfsync失敗は「更新されていない」とも断言できない。 |
| `networkd`のプロフィール読込、認証に伴うファイル参照 | 名前解決、read、必要に応じた実行ファイルのロードが遅延し得る。子プロセス待ち中の制御受付を改善しても、daemon自身が同期FS syscallの中にいる時間は別に測る必要がある。 |
| DHCP・resolver更新 | `/etc/resolv.conf`の一時ファイル、fsync、renameと、その後の所有確認・削除が対象。アドレス取得が成功してもファイル更新の失敗でトランザクションが失敗する可能性がある。 |
| `/run/networkd-child.*`、制御ソケット | 通常の`/run`はtmpfsで、すべてが直接USBに書かれるわけではない。tmpfsのページ探索、CPU占有、共有ロックによる間接影響を分ける。 |
| USB-rootとRTL8822BUの併用 | 共通USB core/HCDの回復やコントローラー再起動はストレージと無線の両方に影響する。AX211はPCI側だが、コマンド実行・設定保存・CPU遅延の影響は受ける。 |

根拠: [wifi-store公開処理](/home/awe/zedBSD/userland/base/net/wifi-store.c:1007)、
[親ディレクトリ同期](/home/awe/zedBSD/userland/base/net/wifi-store.c:1104)、
[resolver更新](/home/awe/zedBSD/userland/base/networkd/main.c:5342)。

Q085の30本はproductionのコマンド・daemon・子処理を通したホストテストだが、
FS・無線・L3には外部境界の代替実装がある。
[その結果](/home/awe/zedBSD/plan/ws005-networking/phase012-wifi-command-scenarios/results.md)は有効なまま、
**zedBSDの実際のsyscall→overlay→UFS→loop→FAT→USB経路を追加受け入れ対象にする。**
ホストで30/30だったことを、この保存経路の永続性や実機応答時間の証明には使わない。

## 3. 重要な修正方針

### A. syscallの512バイト分割は優先して改善する

[syscall.c:82](/home/awe/zedBSD/src/kern/syscall.c:82)とread/write/positional/vectorの各実装で確認した。
通常ファイルに対して、まずページ単位、必要なら上限付きのより大きなbounceを使う。
64KiBの配列をカーネルスタックに置く変更はしない。
割り当てはFS・inodeの長い排他を取る前に行い、メモリ不足時の動作を定義する。

単純な定数変更を本実装の完了条件にはしない。readv/writevも対象にし、短いI/O、EFAULT、
途中エラー、O_APPEND、共有offset、RLIMIT_FSIZE/SIGXFSZ、PIPE_BUF以下の原子性、
パイプ・端末・ソケットの既存意味を保つ。pinned user rangeを直接FSへ渡す案は、
連続したカーネル仮想アドレスを渡す現在のAPIと同じではないため、初段では採用しない。

### B. loopのextent fast pathは有力だが、現状のclaimはI/Oマップではない

[loop_collect_extent](/home/awe/zedBSD/src/drivers/loop.c:35)は`file_block`を捨てており、
claimの範囲は排他・認可用である。レポートの「完成済みの変換表を使うだけ」は訂正する。
[swap-source](/home/awe/zedBSD/src/kern/swap-source.c:1240)には別途、
ファイル上の順序を保持したextent、完全被覆の検証、キャッシュ無効化がある。

loopにも論理offset→物理rangeの不変マップを保持し、claimは書き込み認可として併用する。
断片化、extent境界、親partition offset、転送上限、オーバーフロー、部分失敗、
detach中のI/O寿命を検証する。非FATなどのfallbackを明示し、稼働中の安易な経路切替はしない。

最大の前提はキャッシュ整合性である。物理ディスクのcache lineとFATのsector cacheに
古い内容が残ると、直接書いたloopデータを後続の別セクターRMWが上書きし得る。
特にFAT clusterが4KiBより小さい、またはextent端がcache lineと共有される場合、
隣接する別ファイル／メタデータとの競合がある。一般read aliasの扱いも必要になる。

初段は次の二案を比較し、テスト可能な方を採る。

1. **親のbuffer cacheを通るclaimed extent I/O**: FATのchain探索・512B分割を外し、
   親cacheとの整合性を保つ。二重キャッシュは残り得るが、大きな増幅因子を先に除ける。
2. **親cacheを迂回する直接I/O**: range単位の同期・無効化・alias排他を定義する。
   単発のattach時invalidateだけでは十分でない。条件を証明できないrangeには使わない。

swapの`BUF_INVALIDATE_DISCARD`を無条件にコピーして、まだ必要なdirtyデータを捨てない。
flushは親の実際の永続化境界へ伝播させる。claimの重複確認自体も残るので、
レポート1の「§10のコストまで全部消える」は正確ではない。

### C. FAT/UFS/overlayは、永続化の契約を保つ最適化から始める

- FATの全chain検証には「要求範囲より先の破損も書く前に検出する」という明示契約がある。
  [fat_raw_write](/home/awe/zedBSD/src/drivers/fs/fat.c:2190)を途中で打ち切るだけの変更は不採用。
  検証済みchainと変更世代を保持する方式なら、同じ検証の繰り返しを省ける。
  truncateだけでなく、別openからの変更、raw書き込み、失敗rollbackでの無効化が必要。
- FATの連続データ／zero-fillを複数セクターにまとめる。完全上書き部分の先読みを省く。
  mountの単一sector cacheとの同期を先に定義する。ミラー更新の遅延化とは分離する。
- UFSの完全ブロック上書きで先読みを省き、共有inodeブロックの排他を維持する。
  allocationのzero-fillは、未初期化データを指すpointerを公開しない順序が必要で、
  「後で上書きするから全部省く」とはしない。
- overlay copy-upの作業buffer拡大は上限付きで行い、部分書き込み・中断時の一時ファイル、
  metadata保持、journal公開順序を変えない。既存upperへの小さな書き込みと別々に測る。
- fsyncの重複は調査対象だが、ファイルdata/inode、親ディレクトリ、overlay journalの
  どれを永続化したかを整理してから統合する。失敗を返すべき箇所をまとめて消さない。

write-throughは意図的な仕様で、write-backへの変更は今回の最適化の前提にしない。
dirty上限やsyncerがあるだけでは、書き戻し順序、エラーの後続fsyncへの伝達、
raw alias、reclaim中の再帰I/O、shutdownの整合性までは保証されない。
FATミラー・UFS inode/superblockをfsyncまで遅延する案も、別の永続化設計段階に置く。

### D. USBストレージの再試行は、コマンドの種類と失敗した段階で決める

[bot_command_locked](/home/awe/zedBSD/src/drivers/usb-storage.c:289)は、
data STALLのclear-haltは既に扱うが、CSW STALLの限定再読はなく、transport error後の
`bot_reset`の結果も捨てて元のI/Oを失敗させている。

CSW STALLはBulk-In haltを解除してCSWを再読する手順を追加する。
これはコマンド全体を再発行する処理と分ける。
[USB-IF BOT 1.0 §5.3.3、§6.7](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)に対応する。

その他は、CBW/data/CSWのどこまで進んだか、tag/residue、sense、媒体世代、
実際に停止・drainできたかを記録して判定する。
READ、同一LBAへの同一データWRITE、FLUSHそれぞれに再試行可能条件を定義し、
コマンドや各URBごとに時間予算をリセットせず、一つの操作の絶対期限を共有する。
回復に失敗したURB/bufferを次の試行で再利用しない。

UNIT ATTENTIONを`06/xx`だけで一律再試行する提案は不採用。
特に媒体変更を疑うsenseでは、古いmount/cache/claimのまま書いてはいけない。
同じVID/PIDやdescriptorの再確認だけで、同じストレージ内容と証明したことにもならない。

### E. flushの永続エラーは「回復用操作まで閉じる」点を直す

[storage_submit](/home/awe/zedBSD/src/drivers/usb-storage.c:622)は一度`flush_error`が立つと、
後続WRITEだけでなくFLUSHも実行しない。回復確認へ進めない点は修正対象である。

しかし、エラーをただ消す案は不採用。少なくとも、正常／永続化未確認／回復中／媒体無効を
区別し、通常WRITEを止めた状態でも限定的な回復flushを実行できる契約にする。
同じ媒体・cache世代が維持され、未確認の書き込みを永続化できた場合だけ正常へ戻す。
resetで揮発cacheを失った可能性がある場合、**その後のflush成功だけでは過去の書き込みを証明できない。**
FSが既に失敗を受けてwritableを閉じた場合も、ドライバのフラグだけ戻して書き込み再開しない。
FUA、write-cache無効、SYNC CACHEなど現在のデバイス別方針を保つ。

### F. USB/HCD回復は、小さな局所修正と世代全体の再構築を分ける

U3–U5は対応可能だが「小さく局所的な再試行追加」とは評価しない。
`xhci_cancel_request`には既に状態に応じたStop Endpoint、Reset Endpoint、Set TR Dequeue、
restartと最大8段の処理がある。足りないのは、その手順が失敗した後の進行経路と、
command ringが不確定になった場合の回復である。

コントローラー再起動では同じHCD上のストレージ、RTL8822BU、HIDすべてが対象になる。
新規submitを閉じ、旧処理・completionをjoinし、DMA停止を確認し、未完BIOの結果と
旧世代の資源を確定してから再構築する。command timeout後にフラグだけ戻す、
古いringに追加コマンドを積む、旧BIOを新しいdeviceへ移して成功させる変更はしない。

回復所有者は一つにし、通常I/OのlockやUSB topology/selection gateを保持したまま
再帰的にdevice resetする構造を避ける。稼働中のUSB-rootを維持できる媒体同一性・
cache永続性を確認できなければ、エラーと資源保持が正しい終端になる。
故障したUSB-rootが必ず自動復帰するという保証は設けない。

U7/U9の待機改善は、この所有設計と一緒に行う。runtimeは失われないwake条件付きの待機、
early bootは時間上限付きpollとし、IRQ-off中に増えない`sched_ticks()`だけを期限源にしない。
既存のHAL単調counter契約と、未初期化時の扱いを使い分ける。
U14のIRQ中submit-commit待ちは、remote completionを含む実行順序を先に再現する。

## 4. 全指摘の対応判断

「採用」は問題の方向を採用する意味で、レポート中のコード案をそのまま採る意味ではない。
優先度は、P0=初期計測・局所改善、P1=整合性を含む構造改善、P2=後続機能／測定後の改善。

### レポート1

| ID | 判定・優先度 | 対応方針／訂正 |
| --- | --- | --- |
| 1 | 採用・P1 | FAT全chain再検証を確認。validated map/cursor＋変更世代で省く。要求範囲以後の破損検出を黙って廃止しない。loop側ではBを先行。 |
| 2 | 採用・P1 | 512B RMWとflushを確認。連続データの複数セクター転送、完全上書き時のread省略。cache slotとの整合を含む。 |
| 3 | 事実を確認・P2 | write-through自体はバグではない。write-backは初期スコープ外。Cの永続化設計が前提。 |
| 4 | 採用・P0 | copy-upの4KiB転送を確認。上限付き拡大と部分失敗処理。最初の操作が遅い原因の候補で、常に主因とは断定しない。 |
| 5 | 条件付き採用・P1 | loopのextent化を推奨。ただし論理mapの新設とalias/cache対策が必要。claimだけでは変換表にならない。 |
| 6 | 採用・P1 | clusterゼロ化の先読み・細分化を減らす。公開され得る未書き込み範囲のゼロ保証は維持。 |
| 7 | 部分採用・P2 | FATミラー更新コストは存在。ミラーをsyncまで遅延する案は初段不採用。更新・rollback順序を保つbatchを検討。 |
| 8 | 条件付き採用・P1 | file→journal→upperの複数同期を確認。単なる呼び出し数削除ではなく永続化対象・順序・エラー伝達の統合。dirty世代も並行write/flushの定義が必要。 |
| 9 | 部分採用・P0/P2 | 全block先読みの省略はP0。inode永続化のfsyncまでの遅延はP2。inode persistは既に1回のpwriteのループ外。 |
| 10 | 採用・P1 | IRQ-off下のclaim/range線形探索を確認。物理range索引を作る。論理extent順と混同しない。lock外参照には寿命保証が必要。 |
| 11 | 採用・P2 | buf_syncの先頭からの再探索を確認。現状はdirtyが少なく優先度低め。write-back導入前にdirty索引・並行世代を設計。 |
| 12 | 採用・P2 | scratch割り当てを減らす。ただしロック保持・再帰I/O・reclaim中に同じscratchを共有しない。 |

### レポート2

| ID | 判定・優先度 | 対応方針／訂正 |
| --- | --- | --- |
| 13 | 採用・P0 | 512B分割を確認。Aの通常ファイル向けbounce改善、vectorも含める。全種類のfdへの一括定数変更にはしない。 |
| 14 | 部分採用・P0/P2 | UFSのdata/inode RMWを確認。§9と同一課題として処理し、別の増幅因子として二重計上しない。 |
| 15 | 条件付き採用・P1/P2 | cg再読・summary更新・zero-fillを確認。`active_cg==cg`だけでは有効なcacheと証明できない。失敗した読込・rollback後の無効化、allocation公開順序を維持。 |
| 16 | 採用・P0 | next_direntのheader/name別readを確認。ブロック単位検証・走査へ。複数blockの既存directoryも読めることを保つ。 |
| 17 | 採用・P1 | readdir時のoverlay再lookupを確認。upper名集合と世代付きcursorを検討。実層のd_inoをそのまま公開せずoverlayのidentityを維持。 |
| 18 | 採用・P1 | URB通常待機のspinと8KiB上限を確認。U9/U10と統合。最大転送数を増やすだけではreclaim-safe reserveを保てない。 |
| 19 | 採用・P2 | positive-onlyのname cache、128 entries、inodeのfirst-fit evictionを確認。negative cacheはdirectory世代・rename・mount変更を含める。overlayでは実層変化の無効化も必要。 |
| 20 | 部分採用・P1 | loopと物理diskの二重cacheは成立する。ただし内容が常に全部二重で実効容量が正確に半分、とはいえない。Bの親cache経由案では二重cacheが残る。 |
| 21 | 方針課題・P2 | block write-back用syncerの不在は現在のwrite-throughと整合する。syncer追加だけで永続化の問題が解決するわけではない。 |
| 22 | 採用・P1 | 間接blockを都度読む経路を確認。世代付きcache/走査cursorを検討。truncate、block再利用、rollbackで無効化する。 |
| 23 | 部分反論・P2 | ELFのページ単位readと先読み不足は対象。ただし「file page cacheがない」は誤り。VM objectに共有mmap用page cacheがあり、通常readも公開済みobjectから整合readする。既存契約を拡張する。 |
| 24 | 採用・P2 | tmpfsのsorted linked listを確認。大きいファイル向け索引を検討。小さな設定／制御ファイルの主因と断定しない。 |
| 25 | 採用・P2 | NVMeの4KiB bounceは別媒体の後続上限。今回のUSB-rootの直接原因ではない。DMA/PRP/reclaim契約と一緒に扱う。 |
| 26 | 採用・P2 | FAT列挙の先頭からの走査・path再解決を確認。generation付きcursorを検討。「boot時だけ」と限定せず直接FAT利用にも適用。 |
| F1 | 採用・P1 | UFS1 directory更新の1block制限を確認。host builderは複数blockを生成できるため、単なる容量上限より重要なproducer/consumer不一致。WS024と統合して修正。 |
| F2 | 採用・P2 | overlayはrewindのみ。`..`の再探索も存在。cookie/seek・rename時の世代契約を定めて拡張。通常Wi-Fi設定の直接原因とは未確認。 |

### レポート3

| ID | 判定・優先度 | 対応方針／訂正 |
| --- | --- | --- |
| U1 | 採用・P0/P1 | CSW STALL再読は局所対応。コマンド再試行はDの失敗段階・sense・期限・媒体同一性・drainを条件にする。reset結果を捨てない。 |
| U2 | 採用・P1 | 回復flushまで拒否する点を修正。過去の永続化失敗を消すだけの解除は不採用。E参照。 |
| U3 | 条件付き採用・P1 | quarantine後の回復不足を確認。ただし受理済みwire request失敗は実機状態が不明で、単なる無害なwire errorと分けられない。Fのchecked recoveryを実装。 |
| U4 | 採用・P1 | command_failedの保持とruntime再初期化不足を確認。command abort/停止確認・世代再構築まで一体で対応。フラグのクリアだけは不採用。 |
| U5 | 採用・P0（report-4で訂正） | 元のwait_reusableはcancel失敗後に無期限待機した。HCDの再cancel自体は可能でも、このcallerの進行保証にはならない。q086は同期buffer隔離＋有限cancel/drain＋保持中reuse拒否を実装。DMAの偽完了や未確認解放は行わない。 |
| U6 | 採用・P0/P1 | unchangedなconnected未列挙portに自律retryがない点を確認。port世代＋回数＋retry期限を導入。単にobservedを消す方式では無制限再試行になり得る。 |
| U7 | 採用・P1 | command中のIRQ-off pollと回数上限を確認。ただし停止するのはまず実行CPUの割り込みで、SMP全CPU停止という説明は過大。正常EP0でも必ずcommandを発行する、という説明も誤り。 |
| U8 | 条件付き採用・P0 | IMOD=4000は確認。0/160/4000等の比較を行う。1msは割り込み間隔で、各転送に必ず1ms加算される意味ではない。3ms固定下限・必ず0.75秒は不採用。 |
| U9 | 採用・P1 | URB待機・usb_delay_ticksのspinを確認。runtime待機とearly bootを分け、lost wake、timeout/cancel競合、reclaimを検証。 |
| U10 | 部分採用・P2 | HCD endpoint activeは1件。USB coreの待ち行列・異endpoint並行と区別する。大きいBIO/通常DMA最適化は可能だが、BOTコマンドを勝手に並列化しない。NO_DMA_MAPの定義だけで直接map実装済みとはいえない。 |
| U11 | 採用・P1 | HIDの非STALLエラー後停止を確認。drain後に有限回再armし、連続故障・抜去時は停止。重複input公開とキー押下状態の残留も検証。 |
| U12 | 機能追加・P2 | 外部hub class未実装を確認。root-port前提の別機能開発であり、今回のroot遅延修正の前提にはしない。 |
| U13 | 診断のみ採用・P1 | stage付き診断・panic経路の整備は有効。壊れた参照数や所有状態のままquarantineして継続する一律変更は不採用。trapが必ず無言resetになるという断定もできない。 |
| U14 | 要再現・P1 | terminal公開内のsched_yield経路を確認。到達するならIRQ文脈での待機は重大なので優先検証する。remote submit/complete/cancelを交差させ、必要なら終端公開の引き渡しを非待機化。 |
| U15 | 機能追加・P2 | CAPACITY(10)の上限を確認。容量だけでなくREAD/WRITE(16)・block型・範囲確認を一緒に拡張。2TiBという境界は512B論理sectorの場合で、全デバイス共通ではない。 |

## 5. レポートへの主な反論の根拠

1. **ページキャッシュは既にある。**
   [vm_object_fault](/home/awe/zedBSD/src/kern/vm-object.c:1079)はcached pageを保持して再利用する。
   [file_io_transferのcoherent read](/home/awe/zedBSD/src/kern/file.c:957)も存在する。
   exec間で常に内容を保持する汎用cacheとは違うが、「存在しない」から設計を始めてはいけない。
2. **FATの全chain検証は意図的な契約。**
   [fat_raw_write](/home/awe/zedBSD/src/drivers/fs/fat.c:2214)に要求範囲以後の破損検出が明記されている。
   最適化するなら同じ検証結果を再利用できる根拠が必要。
3. **extent fast pathにも認可とcache処理が残る。**
   [claimed direct write](/home/awe/zedBSD/src/kern/disk.c:1308)はclaim検査を迂回するAPIではない。
   [swapのcache無効化](/home/awe/zedBSD/src/kern/swap-source.c:267)まで読んで移植境界を定める。
4. **xHCI停止段階は実装済み。**
   [xhci_cancel_request](/home/awe/zedBSD/src/drivers/pci-xhci.c:2006)を単純な失敗即保持とみなさない。
   また[endpoint recovery](/home/awe/zedBSD/src/drivers/pci-xhci.c:1538)は状態によって即成功し、
   全EP0転送ごとのReset commandではない。
5. **IMODとBOT queueの意味。**
   IMODは最小割り込み間隔を制御し、counterは前の割り込み時から進む。
   したがって3回の完了を一律3msの追加待ちへ換算できない。
   [Intel xHCI §4.17.2 / §5.5.2.2](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf)。
   またBOTは前のCBWに対するCSWを受ける前の次CBW送信を禁止する。
   HCDのTD並列化とBOTコマンドの並列化は別である。
   [USB-IF BOT §3.4](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)。
6. **ディレクトリ制限は生成側との不整合。**
   [UFS1 dir_add](/home/awe/zedBSD/src/drivers/fs/ufs1/ufs1-vfs.c:990)は1blockまでだが、
   [zedimage-host finish_dirs](/home/awe/zedBSD/tools/build/zedimage-host.c:67)は複数blockを生成できる。
   レポートの「formatterも確認すべき」は確認できた。読み取り成功と更新可能性を分けて試験する。

## 6. 実装の段階案とWSの担当

以下は実装をQueue化するときの分割案であり、今回Phase IDを予約したり完了済みQ085を再開したりはしない。

| 段階 | 作業と依存 | 完了条件 | 担当候補 |
| --- | --- | --- | --- |
| A: 計測基準 | syscall/FS/BIO/BOT段階、flush、chain歩数、IRQ-off、実時間・CPU時間を集計。既存traceも活用 | 同一イメージ・同一操作で再現し、tmpfs・FAT直書き・overlayの比較を保存 | WS018＋WS004、net観測はWS005 |
| B: 局所的な増幅削減 | A後。通常ファイルbounce、UFS全block先読み省略、directory block走査、copy-up chunk改善。IMOD比較は独立実験 | 従来機能・途中失敗を保ち、対象のFS呼び出し数／BIO数／CPU費用が減る | WS018、IMODはWS004 |
| C: loop/FAT | A後。論理extent map、claim/alias整合の設計を固定して実装。FAT連続I/Oと検証済みcursor | 断片化・sub-page cluster・隣接file・raw alias・detach競合で破損なし、増幅減少 | WS018＋WS004 |
| D: BOT・媒体永続性 | A後。CSW STALL、sense別・段階別retry、総期限、flushの回復用状態 | 単発故障から回復、永続故障は真のエラー、reset後に失われた書き込みを成功扱いしない | WS004 |
| E: USB実行・回復基盤 | Dの状態契約と協調。runtime wait、command期限、列挙retry、U14確認、必要なHCD回復所有者 | lost wake・remote completion・cancel/detach競合なし。旧DMA/世代を再利用せず、可能な回復は自律進行 | WS004、HIDはWS006と連携 |
| F: UFSと名前解決の構造改善 | B/C結果を受けてallocation/indirect/cacheを改善。F1は統一UFSの機能要件へ | 複数block directory、rename/truncate、失敗rollbackと再起動の一致 | WS024中心、WS018/WS019の既存受け入れ再利用 |
| G: 統合受け入れ | 対象実装が揃った後。既存30話＋実FS保存経路＋USB障害・負荷＋再起動 | 各結果、永続データ、errno、応答時間、未完処理が一致 | WS005＋WS003/004 |

既にユーザーが決定した[WS024の単一64bit UFS方針](/home/awe/zedBSD/plan/ws024-unified-ufs/ws.md)は維持する。
現状のbuild backendは依然UFS1 imageを生成しており、今回の指摘もその稼働経路に当たる。
syscall・loop・FAT・USBの改善は統一作業から独立して進められる。
一方、新しいUFS directory機能や大規模なinode永続化設計をUFS1専用に作り込み、
後で二重実装することは避ける。統一時に現行UFS2にも同種の問題がないか確認する。

遅延write-back、汎用page cache拡大、UAS/深いqueue、外部hub、大容量SCSIは後続候補。
これらすべてをWi-Fi修正の完了条件に束ねない。

## 7. 検証計画

### 性能と応答

- 512B/4KiB/8KiB/64KiBのoverwrite、append、fsyncを分離。cold/warm、初回copy-up/既存upperも分離。
- data imageの32/64MiB、連続/断片化、512B/4KiB/32KiB FAT clusterで、実際のgeometryを記録。
- syscall回数でなく、backend呼び出し数、loop/物理BIO数とbytes、FAT chain歩数、flush数を別々に集計。
- IMOD比較ではI/O時間だけでなくIRQ率、CPU時間、HID応答、RTL8822BUとの同時利用も測る。
- daemon受付待ち、storeのwrite/fsync/rename、exec、DHCP、radio待ちを別計時。
  p50/p95/最大値を保存し、遅い保存を無線接続時間へ混ぜない。改善率の事前保証はしない。

### 故障と整合性

- sector/block境界、短いread/write、各metadata更新の失敗、ENOSPC、割り当て失敗、
  fsync成功前後、rename公開前後を注入。fsyncの成功が再起動後の期待内容に対応することを確認。
- extent共有cache lineの隣接ファイル書き込み、直接read alias、raw書き込み拒否、
  active claim中のtruncate/rename/unlinkとdetachを確認。古いcacheで新データを上書きしない。
- BOTのCBW/data/CSW STALL、CSW不正tag/residue、CHECK CONDITION、失敗reset、
  command timeout、遅延completion、二重terminal、media交換を段階ごとに注入。
- flush失敗→回復成功／継続失敗／cache喪失の疑いを分け、成功偽装、未完BIO放棄、
  古いDMAへの再利用、永続エラーの無言解除がないことを確認。
- USB storage＋RTL8822BU＋HIDを共有HCDで試験し、一つの回復が他を黙って破壊しないことを確認。
- 外部モデルだけで成功を返すテストにせず、production FS/USB処理を通す。
  使用中の起動媒体を揺らして故障を作る方法は採らず、使い捨てイメージと制御したfault注入を使う。

### 既存資産と最終受け入れ

既存のFAT native/consolidation、UFS independence/consistency/metadata、storage foundation、
xHCI concurrent/recovery、USB storage/no-media、HIDのfixtureを適用範囲に応じて再利用する。
例: `plan/ws018-kernel-architecture/tests/run-fat-native-vfs-host-test.sh`、
`plan/ws004-hardware/tests/run-usb-recovery-contract-test.sh`、
`plan/ws019-installation/tests/run-storage-foundation-test.sh`。
既存fixtureがcodecだけを試す場合は、今回変える実行・回復経路を追加する。

最後に30本のnet Wi-Fi host回帰を再実行し、zedBSD上の実際のstore更新・通知・
disable/recovery、USB-rootでの保存・再起動まで確認する。
無線の30話すべてをQEMUだけでRF検証できるとは扱わず、deterministic radio境界と
RTL8822BU/AX211実機の範囲を分けて記録する。
amd64 runtimeは`qemu-system-x86_64`、イメージは使い捨てコピー、
buildはamd64/PCAT/PC98を直列の`make -j16`で行う。aggregate `make check`は使わない。

今回行ったのは3レポート・現行ソース・build生成経路の静的照合と、BOT/xHCI仕様の確認である。
遅延値、実機での再現頻度、媒体上の破損、個々のCRITICAL指摘の実測再現は未確認である。
