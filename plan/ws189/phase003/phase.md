<!-- awesome-plan project=zedbsd record=ws189-p003 -->
# ws189-p003: app — 画像の drag の元と受け、文字の drag の元と受け、desktop の file

Status: test-wait（2026-10-08 q902 P1 の照合: T1-434・437・439 で text-between-windows 1〜4・photo-to-notes 1〜2・apps.browser.image-drag を確認。未: apps.pdfviewer.drag-out・desktop.dnd.photo-to-notes 3〜4・text-between-windows 5〜6）（旧: test-wait（T1-434、2026-10-08 Q1 が依頼。実装・build・host 試験は済み））
Disposition: normal
Parent: [WS189](../ws.md)
Queue: q892（P1、2026-10-08）
Design: [p001](../phase001/phase.md) §4

## 範囲

p001 §4.0〜4.7（共有の PNG、Text Editor、Notes、Photos、PDF Viewer、desktop、Browser と libbrowser の口）。Mail（§4.8）は p004。

## 記録

### 実装（2026-10-08、P1）

- 共有 `userland/desktop/picture/png-write.c`・`.h`（新）: `kl_picture_png`（不透明は RGB、ほかは RGBA に戻す）、`kl_picture_png_rows`（PDF の PNG の行を包む）、
  `kl_picture_fit`（箱の平均で縮める）。libz-compat だけに依る。
- Text Editor（`textedit/app.c`・`main.c`・`draw.c`・`textedit.h`）: 選択の中の左の press が 6 px 動くと `kl_window_drag_text`（COPY）。動かずに離すと click（caret をそこへ）。
  drag の間は `kl_window_accept_drops(0)`、`KL_WINDOW_DRAG_DONE` で TEXT に戻す。受け: `KL_DROP_TEXT`、本文の card の上で copy と答え、挿入点に accent の 2 px の線と glow
  （`draw_drop`、kl_drop_caret と同じ寸法）、drop で 1 回の undo の挿入と選択。dialog の下では受けない。log `TEXTEDIT DND drag start|drop|drag done`。
- Files の desktop（`files/window.c`・`ui-desktop-drag.c`・`dnd.c`・`main.c`・`files.h`・`window.h`）: desktop は URIS・IMAGE・TEXT を受ける（folder の窓は URIS のまま）。
  file 名の無い画像・文字は `drop_content`: desktop の folder の cell だけを target（folder の item は不可）、copy と答え、drop で `fm_dnd_receive_content` が
  `~/Desktop/Image.png`・`Text Clipping.txt`（`kl_tr`、在れば「 2」…、O_EXCL）に書き、`fm_desktop_dropped` で drop の cell に置く。log `ZFILES DND content`・`DESKTOP drop-file`。
- Photos（`photos/main.c`・`view.c`・`app.h`・Makefile）: grid の cell と 1 枚の表示の絵の press（`KL_HIT_ACTIVE`、`VIEW_ID_PICTURE`）を覚え、8 px 動くと drag。
  data は text/uri-list（`file://`、%XX）と image/png（NULL で始めて `kl_window_drag_fill`: 1 枚の表示は `view->picture`、grid は `ph_decode`→`ph_fit` 2048→`ph_turn`）。
  icon は thumb か表示の絵（中心を hot）。COPY だけ。drag の後に `kl_ui_pointer_cancel`。log `PHOTOS DND start|picture`。
- Notes（`notes/main.c`・`window.c`・`picture-file.c`・`app.h`・`notes.h`・Makefile 3 つ）: `app_put_image` を `app_place_image`（点を受ける）に分けた。Select の移動で pointer が窓の外へ出ると、
  画像（Notes が bytes を持つ物）を `kl_window_start_drag`（COPY|MOVE）にし、page の移動は取り消す。PNG は `notes_picture_png`（PNG はそのまま、PDF の行は包む、JPEG・RGBA は
  decode→向き→2048→PNG）で後から埋める。受け: `KL_DROP_IMAGE`、頁の上で copy（自分の drag は move）と答え、窓の短い辺の 1/3 の四角の枠（accent、`app_drop_draw`）、
  drop は `notes_picture_load_bytes`→点に挿入、自分の画像は drop の点へ移動。`KL_WINDOW_DRAG_DONE` で contact を終える。log `NOTES DND drag start|drop|drag done`。
  Linux の gcc の maybe-uninitialized（分けた関数の `state`）に memset を足した。
- PDF Viewer（`pdfviewer/find.c`・`view.c`・`main.c`・`viewer.h`・Makefile 3 つ・`platform/amd64/vmunix.mk` の link に libz-compat）: 選択の中の press が 6 px 動くと
  選択の文字を `kl_window_drag_text`（clipboard は変えない）、動かずに離すと選択を外す。文字の外の press が 400 ms 止まった後に動き、press の点が頁の画像（libpdf の
  `pdf_page_editor_hit`）なら、その quad の範囲を 2 倍（最大 2048）で白の上に描いて icon と PNG にし drag（COPY）。止まらずに動くと今と同じ pan。log `PDFVIEWER DND`。
- libbrowser（`page/link.c`・`page.h`・`view/view.c`、`include/browser/browser.h`）: `browser_view_image_at`・`browser_view_image_release`・`struct browser_image`
  （replaced の box を木で探し、`layout_node_bounds` で当たりを見る、src は `page_resolve_location` で絶対に、画素は premultiply して複写）。BROWSER_API_VERSION は 2 のまま。
  exports は `browser_view_*` の glob に入る。
- Browser の shell（`browser/shell/shell.c`・Makefile・`platform/amd64/vmunix.mk` の link）: 左の press が 8 px 動き、press の点に画像があれば drag（image/png は fill、URL は 2 つの文字の型）、
  無ければ今の動き。log `ZBROWSER DND drag image`。

設計からの違い・制限（記録）:
- Notes の drag には icon を付けない（compositor の紙の badge）。画像の画素を Notes は PNG・JPEG の bytes で持ち、icon のための decode を drag の始まりに足さなかった。
- PDF Viewer の長押しの合図（枠）は出さない（時計を足さず、止まった後の最初の動きで判定する）。
- Notes の落とす枠の大きさは画像の大きさが drop まで分からないので、窓の短い辺の 1/3 の四角。
- 試験の AAT（draft）: `tests/scenarios/desktop/dnd/text-between-windows.md`・`photo-to-notes.md`・`content-to-desktop.md`、`tests/scenarios/apps/pdfviewer/drag-out.md`、
  `tests/scenarios/apps/browser/image-drag.md`。

### 確認（2026-10-08、P1）

- amd64: `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws189` で `bin/textedit`・`bin/files`・`bin/photos`・`bin/notes`・`bin/pdfviewer`・
  `bin/browser`・`dynamic/libbrowser.so`: 各 rc 0、warning 0。
- Linux: `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws189-linux all`: rc 0、warning 0（Notes の maybe-uninitialized を直した後）。
- `sh plan/ws189/tests/run-host-png-write.sh`: ok（fit・rows を足した）。ASan・UBSan でも ok。
- `python3 plan/tools/style-check.py`（変えた file）: 新しい指摘 0（残りは元から）。`python3 plan/tools/aat/check-scenarios.py`: PASS。
- 未実施: QEMU の AAT（T1）、FreeBSD の build、実機。
