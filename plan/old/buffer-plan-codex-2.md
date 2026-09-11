# buffer 設計の再整理と段階導入方針

日付: 2026-09-07

照合対象: `/home/awe/zedBSD` HEAD `dd4f31c` (`Fix syscall write`)

状態: 2026-09-07 に方針承認済み。独立した [WS025](../ws025/ws.md) に詳細設計と Phase を登録。実装 Queue は未選択。

承認時に追加された amd64 の 1 GiB 制限解除、loader の全 RAM 報告、DMA 制約付き allocation は [WS025 の memory 設計](../ws025/memory-design.md) に記録する。本書は承認時の方針として保持し、細部は同 WS の P-book で具体化する。

対象: 機能性、性能、障害時の整合性。静的確認による設計案であり、性能値は未計測。

関連: [原案](buffer-plan.md)、[前回答](buffer-plan-codex-1.md)、[今回のレビュー](buffer-plan-claude-2.md)

## 1. 私の考えと、前案から変更すること

**スタブから段階的に実装する方向に賛成する。前案の「対象外」は、最初の実装範囲を限定する説明としてはよかったが、最終的にどこまで直すかの説明として不足していた。**

64 KiB の既存ファイル上書きを一回の data command にするだけでは、新規作成・追記・小書込み・繰り返し読込みの問題は残る。これらも到達目標に含め、metadata batching、キャッシュの寿命と回収、write-back、先読みを一つの段階計画として扱う。本書と前回答が矛盾する箇所は本書で置き換える。

ただし、前案の対象外を全部実装しなければ、現在の問題が一つも解決しないわけではない。要求の分割と毎回の物理連続確保は初期段階で解消できる。UAS や user page 直接 DMA は、その後の計測で必要性を判断する。各段階を単独でも使える改善にしつつ、後で作り直す原因になる所有権・永続化・回収の契約を先に揃える。

「スタブ」は、成功を返すだけの空実装ではなく、**契約どおりに動く既存経路へのアダプタ**を基本とする。将来機能の API を全部先に追加するのではなく、次の段階で利用する最小の境界から導入する。

## 2. レビューの採否

| 指摘 | 判断 | 今回の方針 |
| --- | --- | --- |
| F1: pool が CPU 数程度では不足し、512 B fallback が重い | 問題提起を採用。待機方法は修正 | 64 KiB pool を増やし、4 KiB の予約済み fallback を用意する。inode lease 内で pool を待つ案は採用しない。 |
| F2: 新規作成・追記は allocation metadata が支配する | 採用 | UFS run の次の優先課題に allocation batching を置く。全 block を data で初期化できる場合の zero 省略も含める。 |
| F3: 自前 reset 直後の `06/28` を特例 retry | 問題提起を採用。一般規則としての特例は不採用 | 自前 reset 後の復旧は試験対象にするが、同じ INQUIRY/capacity だけで mounted root を復帰させない。§11。 |
| F4: exec の bounce は PAGE_SIZE のまま | 採用 | 初期 pool 段階に exec の chunk 拡大を含める。content lease によるスナップショットの意味を維持する。 |
| F5: `buf_reclaim()` の呼出し元がない | 事実関係を訂正し、VM 連携不足は採用 | `buf_set_max_bytes()` と `reserve_bytes()` から呼ばれている。VM の reclaim との接続がないことが課題。 |
| F6: USB reservation の容量を計数する | 採用、計数対象を拡大 | HCD の DMA だけでなく core staging、request、alignment、隔離中の領域まで含める。 |
| F7: pool、cache/UFS、USB を独立して進められる | 採用 | 開発・静的レビューは並行可能。依存順で統合し、共有ビルドと実行試験は直列にする。 |

F2 の「約 34 command」、M の「metadata 3〜4」、F4 の「75 → 5」はモデル上の予測として扱う。実数は block 配置、cache hit、indirect、FAT/overlay、flush、エラー再送に左右される。これらを無条件の受け入れ値にはしない。

静的確認した主な箇所は、[buf.c](/home/awe/zedBSD/src/kern/buf.c:474) の reclaim と内部呼出し、[vm-reclaim.c](/home/awe/zedBSD/src/kern/vm-reclaim.c:967) の回収先、[elf.c](/home/awe/zedBSD/src/kern/elf.c:685) の `copy_segment_snapshot()`、[ufs1-vfs.c](/home/awe/zedBSD/src/drivers/fs/ufs1/ufs1-vfs.c:111) の CG 読込みと allocation/write 経路である。

## 3. 最終像と責任の置き場所

