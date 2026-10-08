<!-- awesome-plan project=zedbsd record=ws094 -->

# WS094: desktop の file の icon

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p007（規約と回帰、規約はベータ3）・p009（L3 の数値、uncleared、Q1 の判断）・p012（5330 の実機）が残り。実装の残りは無い）
Primary Milestone: MG006
Related Milestones: MG006
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1、2026-10-17）
Queue: なし（q582 cleared、q588 は p007 の source/host/build の部分だけ cleared、whole p007 uncleared）
Resume point（2026-10-02 ベータ1の計画）: 2026-10-02 user「WS094 は現行設計のまま（Files の `files --desktop` が compositor の desktop surface に描く）実装しきる」。機能（W1〜W4・L1・L2・L4）は済み。残りは **p014（q588 の所有外の規約の指摘を直す、compositor・Files・Image Viewer）→ p012（5330 の実機、ユーザー）→ p007（全 guest 回帰・C9・最後の boot で WS の締め）**。p009 の扱いは p012 の実機の値で決める（超えたら p015）。下の「ベータ1 の到達目標」。
<!-- 旧 resume（2026-10-02 前半）: p011（L4b）はq582でcleared。L3 の計測（p008）は 2026-09-30 に cleared。基準値は (a) 2908 ms（目標 1500）・(b) 1094 ms（2500）・(c) 95 ms（50）。p009 は 2026-09-30 に uncleared: (a) 1954 ms、(c) 90 ms、(b) 1344。p013 cleared。 -->
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標（2026-09-29 ユーザー）

「デスクトップにファイルアイコンの表示。」

- `~/Desktop` の file と folder を desktop（壁紙の上）に icon と名前で並べる。double click で開く（WS093 の対応）、選択・移動（配置の保存）・
  右 click の menu・drag and drop（Files との間）、file の増減の追従。compositor（WS035）と Files（WS071）のどちらが描くかは p001 で決める。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [ws094-p001](phase001/phase.md) | 設計（[design.md](design.md)、J1〜J7 は既定: Files が背景の層の client として描く） | cleared（2026-09-29） | — |
| [ws094-p002](phase002/phase.md) | compositor: `keiland_desktop_v1`（role・token・configure）、重ね順・合成・入力・focus・popup・DnD の対象、`--session` での起動と起こし直し、probe | cleared（2026-09-30） | p001、main の許可（WS035 の source） |
| [ws094-p003](phase003/phase.md) | libkeiland の client の API と Files の `--desktop` の骨組み（`~/Desktop` の icon を右上から描く、監視） | cleared（2026-09-30） | p002、main の許可（libkeiland） |
| [ws094-p004](phase004/phase.md) | 選択・開く（WS093）・keyboard・配置の保存と Clean Up | cleared（2026-09-30、2 回目: 保存した場所への配置 PASS、band の撮り直し、回帰（probe・files-open・host）と boot test PASS。probe の試験の順の誤りを直した） | p003 |
| [ws094-p005](phase005/phase.md) | context menu（項目・空いた所）・名前の変更・Trash・Copy・Paste・New Folder・Show in Files | cleared（2026-09-30: menu の手順 PASS、p003・p004 の手順・probe・files-open・host・C9（10 本）・boot test PASS。compositor の menu-shell.c の desktop の menu の閉じ方を直した（main 了解）） | p004 |
| [ws094-p006](phase006/phase.md) | drag（desktop の中・folder へ・Files の窓との DnD）と touch | cleared（2026-09-30: drag・touch の手順 PASS、p003〜p005 の手順・files-open・host・boot test PASS。probe の restart は試験の probe が時々終わらない（下の注）） | p005 |
| [ws094-p008](phase008/phase.md) | L3a: 100 項目の計測の道具と基準値（計測だけ） | cleared（2026-09-30、QEMU。(a) 2908 ms・(b) 1094 ms・(c) 95 ms・SLOW-FRAME 0） | p006 |
| [ws094-p009](phase009/phase.md) | L3b: 100 項目で L3 の数値目標に入れる | uncleared（2026-09-30、QEMU。(a) 1954・(c) 90 ms で超過。残りは zdesktop の import と Venus の 10 ms、main の判断待ち） | p008 |
| [ws094-p013](phase013/phase.md) | L3c: jpg と gif の thumbnail（`fm_image_load` が PPM・PGM・PNG だけ。Image Viewer と同じ libjpeg-compat（EXIF の向き）・libgif-compat（最初の frame）で読む）。100 項目の画像に jpg・gif を混ぜて thumbnail が出る | cleared（2026-09-30、host と QEMU。decoder を `userland/desktop/picture/` に共有） | p009 |
| [ws094-p010](phase010/phase.md) | L4a: 長い名前（2 行、中を省く）と画面の大きさの変更 | cleared（2026-09-30: 統合の試験 demo-s8-s9.sh で合格（ユーザーの指示）、Terminal と p088 は未切り分け、実機は未実施） | p006 |
| [ws094-p011](phase011/phase.md) | L4b: 置き場の溢れ（grid より多い項目）と、無い名前の保存の行の掃除 | cleared（2026-10-02、q582-i01） | p010 |
| [ws094-p012](phase012/phase.md) | L5: 実機（5330）での L1〜L3 の確認 | planned（agent 1.5h + ユーザー 20 分。WS099 p012 と同じ回にまとめられる） | p009（QEMU の値まで）、実機とユーザーの時間 |
| [ws094-p014](phase014/phase.md) | q588 の所有外の規約の指摘（compositor の display.c・menu.c・desktop.c、Files の thumb.c・ui-context.c、Image Viewer の image.c）を直す | cleared（2026-10-03 Q1） | q588 の review、WS099 p020・p021 と compositor の file を重ねない |
| ws094-p015（条件つき） | p012 で (a)(c) が目標を超えたとき: Files の frame の Vulkan の呼び出しの削減（image ごとの command buffer の事前の記録、phase009「残り」の 3）。触る file: `files/present.c`・`main.c` | planning（p012 の実機の値と main の判断が要る） | p012 |
| [ws094-p007](phase007/phase.md) | 全文の規約と回帰（WS の最後。選んだ段の Phase の後） | uncleared（q588 source/host/build部分はcleared、所有外規約/全guest/実機/最終boot未達）。次の attempt は p014・p012 の後（3h） | p014・p012（と p015 を行うならその後） |

