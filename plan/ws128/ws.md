<!-- awesome-plan project=zedbsd record=ws128 -->

# WS128: 標準アプリ全般のベータ1 のブラッシュアップ

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p001〜p006・p009〜p011 cleared。p008 は T2-025 PASS 8/8 で Q1 の判定（FreeBSD は 10/13 以降）、p012（montage-4 の icon）は UAT の image で確認、p007 は実機の UAT）
Primary Milestone: MG006
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1、2026-10-17）
Queue: none
Resume point（2026-10-02 計画）: **p001（棚卸し・回帰の取り直し・候補、source は変えない）** と、明白な欠落の直し **p002（Notes の Open）・p003（Text Editor の Replace）** はすぐ投入できる（互いに file が別で並列可）。p004〜p006 は p001 のユーザーの選択待ち。既存の WS（WS079・WS090・WS100・WS102）の残りは下の照合表のとおり、その WS で実行する。
2026-10-02 user: スクロールバーは macOS 風の重ね表示（ws127 で libkeiui の共通部品にする）。各 app もこの部品に揃える。
<!-- awesome-plan-current:end -->

## 目標（2026-10-02 ユーザー（ベータ1、リリース目標 10/17））

「標準アプリ：まんべんなくブラッシュアップします。」

- Text Editor・Image Viewer・Notes・PDF Viewer・Terminal・音量・スクリーンキーボードなどを、まんべんなくベータ1 の品質に仕上げる（Files は WS127、Settings は WS089、compositor と App Home は WS099、desktop の icon は WS094、音楽アプリは WS120）。

## ベータ1 の到達目標と受け入れ（測れる形）

| # | 受け入れ | 測り方 | Phase |
| --- | --- | --- | --- |
| A1 | 最終の image で各アプリの既存の回帰が全て PASS: Text Editor（`plan/tools/textedit/host-core.sh`）、Image Viewer（`plan/tools/imageview/run-host.sh`・`imageview-guest.sh`）、Notes・PDF Viewer（`plan/ws079/tests/demo-s8-s9.sh` と host）、音量（`volume-p004.sh`・`volume-p005.sh`）、スクリーンキーボード（WS102 の guest の手順）、Terminal（p001 で特定する既存の試験） | QEMU の Venus と host | p001（基準）、p008 |
| A2 | p001 の通しで見つけた不具合のうち重い・中が 0（直したか、ticket でユーザーが非阻害を決めた） | p001 の不具合の表 | p002〜p006 か新しい Phase |
| A3 | UI に出ている未完成の機能が 0（例: Notes の「Opening from Notes is not available yet」） | p001 の照合（`not available`・`not yet` などの文言の grep と menu の通し） | p002 ほか |
| A4 | p001 でユーザーが選んだ改善の項目が全て実装・試験 PASS | 各 Phase | p003〜p006 |
| A5 | 5330 の実機で標準アプリの通し（開く・編集・保存・閉じる）がユーザーの目視で通る。WS079 の S8・S9、WS100 の A7 も同じ回に | ユーザーの目視と SSH の log | p007 |
| A6 | 変えた source の全文規約、boot test | — | p008 |

## Phase

| Phase | 目的 | Status | 依存 | 目安 |
| --- | --- | --- | --- | --- |
| [ws128-p001](phase001/phase.md) | 棚卸し: アプリごとの回帰の取り直し、menu の全項目の通しと不具合の表、未完成の UI の一覧、改善の候補（価値・規模・危険・依存）。既存 WS の残りとの照合の確認。最後にユーザーが選ぶ | cleared（2026-10-03 q623-i01、[requirements.md](requirements.md)） | — | 3h |
| [ws128-p002](phase002/phase.md) | Notes の Open（file chooser で Notes の PDF・他の PDF を開く）と Save As | cleared（2026-10-03 q621-i01、QEMU の Venus） | —（p001 と並列可） | 2h |
| [ws128-p003](phase003/phase.md) | Text Editor の Find & Replace（Replace・Replace All、undo で戻せる）と Open Recent | cleared（2026-10-03 q621-i01、QEMU の Venus と host） | —（p001 と並列可） | 2h |
| [ws128-p004](phase004/phase.md) | PDF Viewer の文字の検索と選択・copy（libpdf に文字の抽出（ToUnicode）が無いので規模が大きい） | cleared（2026-10-07、T1-288） | p001、libpdf（`userland/base/libpdf`） | 4h 以上 |
| [ws128-p005](phase005/phase.md) | Image Viewer: Move to Trash・Open With・slideshow（画像の copy は F-074 へ移管、2026-10-04 Q1） | cleared（q666、T1-078） | p001 | 2h |
| [ws128-p006](phase006/phase.md) | Terminal: scrollback の検索・色の theme と font の大きさの保存 | cleared（q666、T2-025） | p001 | 2h |
| [ws128-p007](phase007/phase.md) | 5330 の実機で標準アプリの通し（ユーザーの目視、WS079 S8/S9・WS100 A7 と同じ回） | planning（p002〜p006 の選んだ物の後、実機とユーザーの時間） | 実装の Phase | agent 1h + ユーザー 30 分 |
| [ws128-p009](phase009/phase.md) | Terminal: 「CJK Ambiguous Width を全角で扱う」を menu で即座に切り替える（2026-10-03 user の指示） | cleared（2026-10-03 Q1、T1 の terminal-p009-guest・menu-p003） | —（p006・WS131 p018 と同時に流さない） | 2〜3h |
| [ws128-p010](phase010/phase.md) | Terminal の右端の wrap を xterm と同じ保留にする（BUG-150、Emacs の画面が 1 行ずれる） | cleared（2026-10-03 q636-i01、FreeBSD 実機と host、Q1 の照合待ち） | — | 1〜3h |
| [ws128-p011](phase011/phase.md) | BUG-155: Terminal で IME の日本語を入力する（text-input-v3、組み立て中の文字を cursor に描く） | cleared（2026-10-03 Q1、T1-012） | — | 2h |
| [ws128-p012](phase012/phase.md) | App Home の app の icon をデザインした物に差し替える（2026-10-05 夜 ユーザー） | planning（デザインの方向はユーザーに確かめる） | | |
| [ws128-p008](phase008/phase.md) | 全文規約と回帰（WS の最後） | in-progress（q667、P2。規約・build・host 済み、QEMU 回帰・FreeBSD は T1 待ち、実機 p007 待ち） | 実装の Phase | 2h |