通常 I/O の初期共通上限は前案どおり 64 KiB とする。上限は物理連続性、FS の mapping 境界、device 制限によって短くなり得る。先読み window はこれより大きくできるが、複数の bounded I/O で満たす。

| 層 | 最終的に担当すること |
| --- | --- |
| syscall / exec | user copy と bounded buffer の貸出し。ファイルキャッシュの永続的な所有者にはしない。 |
| VFS / VM file object | 通常ファイルの内容の一貫性、resident page、mmap/read/write の共有、dirty 内容の主たる所有者。 |
| filesystem | logical offset の mapping、割当予約、metadata の更新と公開順序、truncate、journal/recovery。 |
| buffer cache | filesystem metadata と block I/O のキャッシュ、連続 run。移行中の data の複製は容量と無効化規則を持たせる。 |
| disk / loop | submission/completion、転送制約、claim と世代、下位まで届く flush。上位と別の dirty 内容を無制限に蓄えない。 |
| USB / HCD | BOT の直列実行、転送用予約領域、DMA 完了・取消し・隔離の寿命、媒体状態。 |
| VM / allocator | resident memory 全体の予算、clean page の回収、dirty 書戻しとの協調、将来の vmap。 |

空いている仮想アドレス範囲を確保するだけでは cache 容量は増えない。利用可能な**物理ページ**を需要に応じて cache に割り当て、アプリや DMA に必要になれば返す仕組みが必要である。巨大なファイルサイズ分を先に commit せず、resident page と管理情報だけを計上する。kernel vmap は非連続物理ページを連続仮想領域として扱うための別の基盤であり、回収方針そのものではない。

前案で対象外にした項目は次のように位置づけ直す。

| 項目 | 位置づけ |
| --- | --- |
| UFS/FAT metadata batching、allocation/indirect cache | 必須の後続段階。新規作成・追記の改善に直結する。 |
| write-back、dirty watermark、syncer | 必須の後続段階。まず既存割当済み data、次に recovery を備えた metadata。 |
| 汎用 file page cache と VM reclaim 連携 | 到達目標に含める。保持寿命と単一の内容所有者から実装する。 |
| 先読み、実際に呼出元を待たせない I/O | 到達目標に含める。同期 worker から始め、需要読込みを優先する。 |
| scatter/gather、kernel vmap | 非連続ページを扱う後続基盤。初期 batching や write-back の前提にはしない。 |
| exec の page 共有、user page 直接 I/O | 前者は snapshot 契約を定義後、後者はコピー費用が支配する環境での測定後。 |
| NVMe 多重発行、UAS | device 別の拡張。現在の USB BOT 改善の完了条件にはしない。 |
| BOT command 並列化 | 導入しない。同じ BOT interface の command は直列に保つ。 |
| 実媒体交換後の既存 mount 継続 | 導入しない。旧世代を切り離し、新しい mount として扱う。 |
| 測定前の IMOD 変更 | 機能の積み残しではない。実機測定という導入条件を維持する。 |

## 4. スタブ導入時から固定する契約

### 4.1 未対応を成功で隠さない

各機能は「既存経路のアダプタ → 限定した対象で有効化 → 障害試験 → 既定化」の順とする。

- allocation API の `commit` が何もしなくてよいのは、互換実装が既に全処理を終え、保留した metadata がない段階だけ。これを atomic transaction と呼ばない。
- delayed write の要求を同期処理で満たすことは可能。ただし実効 policy を `through` と表示し、遅延が有効になったとは報告しない。
- async capability は、submission が device 待ちをしない実装でのみ公開する。既存同期処理に名前を付け替えるだけでは公開しない。
- SG 非対応時は、検証済みの範囲を順に転送するアダプタに落とす。未対応 flag を黙って無視しない。

設定値は観測可能にするが、すべてを任意の時点で書換え可能な sysctl にはしない。pool/DMA 容量変更には貸出しの drain、write-back OFF には書戻し完了が必要であり、失敗時は元の有効状態を保つ。

### 4.2 書込み受理・転送完了・永続化を分ける

1. **受理**: 所有する cache に内容を保持した。write-back の `write()` はここで返せる。
2. **転送完了**: 下位 driver が書込みを完了した。device の volatile cache にある可能性が残る。
3. **永続化**: 必要な flush/FUA と順序制約を満たした。`fsync()` 成功が保証する境界はこちら。

この区別は metadata batching の段階から必要である。新 block の data write 完了直後に inode pointer を書くだけでは、device が永続化順を逆転する場合の保証にはならない。

### 4.3 待機・再入・所有権