## ベータ1 の到達目標（2026-10-02 計画、fg019）

現行の設計（J1: Files が desktop の層の client）のまま、WS の完了まで持っていく。新しい機能は足さない（溢れた icon は「今のまま」）。

| # | 受け入れ | 測り方 | Phase |
| --- | --- | --- | --- |
| D-B1 | q588 の review の所有外の指摘（WS094 が持ち込んだ規約の違反）が 0。既存の例外（main.c 2・picture.c の setjmp 1）は記録 | `style-check.py`・`style-extra.py`・全文の目視 | p014 |
| D-B2 | 5330 で L1 の操作が働き、L3 の (a) ≤ 1500 ms・(c) ≤ 50 ms・SLOW-FRAME 0（3 回の中央値） | ユーザーの操作と SSH の log | p012（超えたら p015） |
| D-B3 | `files-desktop-guest.sh` の全手順・desktop-guest（probe）2 回・p010・Files の 14 本・C9・最後の boot test が同じ image で PASS | QEMU の Venus | p007 |
| — | 完了の処理（試験を `plan/tools/files/` へ、Phase の directory の削除）は Q1。WS094 の完了は WS090 p009（Files の libkeiui への移行）の前提 | — | — |

source の衝突: p014 は compositor の `display.c`・`menu.c`・`desktop.c` を触る → WS099 p020・p021、WS113 p004 以降と直列。Files の `thumb.c`・`ui-context.c` → WS127 の Files の Phase と直列。
`imageview/image.c` → WS128 の Image Viewer の Phase と直列。p007 は source を変えない見込み（回帰で直しが要れば所有を Q1 に確かめる）。p012 は 5330 と i915 の lock を使う。

注（p006）: `desktop-guest.sh` の restart の手順で、試験用の probe（`--timeout-s=3`）が時々終わらず、起動の上限の行が出ないことがある（p002 で 1 回観察、
p006 で 2 回再現、別の 2 回は再現せず）。compositor は role と ack を正しく扱っている。p006 は Files だけを変えており、compositor は p005 と同じ。
probe の側（または libc・kernel の poll や time）の原因は未調査。main に報告済み（bug の起票は main の判断）。

## 段（L1〜L5、2026-09-30 main の方針「広く浅く」）

各段は「まず動く」から磨き込みへ進む。1 つの段を小さな Phase に分け、数値目標と測り方を持つ。確かめるのは QEMU の Venus（L5 だけ実機）。

| 段 | 内容 | 数値目標 | 測り方 | Phase | 状態 |
| --- | --- | --- | --- | --- | --- |
| L1 | icon の表示・開く・選択・menu・名前の変更・Trash・Copy・Paste・New Folder・Clean Up | 手順 show・input・saved・menu が PASS（誤りの log 0） | `files-desktop-guest.sh install show watch input saved menu` | p002〜p005 | 済み |
| L2 | drag（desktop の中・folder へ）、Files の窓との DnD（両方向）、touch（double tap・long press・long press の後の drag） | 手順 drag・touch が PASS | `files-desktop-guest.sh install show drag`（base image）、`… install show touch`（pen image） | p006 | 済み |
| L3 | 項目が多いときの速さ | `~/Desktop` に 100 項目（画像 20 を含む）で、(a) `files --desktop` の起動から `DESKTOP ready` まで 1500 ms 以内、(b) file を 1 つ足してから表示まで 2500 ms 以内（今の監視は 2 秒ごと）、(c) click から選択の frame まで 50 ms 以内（`SLOW-FRAME` の行が 0） | p008 で作る計測の手順（Files の log の時刻と `SLOW-FRAME`、3 回の中央値） | p008（計測）・p009（直し） | 一部（p009、QEMU: (b) と SLOW-FRAME は以内、(a) 1954・(c) 90 ms は超過） |
| L4 | 見た目と端の場合 | (a) 40 文字の名前が 2 行に収まり中を省く（画面で確認）、(b) 1280x800 から 1920x1280 に変えても保存の場所を保ち、外れた項目が空いた cell に入る（log）、(c) grid の cell より多い項目は描かずに数を log、保存の行の無い名前は次の保存で消える | host の試験と guest の画面・log | p010・p011 | 未着手 |
| L5 | 実機 | 実機（5330）で L1 の手順と L3 の (a)(c) が QEMU の目標以内 | 実機の手順（p012 で決める） | p012 | 未着手（実機が要る） |

