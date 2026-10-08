<!-- awesome-plan project=zedbsd record=ws177-p012 -->

# ws177-p012: Notes の文字の box（案 K、Notes の側）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 Q1 判定、T1-415。指は実機の UAT、回転（11）は backlog）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q887 の 2（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 7・8・10〜14（WS175 ws175-p008）、[案](../phasing-20261008.md) の K。9（libkeiland の field の clipboard・語・undo）は [ws177-p013](../phase013/phase.md)

## 設計と変更（2026-10-08 P1）

| 行 | 形 |
| --- | --- |
| 7 zoom・scroll・窓の大きさ | box の場所を page の点で覚え（編集する物の四隅 `box_quad`、新しい文字は `box_x`・`box_y`・`box_width`、高さ `box_height`）、`app_box_rect` が今の view から窓の rectangle を出す。`app_box_follow` は描くたびと box の frame ごとに場所を比べ、変われば box と window の rectangle を移して box を描き直す（log `NOTES TEXT box moved rect=`）。指の zoom・scroll、F11、窓の大きさの変更のどれでも付いて動く |
| 8 指 | `window.c`: box の上に下りた指の入力（DOWN から UP まで、その指の id）は box だけに渡す（kl_ui が caret・選択に使う。頁は動かない）。cancel は両方へ。`touch.c` は頁の tap も queue に入れ（toolbar の印 `tap_toolbar`）、`app_touch` は box が開いている時の頁の tap で box を確定して閉じる |
| 10 Ctrl+S・Ctrl+W | `window.c`: box が開いていても Ctrl+S（Shift でも）と Ctrl+W は box に渡さず Notes の key にする（System Menu がある時はそもそも menu が取る）。`app_action` の SAVE・CLOSE は box を確定してから保存・終了する（従来の道） |
| 11 回転した頁・文字 | 未実装（理由）: kl_field・kl_text_area は横書きの rectangle しか描けず、caret と box を文字の向きに回すには libkeiland の canvas の変換が要る。今は回った行の外接の rectangle の下に横の box を出す（従来どおり）。backlog に残す |
| 12 長い行（512 byte 超） | kl_field に入らない行（511 byte 以上）は数行の box（kl_text_area、8192 byte）で開く。確定の時、行の文字の改行は空白に（行は 1 行） |
| 13 box の font が無い | `box.c` の `box_font`: keiland.ttf が開けなければ keiland-mono.ttf、keiland-fallback-mono.ttf、keiland-fallback.ttf の順に開き、使えた物で box を出す（log `NOTES TEXT box font=PATH error=N`）。全部無い時だけ従来の status |
| 14 置き換えの font が無い時 | code は従来のまま（`app_box_try` が ENOTSUP で「No font for these words is installed」を出し box を残す）。画面の確認は UAT |

- 変更: `userland/desktop/notes/main.c`・`window.c`・`box.c`・`touch.c`・`touch.h`・`app.h`。
- 試験: AAT `tests/scenarios/apps/notes/text-box-follow.md`（新）と `plan/tools/aat/scenarios/helpers_notes_edit.py` の `apps.notes.text-box-follow`（box を開いて F11 → `box moved`、` again` を打って Ctrl+S → 確定と保存、pdftotext に again）。

## 確認（build、2026-10-08）

- build（warning 0）: `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=plan/ws089/tests/config-amd64-settings.mk build/p1-k/bin/notes`、Linux `build/p1-linux/bin/notes`。style-check: 変えた file は 0 件。`check-scenarios.py` PASS。
- Notes の host の試験は無い（窓・描画が要る）。

## 未実施

- QEMU（T1）: AAT `--only 'apps\.notes\.(text-box-follow|pdf-edit-text)'`（後者は回帰）。
- UAT（5330 の touch）: 指の zoom・scroll で box が付いて動く、box の上の tap で caret、外の tap で確定。System Menu の無い compositor での Ctrl+S・Ctrl+W。512 byte を越える行の編集（試料に無い）。14 の status の見え方。
- 11（回転）は未実装。

## Event

2026-10-08 / q887-i02（P1）: 実装と build の確認。

## T1-415 の判定（2026-10-08 Q1）

text-box-follow: box を開き（rect=339,161,255,36）、F11 で `box moved rect=432,175,303,36`、打って Ctrl+S で保存、pdftotext に打った again、Ctrl+W で dirty=0。pdf-edit-text の回帰も fail なし。Q1 が PNG（build/review/t1-415/ の box・fullscreen）で、box が窓でも全画面でも編集する行のすぐ下の同じ相対の位置に付いて動くのを目視。指は AAT に touch の口が無いので 5330 の UAT。cleared。