pool、dirty credit、cache line の確保に共通して「その資源を返す処理が必要とする lock/lease を持って待たない」を原則にする。loop 再入では同じ処理を別 thread に移した場合も含めて claim・世代・I/O の目的を伝える。thread-local な再入状態だけに依存しない。

非同期 I/O は BIO、segment 配列、page、disk/claim、DMA mapping の参照を完了または確実な取消しまで所有する。呼出元の stack は使用しない。完了通知を一度だけ行い、waiter が解放できる状態を公開した後は BIO を参照しない。callback が submit の返却より先に実行される場合も扱う。拒否と受理後エラーの通知規則を明記する。

## 5. 初期の粒度改善を、後続設計につながる形に直す

### 5.1 pool と exec

64 KiB pool は `max(16, 2 × online CPU)` を希望要素数とし、**実確保量**を RAM の 1/64 かつ 4 MiB 以下に制限する。これは候補の既定値であり、小容量機に 1 MiB を強制する下限ではない。要素数だけでなく allocator header と page rounding を予算に含める。

同じ予算内に 4 KiB fallback reserve を置く。初期配分は予算の最大 1/4 を小要素に充て、残りを大要素にする。構築時に実現できた個数を公開し、低メモリでは減数できる。fallback のたびに固定 heap を確保し直す方式は採用しない。両 pool が空のときにだけ、既存の小さい stack 経路を最後の前進手段として残す。

初期実装は lease 取得後の try-borrow を維持する。**10 ms 待ちを lease 内に追加する案は採用しない。** 借り手は FS lock、下位 I/O、loop 等を待つため、「借り手は pool 以外を待たない」というレビューの前提は成立しない。将来待機を導入するなら、user page pin・file/content lease より前に有限待機し、lock 順序と公平性を再検証する。

通常負荷では大要素の fallback 率 1% 未満を調整目標とするが、無制限の並行負荷に対する保証や、自動増設条件にはしない。大小 pool の利用率、枯渇理由、同時使用数、fallback latency、予約した実 bytes を測る。

exec もこの pool を使い、`copy_segment_snapshot()` の一回の読込みを最大 64 KiB にする。既存の content lease を維持し、短い segment、非整列、BSS、途中失敗を扱う。これにより上位の 4 KiB 分割は減るが、実 device command 数の削減は下位 run と cache の状態次第である。

### 5.2 buffer cache / UFS run

前案の「caller の連続入出力領域を直接使う」は維持する。追加の共有 staging は不要である。ただし複数 line を保持したまま、従来の待機可能な `acquire_line()` を順に呼ぶ設計は修正する。

必要な管理領域の準備は multi-line busy を持つ前に行う。run の line は try-acquire で揃え、競合時は既に取った line をすべて解放して bounded retry または短い run に戻す。下位 FS の再入時に上位 line の解放を待つ経路、allocation/reclaim が同じ line に戻る経路を試験する。単なる block 順ロックだけで再入の循環は解消しない。

read は hit/miss 境界、write は完全 line と部分 line、UFS は hole・mapping・indirect・物理的不連続の境界を守ってまとめる。部分完了と失敗では、完了した範囲だけを反映し、未完了・不確定な内容を clean にしない。

### 5.3 USB reservation と allocator

normal bulk 64 KiB と reclaim-safe 8 KiB の資源を分け、core staging と HCD DMA の双方を予約する。通常 bulk を 2 本予約する構成なら payload だけで core 128 KiB + HCD 128 KiB = 256 KiB/device となり、さらに管理情報・alignment・小 reserve が加わる。4 KiB page では 64 KiB は 16 page であり、allocator header を持つ確保の実 page 数はさらに増え得る。

attach 失敗の巻戻し、部分予約時の 8 KiB fallback、複数 device、timeout 後の隔離 bytes を計上する。DMA 停止を証明できない reserve を再貸出しせず、再試行のたびに無制限に代替を増設しない。

allocator の rotor/word scan は独立して進める。IRQ-off 外で候補探索する拡張は、bitmap 読取りと再検証の同期契約を備えてから行う。pool 化で hot path の確保を減らすことと、allocator 全体の探索時間を短縮することを別々に測る。

## 6. M: metadata batching を前倒しする

### 6.1 UFS allocation の単位と公開順序

M0 を採用するが、`cg_dirty` 等の flag だけを持つ transaction では不足する。現在の mount にある CG 作業 buffer は共有かつ切替え可能なので、更新対象・旧内容・予約した block・公開済み範囲を保持し、並行 allocation と競合しない単位を定義する。