WS の完了の条件（design §7 の受け入れ）は L1・L2 で満たした。L3 以降は磨き込みで、main が順番と止め時を決める。p007（全文の規約と回帰）は、選んだ最後の段の後に行う。

### 進め方とハーネス（2026-09-30 Q1 の補足）

- **L3 の作業像**: まず計測だけの Phase（p008）で、100 項目の `~/Desktop` を作る script（`plan/tools/files/make-home.sh` を広げ、画像 20 を含む）と、
  `files --desktop` の log の時刻（`DESKTOP ready`、file の追加から表示、`SLOW-FRAME`）を集める script を作り、3 つの数値を出す。直すのは p009 で、
  一番遠い目標から 1 つずつ（例: thumbnail の作成を後回しにして ready を早める）。
- **ハーネス**: 既存の `plan/ws094/tests/files-desktop-guest.sh` に手順 `perf100` を足す。判定は log の時刻だけで行い、画面は確かめの 1 枚。
- L4・L5 は、他の WS が L2・L3 にそろってから。

### ユーザーの判断（2026-09-30、溢れた icon）

- 1280x800 の画面に 100 項目を置くと 13 列 × 7 行の 91 個だけが出る件は「今のまま」（macOS や Windows と同じく、溢れた分は出さない）。L3 の計測は 91 個の表示で行う。

### Q1 の判断（2026-09-30、p009 の再開の条件）

- p009 の残り（(a) 1954 ms・(c) 90 ms）の大部分は Files の外（zdesktop の buffer の import と、QEMU の Venus の同期の呼び出しの約 10 ms の刻み）にある。
- **L3 の (a)(c) の合否は実機（5330、L5 の p012）で判定する。** QEMU の値は参考として記録し、合否に使わない（Venus の 10 ms の刻みは QEMU に固有の見込みのため）。
- zdesktop の import の短縮（`import_layout` の submit と `vkQueueWaitIdle` を次の合成の barrier にまとめる）は、全ての app の最初の frame に効くので、compositor の Phase として WS099 の p016 に移す。
- Venus の 10 ms の調査は Future Work（F-064）に。Files の frame ごとの command buffer の事前の記録は、実機の値を見てから。
- 広く浅くの方針で、WS094 は L3 をここで区切る。次は p013（jpg・gif の thumbnail、デモの写真が汎用の icon になるため）。

## q582 / B2 checkpoint（2026-10-02）

p011を有限Queueでcleared。hidden/prune・失敗listingからの復帰のhost試験、warning0 target build、prune/L1/drag/menu guest回帰、最終staged imageのboot-testがPASS。fresh full-image生成はtoolchain制限のため未実施、B main指定fixtureを独立コピーして現行desktop binary/libraryを導入した。WSはincompleteのまま、p007/p012とp009の判定はこのQueueに含めない。[詳細](phase011/q582-result.md)。

2026-10-02 / b2-q588-partial-dispatch: p011/q582 clear後、[p007のsource/host/build部分](phase007/q588-approved-scope.md)を同じB2へ投入。全WS sourceはinventory/review、wayland等の所有外は編集せずfindingsを渡す。実機p012と最終guest/C9/bootを受け入れから除かず、WS incompleteを保持。

2026-10-02 / b2-q588-terminal: [p007部分結果](phase007/q588-result.md)をB main9323725bへ統合/ACK、source conformance partial item cleared。whole p007は所有外規約/実機/全guest/C9/最終boot未達でuncleared、WS incompleteを維持。19原ログhashを照合、同時進行B1 source後のinventory再照合は最終conformanceで必要。user指示でB2終了、背景q593は未実装/再開資料保存。

2026-10-02 / ws094-beta1-plan: fg019 の計画エージェントが「現行設計のまま実装しきる」（user）をベータ1 の到達目標 D-B1〜D-B3 にし、p014（所有外の規約の指摘）と条件つきの p015 を追加。順は p014 → p012 → p007。既存の Phase の結果は不変。Queue は未投入。