## 既存の WS の残りとの照合（2026-10-02、その WS で実行し WS128 では重ねない）

| WS | 残り | ベータ1 の扱い（計画エージェントの案、Q1・ユーザーが決める） |
| --- | --- | --- |
| [WS079](../ws079/ws.md) Notes・PDF Viewer | S8・S9 の 5330（mouse）と Windows の QEMU（touch）のユーザーの確かめ（`plan/ws079/demo-s8-s9-manual.md`）、L3 の実機のペン | S8・S9 の 5330 は WS128 p007 と同じ回に。L3 の実機のペンは後 |
| [WS090](../ws090/ws.md) libkeiui | p015（案、Terminal・Notes の scroll を `kui_scroll` へ）、p009・p010（Files）、p007（Settings）、p012（規約） | **ベータ1 の後を推奨**（Files・Settings・Terminal の大きな移行は WS127・WS089・p006 と同じ file を変える）。ユーザーの判断 |
| [WS100](../ws100/ws.md) 音量 | p006（案、5330 の HDA で鳴るか、A7） | WS128 p007 と同じ回にユーザーが聞く |
| [WS102](../ws102/ws.md) スクリーンキーボード | p022（絵文字の面、planned）、p010・p011（速さ、優先を下げた）、p012（IME と組んだ日本語、WS095）、p013（Windows の QEMU の touch）、p014（規約） | p022 はベータ1 に入れる候補（compositor の `keyboard.c`、WS099 の shell.c 系と file は別）。p012 は WS095 の進みしだい。p010・p011 は後。p014 は WS102 を締めるなら要る |
| WS091（Image Viewer）・WS092（Text Editor）・WS093 | completed | 新しい改善は WS128 で（p003・p005） |

## source の衝突（並列の注意）

- p002（`userland/desktop/notes/`）・p003（`textedit/`）・p005（`imageview/`）・p006（`terminal/`）は互いに file が別で並列可。
- p002 は libkeiui の `kui_file_chooser` を使う（libkeiui は変えない見込み。変えるなら WS090 の担当と調整）。Notes の窓は WS090 p011 で `kui_window` に移っている。
- p004 は `userland/base/libpdf`（WS079 の source）を変える → WS127 p004（PDF の thumbnail、libpdf を読む）と直列。
- p005 は `picture/` を変えるなら WS094 p014・WS127 p004 と直列。`imageview/image.c` は WS094 p014 と直列。
- p006 は WS090 p015（Terminal の scroll）と同じ `terminal/` を変える → 同時に流さない。
- 5330 の実機の回（p007）は WS099 p012・WS094 p012・WS089 p011・WS127 p007 と同じ image・同じユーザーの時間にまとめられる。

## 未決の判断（ユーザー）

1. p001 の候補からベータ1 に入れる項目（p004〜p006 の採否）。
2. WS090 の libkeiui への移行（p015・p009・p010・p007）をベータ1 の前に行うか（計画エージェントの案: 後）。
3. WS102 p022（絵文字の面）をベータ1 に入れるか。

## Event

2026-10-02 / ws128-beta1-plan: fg019 の計画エージェントが到達目標 A1〜A6、p001〜p008、既存 WS の残りとの照合を作成。p001〜p003 は planned、他は planning。Queue は未投入。

2026-10-04 Q1（user「任せます」で判断を委ねられた）: ベータ1 に向け、標準 app の作業を WS131 の app の移行（p012 以降）より先にする。p001 の候補から Q1 が採る: p005（Image Viewer: Move to Trash・Open With・slideshow・画像の copy）、p006（Terminal: scrollback の検索・色と font の設定の保存）。p004（PDF の文字の検索と選択、libpdf の文字の抽出が要り規模が大きい）はベータ1 の後へ。p007 は実機。