最初は一回最大 64 KiB、単一 CG 内の bounded な allocation batch とする。別 CG や管理領域の上限に当たれば batch を区切る。既存処理を呼ぶアダプタから始め、次に CG/super/inode の重複更新を減らす。

守る順序は次のとおり。

1. block を予約し、他の割当てに渡さない。公開する pointer/size は transaction 内の作業像に置く。
2. 各 block の全 bytes を zero または user data で初期化する。全面を data で埋める block は先行 zero を省略できる。部分 block と隙間は初期化が必要。
3. allocation 情報と初期化済み data を、pointer 公開に必要な永続化境界まで進める。
4. indirect/inode pointer と size を整合する順序で公開する。最終 `fsync` は残る metadata と下位 flush まで完了する。

data と無関係の inode 属性更新が、同じ inode block 内の未準備 pointer を一緒に流してはいけない。共用する in-memory inode を先に変更して `persist_inode` だけ遅らせる設計にはしない。

`abort` は `void` にせず、巻戻し完了・既に公開・結果不確定を表現する。pointer が永続化された可能性がある block を即 free に戻さない。解除の永続化を確認できなければ保留し、必要なら filesystem を書込み停止にする。失敗した dirty metadata の後日の再送で、取り消した pointer が復活しないことも保証する。

この batching 自体に crash atomicity はない。journal 導入前の crash では未到達の割当てが残り得るため、「どの瞬間でも fsck 修復不要」とは約束しない。未初期化 block の参照と二重割当てを防ぎ、どの残骸を checker/recovery が処理するかを明示する。command 数の削減のために必要な永続化 barrier を省かない。

### 6.2 UFS/FAT の metadata cache

- CG は番号一致だけで hit にせず、有効 bit・mount/media 世代・dirty の責任を持つ。初期値の `active_cg == 0` を有効な CG 0 と誤認しない。切替えと rollback も無効化条件に含める。
- indirect block は inode ごとに無条件で 8 KiB を常駐させない。既存 buffer cache の参照・pin を使うか、共通予算に入る bounded cache とする。block の再利用、truncate、pointer 更新、rollback、世代変更で古い内容を使わない。
- FAT の複数 slot 化は、まず clean read cache から始める。dirty slot を複数残す変更は書込み順序の変更なので、後の metadata transaction と一緒に導入する。
- chain cursor/検証結果は file offset だけでなく共有 chain generation に結びつける。別 open、truncate、延長、free/reuse、rollback、媒体変更で更新する。
- FAT mirror の遅延は単独の性能 flag として既定化しない。primary と mirror が異なる世代になったときの検出・復旧を定義した後に有効化する。

FAT は clean cache/cursor の次に、一つの FS 操作に閉じた metadata batch を入れる。最初は既存処理へのアダプタ、次に上限付きの sector 作業像を保持して同じ sector の重複更新をまとめ、操作の既存の完了境界までに primary/mirror/directory を必要な順序で書く。途中失敗と rollback の結果を確認し、結果不確定な chain を利用・再割当てしない。この同期 batching と、操作をまたいで dirty を残す write-back は別段階である。後者の recovery は W-meta で個別に評価し、根拠が揃うまでは同期 batching の状態で運用する。

### 6.3 flush の重複排除

M2 の目的は採用するが、「overlay の fsync は SYNCHRONIZE CACHE 最大一回」を普遍的な完了条件にしない。journal commit 等が flush 間に新しい write を発生させるなら、異なる永続化境界であり統合できない。

leaf disk には accepted write の世代、完了した連続範囲、flush 済み範囲を持たせる。flush は対象世代の write 完了を待ち、成功後にのみ永続化済み範囲を進める。同時 flush は同じ範囲の要求だけ合流でき、後から受理した write は前の成功に含めない。部分・失敗・結果不確定な write、reset、media generation 変更は証明を無効化する。

mount/overlay/loop は別に論理的な dirty 世代を持つ。まだ BIO になっていない dirty page がある状態を、leaf の「新しい write がない」だけで同期済みと見なさない。device が flush/FUA にどう対応するかも含め、**同じ永続化済み範囲への重複 flush だけを省く**。

## 7. P0: file page cache の寿命と回収を先に設計する

レビューの `get_shared()` を呼ぶだけのスタブは、API の試作にはなるが、繰返し read を速める cache にはならない。現在の [vm_object_put()](/home/awe/zedBSD/src/kern/vm-object.c:263) は最後の mapping が離れた clean object を破棄する。read のたびに get/put するだけでは、二回目の `cat` まで保持されない。

