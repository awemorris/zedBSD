<!-- awesome-plan project=zedbsd record=ws177-p008 -->

# ws177-p008: Files の Recents の仕上げ（案 C）

Parent: [WS177](../ws.md)
Status: test-wait（T1-411）（2026-10-08 P1 q884 の 5: 実装・host PASS・build warning 0、main に統合）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q884 の 5（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 37・38・39（q824 ws148-p002）、[案](../phasing-20261008.md) の C

## 設計と変更（2026-10-08 P1）

| 行 | 形 |
| --- | --- |
| 37 Keep recent items が off の Recents | Recents を読む時に `kl_recent_keep` も読み（`fm_app.recents_off`、log `ZFILES RECENTS kept=N`）、空で止められていれば「Recent items are not kept」と「Turn on Keep recent items in Settings > Storage.」を出す（`grid_message` に 2 行目の hint を足した） |
| 38 Clear Recents の確かめ | `fm_action_clear_recents` は問いを出す（`FM_DIALOG_CLEAR_RECENTS`: 「Clear Recents?」「The N items leave the recent list; the files stay.」、Cancel・Clear）。Enter・Clear で空にし（`actions_clear_recents_now`、従来の log `ZFILES RECENTS clear error=`）、Esc・Cancel では何もしない |
| 39 他の app の「最近の file」の menu | libkeiland に `kl_recent_stamp(&stamp)`（**KL_VERSION 66**）: 一覧の file の inode・大きさ・時刻と止められているかから作る値で、変更のたびに変わる（変更は新しい file を rename するので inode が変わる）。Text Editor（Open Recent を持つ唯一の app）は一覧を読む時に stamp を覚え、窓が keyboard を得た時に stamp が違えば読み直す（log `TEXTEDIT RECENT changed: read again`）。他の app（pdfviewer・notes・imageview）は add だけで menu を持たない |

- 変更: `files/files.h`・`actions.c`・`ui-grid.c`・`ui-search.c`・`ui-overlay.c`、`textedit/textedit.h`・`main.c`、`libkeiland/recent.c`・`exports.map`、`include/keiland/keiland.h`。
- 試験: `plan/ws148/tests/run-host-files-recents.sh`（WS148 は未完了、問いと Esc で残る・Enter で空・止めた一覧の表示を足した）、`plan/ws177/tests/host-recent-stamp.{c,sh}`、QEMU `plan/ws177/tests/recents-p008.sh`・`config-amd64-p008.mk`。

## 確認（host・build、2026-10-08）

- `sh plan/ws148/tests/run-host-files-recents.sh` → PASS（9 の ok、問いの絵と止めた一覧の絵を確認）。`sh plan/ws177/tests/host-recent-stamp.sh` → PASS（ASan/UBSan）。
- build（warning 0）: `make BUILD=build/p1-d ZEDBSD_CONFIG=plan/ws089/tests/config-amd64-settings.mk build/p1-d/dynamic/libkeiland.so build/p1-d/bin/files build/p1-d/bin/textedit build/p1-d/bin/settings`、`keiland-linux.mk all`（warning 0）。exports.py --check OK。style-check: 変えた file は増えない。

## 未実施

- QEMU（T1）: `recents-p008.sh`（他の program が一覧を空にした後、Text Editor の窓の focus で読み直す）。

## Event

2026-10-08 / q884-i05（P1）: 実装と host・build。