そこで mapping reference、実行中 I/O reference、cache 自身の保持 reference を分ける。最後の利用者が離れた clean object/page は予算内で残り、LRU 等の回収候補になる。inode/file/mount の寿命も保持し、unmount、truncate、overlay copy-up、media change で切離せるようにする。dirty が残る object は黙って破棄しない。

read cache は最初は write-through と組み合わせる。通常 read、mmap、通常 write の内容世代を揃え、page fill は page-cache 経路へ再帰しない内部 I/O とする。object 取得と file/content transaction の順序も既存 VM 経路に合わせる。overlay は実層 inode を使い、copy-up 時に旧 lower の page を upper の新内容と混同しない。

loop の claimed backing file は、現在の VM object 利用拒否・迂回規則を維持する。上位 UFS の file page と、その内容を含む FAT backing file の page を独立した書戻しの所有者にはしない。

`buf_reclaim()` は存在し cache 内部で使われている一方、VM pressure からの回収には接続されていない。まず VM が clean buffer/page を回収できる入口と全体予算を作る。VM lock の下で FS/USB writeback を呼ばず、dirty は予約資源を持つ worker に処理を依頼する。実行中 I/O、pin、swap 用 reserve、DMA 隔離中の領域を回収可能 bytes に数えない。

「exec のたびに必ず USB から読み直す」というレビューの表現も修正する。既存 block cache に残っていれば現在も device read は省かれる。page cache の効果は、保持可能な working set の拡大、VM との共有、下位 mapping/copy の削減まで分けて測る。仮の二重 cache には共通上限と除去段階を設定し、単に固定 16 MiB cache の上へ無制限の cache を足さない。

## 8. W: write-back はエラーと順序から導入する

### 8.1 W0: 観測と ownership。まだ遅延しない

dirty object/page/metadata に所有 inode・mount・disk generation、dirty 世代、書戻し中世代を関連づける。索引は既存 key lookup と ready queue を組み合わせ、dirty を付けるたびに block 昇順リストを全走査する実装は避ける。run の近傍検索と enqueue の費用を分けて測る。

error sequence と実際の errno を記録し、**通知を消費する cursor は open file description ごと**に持つ。同じ inode の一つの cursor を全 fd で共有すると、ある fd の fsync が他の open のエラーを消してしまう。mount sync 等には独立した観測者を設ける。この考え方は [Linux errseq の観測者ごとの cursor](https://docs.kernel.org/core-api/errseq.html) と一致するが、レビューの inode 共用 cursor 案はそのまま採用しない。

fsync 中に新しく発生したエラーを、未報告のまま cursor 更新で飛ばさない。対象 dirty の再送に現在も失敗するなら、過去エラーを一度報告済みでも fsync は失敗する。close の一回のエラーだけを通知経路にせず、close を暗黙の全永続化保証へ変更しない。

### 8.2 W1: 最初に遅延する対象を限定する

最初は opt-in mount の**既存割当済み通常ファイル data**だけを、上位の file cache を主な所有者として遅延する。新規 allocation、size/pointer 公開、FAT table/directory、journal は従来の同期・順序規則を維持する。必要な data を永続化してから metadata を公開する。

`write_block()` を呼んでいるから data、といった関数名による分類はしない。特に FAT から見た loop backing file の data は上位 UFS の inode/bitmap でもある。レビューのように UFS data、loop、FAT backing を一括で遅延対象にすると、上位が待った write を下位が再び dirty にする恐れがある。

内部 I/O に「上位 writeback の drain」「順序付き metadata」等の目的を伝え、loop/FAT を通っても下位の遅延 queue に戻さない。通常 write の遅延と、fsync/syncer が下位へ送り出す処理を区別する。将来多層で dirty を持つ場合は、各層の完了と永続化の連鎖を明示的に拡張する。

syncer は初めは同期 I/O を行う worker でよい。周期だけでなく dirty age、量、明示的 fsync で起動し、範囲・世代を固定して書き出す。同じ page が書戻し中に再度更新されたら新しい dirty 世代を残し、古い完了で clean にしない。DMA が参照中の内容を変更しない仕組みも必要である。

### 8.3 W2: watermark と障害時の前進

high/low は候補として 40%/20% から測るが、全体と device ごとの bytes 上限、最古 dirty age も持つ。一台の停止 device が全メモリを占有しないよう、write の前に dirty credit を予約する。

credit 待ちは inode/content lease 等を取る前に行い、内部再入は予約済み credit を引き継ぐ。FS lock を持った `buf_write_ex()` 内で単に sleep する方式は採用しない。reclaim/syncer 用の最低資源を別に確保し、clean reclaim → worker の書戻し → bounded な待機/エラーの順を定義する。dirty data を捨てて容量だけ回復させない。

device が停止している場合に「次の周期までに必ず書ける」とは約束しない。試行開始の期限と完了を区別し、signal、timeout、書込み停止、診断と再試行の動作を決める。unmount/shutdown は drain の失敗を隠さない。

### 8.4 W3/W4: metadata write-back と既定化

metadata を全体的に遅延するには、data-before-pointer、allocation-before-reference、unlink-before-free の依存関係と、crash 後の回復が必要である。局所的に `flush_range(data)` を一行追加するだけでは、複数 inode、同じ metadata block、rename、truncate、mirror 等を扱えない。

この段階は [WS024 の単一 64-bit UFS 方針](../ws024/ws.md) に接続する。現 UFS1 で初期 batching と回帰を整えつつ、長期の journal/metadata write-back は統合先 UFS の責任とする。既存 UFS2 の journal/snapshot を評価し、足りない commit/replay/barrier を補う。UFS1 専用の大きな新 journal を作ってから捨てる順序にはしない。

初期方針は ordered data + metadata journal とし、replay と torn write の検出を含めて契約化する。FAT は同じ保証が自然に得られるわけではないため、独立した recovery 根拠を備えるまで metadata/mirror の即時規則を残す。既定化は FS・mount・device ごとに行い、グローバル flag 一つで全経路を有効にしない。

## 9. S/P: async、先読み、SG、exec 共有

現在の [USB storage submit](/home/awe/zedBSD/src/drivers/usb-storage.c:696) と [NVMe submit 経路](/home/awe/zedBSD/src/drivers/pci-nvme.c:1733) は下位転送の完了を待つ。`b_done` があることと、`bio_submit()` が呼出元を待たせないことは別である。レビューの「loop 以外は実質 async」は採用しない。

最初の async は bounded worker queue による実装でもよい。queue への受理は bounded な処理とし、待機可能な同期 API を別 wrapper とする。BOT 自体の command は直列でも、その待ち時間に別 device や別処理を進められる。I/O 完了、取消し、device detach の参照契約を §4 に従って先に検証する。

先読みは open file description ごとの連続アクセス検出から始める。要求範囲の read 合成と、要求外を先に読む機能を分ける。window は EOF・メモリ予算・queue 枠で制限し、seek/pressure で縮め、需要 I/O を優先する。古い content generation の完了は採用しない。先読みだけの失敗を、既に成功した要求範囲の read エラーにはしない。

SG がまとめるのは**RAM 上で非連続な buffer**である。通常の一つの read/write command は連続 LBA を扱うので、disk 上の断片を SG だけで一 command にできない。初期 syncer は bounded bounce への copy で十分動作し、SG を write-back 開始の必須条件にはしない。

SG 導入時は CPU physical address をそのまま device DMA address と見なさず、DMA map/unmap と device の alignment、境界、segment/TRB 数を検証する。pin だけでは DMA 中の内容の書換えを防げないため、writeback snapshot、書込み排他または COW を組み合わせる。timeout 後も DMA 停止を証明できなければ page と mapping を隔離する。read page は完了前に valid としない。xHCI の memcpy 削減は、この契約を守った後に測る。

exec は初期に pool copy を改善し、後に page cache からの共有へ進む。単に `MAP_PRIVATE` とするだけでは、実行中に backing file が書き換わった場合の snapshot を保証できない。現行 content lease の意味を引き継ぐ実行中 write 制約か、内容世代ごとの snapshot を設計する。ELF の非整列 segment、末尾 zero、BSS、権限、interpreter まで扱ってから共有を有効にする。

user page 直接 I/O、NVMe queue depth、UAS は別の計測段階とする。user page の pin は DMA mapping や cache coherence の代わりにはならず、unmap/truncate/COW、32-bit address 制約、高位物理ページへの kernel mapping も前提になる。

## 10. 実施順と、それぞれの到達点

以下は設計上の段階名であり、登録済み WS/Phase/Queue ID ではない。実装時に既存 M/W/P と照合して有限の Queue に分ける。

| 段階 | 内容とレビュー案との対応 | 主な依存・完了時の姿 |
| --- | --- | --- |
| A0 | baseline、各層 counter、所有権・順序契約 | 現行値と予測を区別。new/append/small write も測定対象にする。 |
| A1 | 大小 pool、exec chunk、allocator の初段 | A0。毎回の大きな物理確保を除き、枯渇時にも前進する。 |
| A2 | cache run と UFS run | A0。まず cache、次に UFS。連続要求の分割を減らす。 |
| A3 | USB core/HCD reservation と通常 64 KiB | A0。A1/A2 と独立開発でき、統合後に端から端まで測る。 |
| M | allocation adapter → batching、CG/indirect、FAT clean cache/cursor → 同期 metadata batch、flush 証明（M0/M0'/M1/M2） | A2。新規作成・追記を改善し、永続化境界を明示する。 |
| C | file object の保持寿命、read cache、clean reclaim、共通予算（P0 と W2 の clean 部分） | A1/A2 と ownership 契約。M と独立部分を進められる。 |
| D | dirty/error 世代、内部 drain の伝播、同期 adapter（W0） | M/C の所有者が確定。挙動は write-through のまま検証する。 |
| W-data | 既存 data の write-back、syncer、credit/throttle（W1/W2） | D。通常 write と fsync の責任を分け、小書込みをまとめる。 |
| W-meta | 統合 UFS、journal/replay、metadata write-back（W3/W4） | M/D/W-data と WS024 の format/recovery 契約。限定有効化から既定化へ。 |
| R | 実 async submission と先読み（S0 の async/P1） | C と §4 の lifetime。W-meta 待ちは不要。同期 worker から device 別に進む。 |
| E | cache からの exec と snapshot 共有（P2） | C、実行中の内容変更規則。R は性能上の連携であり correctness の前提ではない。 |
| O | vmap/SG、NVMe 多重化、必要なら直接 I/O/UAS（S0 の SG/S1/S2/S3/P3） | 個別の実機測定と DMA/VM 契約。前段の完了を待たせない。 |
| H | 媒体状態機械、実機 IMOD | 媒体修正は独立。IMOD は A3 統合後の実機測定で判断する。 |

初回の Queue 候補は A0〜A3 のうち時間枠内で検証できる範囲とする。M をその直後の優先課題に置く。C の寿命設計と D の error/ownership 設計は早期にレビューし、write-back 有効化後に所有者を作り直すことを避ける。

スタブ段階だけの完了で後段全体を「実装済み」にしない。各段階の記録には、利用可能な機能、実効 policy、未実装部分、次の有効化条件を残す。並行作業は同じ master/queue を競合更新せず、統合と共有環境の build/runtime test を直列化する。

## 11. F3: USB reset と媒体変更の判断

root が自分の reset 後に回復できない経路は解消すべきである。ただし `06/28` を自前 reset 直後なら一般に retry 可能とする根拠は足りない。

T10 の ASC/ASCQ では `28/00` は媒体変更の可能性、`29/00` は reset/power-on、`2A/01` は mode parameter 変更、`2A/09` は capacity 変更として区別される。前案の `06/2A` 一括分類も狭め、確認した ASCQ ごとに扱う。[T10 ASC/ASCQ 一覧](https://www.t10.org/lists/asc-num.htm)

2026-09-07 に確認した Linux の `scsi_io_completion_action()` も、UNIT ATTENTION を removable device なら changed として失敗させ、非 removable では retry とする分岐を持つ。「usb-storage が 28 と 29 を同列に扱うから」という説明は、この差を省略しており、そのまま設計根拠にはしない。[Linux scsi_lib.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/scsi/scsi_lib.c)

USB bridge/reader の serial と capacity は別媒体でも一致し得る。reset 直後という時刻条件を追加しても、同じ medium の証明にはならない。一般処理では `28/00` を revalidate、成功した自前 reset に対応付けられる `29/00` は一回の retry、mode change は再設定、capacity change は世代変更として扱う。その他の UA は ASCQ に応じて bounded な分類・再取得を行い、未分類を無条件 retry にしない。

reset 由来の `28/00` を出す実機が再現できた場合は、VID/PID・firmware・媒体の着脱構造・reset の範囲を記録し、device 固有 quirk の可否を検討する。INQUIRY/capacity の一致だけで quirk を有効化しない。

同一媒体の回復では、上位へ最終 EIO を返して filesystem が書込み停止になった後に driver だけ ONLINE に戻す構造を避ける。現在の request を保留した bounded recovery/control path として扱い、worker が自分の保留 BIO の drain を待つ循環を作らない。失敗時には最終 error と state を一致させる。

実媒体変更を検出した場合は、driver submission だけでなく旧世代の cache hit、mount/partition/claim も admission を止める。旧 mount を新媒体へ接続したまま復帰させない。これを単なる性能用 latch clear と混ぜない。

## 12. 受け入れの観点と性能評価

実装 Queue ごとに次のうち関係する項目を具体的 fixture にする。今回は実行していない。

| 分類 | 必須の確認 |
| --- | --- |
| 粒度 | warm/cold、64 KiB 連続 read/overwrite、新規作成、append、部分 block、hole、indirect、断片化。各層の run/command を分けて数える。 |
| pool | 大小両 pool の枯渇、低 RAM、並行 I/O、exec と通常 I/O の競合、partial copy/EOF/error、返却漏れ。 |
| cache 再入 | 競合 line、loop 二段、準備中の reclaim、短い run への fallback、途中失敗と同時 read/write。 |
| allocation | batch の各永続化境界で失敗、CG 跨ぎ、ENOSPC、部分成功、abort 失敗、pointer 不確定時の再割当て禁止。 |
| metadata cache | 別 open の延長/truncate、free/reuse、rollback、CG 切替え、FAT mirror 不一致、媒体世代の変更。 |
| flush | write 無しの反復、flush 中の新規 write、部分 write、flush 失敗、二つの fsync、overlay journal の別 commit 境界。 |
| error 通知 | 二つの独立 open、dup の共有状態、fsync と新規 error の競合、再送失敗、close 後にも dirty owner が残る場合。 |
| write-back | 同じ block への反復と異なる block への小書込み、書戻し中の再 dirty、複数 inode/device、fsync/unmount/disable。 |
| メモリ不足 | clean 回収、dirty high/low、worker の最低資源、停止 device、signal/timeout、lock を保持した待機がないこと。 |
| page cache | 最終 close 後の再 open、MAP_SHARED、copy-up、truncate/EOF、reclaim、unmount。予算内の warm working set では対象 file data の device read がゼロ。 |
| async/DMA | submit 前後の即時完了、拒否、部分完了、取消し成功/失敗、detach、世代変更、遅い完了、隔離領域の再利用禁止。 |
| exec/先読み | 非整列 ELF/BSS/interpreter、exec 中の file 更新、seek、EOF、先読み失敗、pressure、古い世代の完了破棄。 |
| crash | volatile device cache の保持/消失、write の reorder、契約上許される torn write、flush 前後の crash と replay。成功済み fsync の対象は保持される。 |
| USB 実機 | 通常 64 KiB と 8 KiB reserve、複数 device、BOT reset/UA、mode/capacity change、WLAN/HID 同時負荷、IMOD ごとの差。 |

QEMU の process kill だけでは、device volatile cache の消失や write reorder を十分に再現できない。記録した永続化境界に基づく fault model を用い、各段階の crash 保証と必要な recovery を判定する。ランダム kill は補助試験にする。元 image を維持し、破壊試験は disposable copy を使う。

性能は data、allocation metadata、journal、flush、retry、copy、物理確保を分け、warm-up 後の分布と p50/p95/p99、CPU time、IRQ-off、resident/dirty/quarantined bytes を記録する。単発の guest 10 ms 刻み測定から実用速度を断定しない。

連続配置・必要な cache/reservation が利用可能・エラーなしという条件を固定した **64 KiB overwrite の data command 一回**は初期の到達条件にできる。一方、新規作成の総 command 数、二回目 exec 全体の device read ゼロ、すべての fsync が物理 flush 一回という値は、metadata や永続化境界を含むため無条件には要求しない。

write-back の性能試験は「多数の write + 最後に fsync」と「毎回 write + fsync」を分ける。後者の明示的な永続化境界を write-back で消すことはできない。設定保存の temp file fsync、rename、directory fsync も、それぞれが担う保証を残したまま重複分を探す。

各実装段階では関連する q086 の 50 シナリオ、q087 syscall 境界、FS/VM/USB の既存回帰を選択し、USB-root 統合時に net wifi の受け入れへの影響も確認する。amd64/pcat/pc98 の supported build は段階の変更範囲に応じて実施し、aggregate `make check` は使わない。

## 13. このレビューで確認したい方針

提案する到達目標は「64 KiB 転送の貫通」から「新規作成・追記・小書込み・反復読込みまで一貫した I/O と cache」に広げる。そのための段階は本書で定義し、初期実装は bounded な write-through 改善から始める。

metadata batching を早く入れ、page cache の寿命・dirty owner・エラー通知・永続化境界を先に固めてから write-back を有効化する。長期の metadata recovery は単一 64-bit UFS へつなぐ。これが、今回のレビューと段階導入の提案を取り込みつつ、後段で所有権と順序を作り直さずに進める方針である。
