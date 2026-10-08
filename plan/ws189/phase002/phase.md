<!-- awesome-plan project=zedbsd record=ws189-p002 -->
# ws189-p002: libkeiland と compositor — 画像の型、受け入れの印、icon、dock の spring-loaded、画面をまたぐ、複数の data device

Status: test-wait（T1-433、2026-10-08 Q1 が依頼。実装・build・host 試験は済み）
Disposition: normal
Parent: [WS189](../ws.md)
Queue: q892（P1、2026-10-08）
Design: [p001](../phase001/phase.md) §2・§3・§5・§6

## 範囲

p001 の §2（libkeiland）と §3（compositor）、§5 の p002 の試験。app（p003）と Mail（p004）は含まない。

## 記録

### 実装（2026-10-08、P1）

libkeiland（KL_VERSION 70、main の統合で WS188 の 71 の後ろ。keiland.h の注に 70 と 71 の両方）:
- `include/keiland/keiland.h`: `KL_DROP_IMAGE`、`struct kl_drag_icon`、`KL_DRAG_ICON_MAX`・`KL_DROP_RING`・`KL_DROP_CARET`・`KL_DROP_FILL_ALPHA`、
  `kl_window_start_drag_icon`・`kl_window_drag_fill`・`kl_drop_frame`・`kl_drop_caret`・`kl_ui_pointer_cancel`。exports.map は exports.py で作り直し。
- `libkeiland/ui/clipboard.c`: 型 "image/png"（順は file 名 → 画像 → 文字）、型ごとの上限（file 名 1 MiB・画像 64 MiB・文字 16 MiB、読む場所は倍に伸ばし上限+2 で止める）、
  答えの重複の抑止（`drop_answered`）、drag の icon（160 px に箱の平均で縮め、不透明度 217/255、淡い縁、角 6 px。start_drag の後に attach(-hot)）、
  `kl_window_drag_fill`（data を後で埋める）、同じ program の別の窓の drag の直読み（`clipboard_drag_owner`）、文字の型（UTF-8 が無ければ text/plain）、
  `clipboard_send` の write の間の SIGPIPE の無視、型の数 6。
- `libkeiland/ui/present-shm.c`: `shm_make`・`shm_free` を内部の `keiui_shm_make`・`keiui_shm_free` に（icon の buffer に使う）。
- `libkeiland/ui/drop-look.c`（新）: `kl_drop_frame`・`kl_drop_caret`。`libkeiland/ui/ui.c`: `kl_ui_pointer_cancel`。Makefile 3 つに drop-look.c。

compositor（`userland/desktop/wayland/`）:
- `dnd-state.c`・`.h`（新、server を知らない）: 印の判定 `kwl_dnd_mark`（中立・コピー・移動・選ぶ・不可）と名前。Makefile 3 つに追加。
- `data.c`: target の client の全部の data device に offer と enter・motion・leave（`dnd_devices`・`dnd_offers`、最大 16）、`drag_pick`・`drag_forget`・`drag_clear_target`、
  drop は選ばれた device に・ほかは leave、`SOURCE_TARGET` は選ばれた offer だけ、drop の前の drag の offer の receive を空に（log `refused=not-dropped`）、
  `kwl_data_drag_mark`（log `KWL DATA drag state=…`、dirty）、spring の間は target 無し、icon の offset を start_drag で 0 に、enter の log に devices と output。
- `protocol.c`: 今の drag の icon だけ attach の 0 以外の offset を受け、commit で累計（`offset_x`・`offset_y`）。`compose.c`: icon を pointer + 累計に、印を描く。
- `shell.c`: `kwl_glass_draw_drag_mark`（緑の +、accent の 3 点、赤の横棒）。
- `apps-bar.c`: spring-loading（SPRING_MS 700、icon の光が強まる、1 窓は `kwl_glass_switch_to`、複数は preview、preview の上で 700 ms でその窓、離れて 300 ms で preview を閉じる、
  drag の終わりで消す。log `KWL APPS spring app=… surface=…|previews=N`、preview の via=spring）。`kwl.h`: 状態の field と定数。
- `seat.c`: drag の間は `kwl_seat_pointer_update` が何もしない（spring で前に出た窓に pointer の enter を送らない）。
- `heads.c`: drag の間は全部の head が pointer の分を描く。

試験:
- `userland/tests/data-probe/main.c`: drag の受け（enter ですぐ receive して `when=early`、copy で受け、drop で `when=drop`、finish）。
- `plan/ws189/tests/host-dnd-state.c`・`run-host-dnd-state.sh`（印の表の全部の組）。
- AAT（draft）: `tests/scenarios/desktop/dnd/`（same-program-windows・refused-mark・dock-spring・early-receive・across-displays、text-between-windows は p003）。
  試験の image: `plan/ws189/tests/config-amd64-aat-dnd.mk`（AAT の image に data-probe）。

### 確認（2026-10-08、P1）

- `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws189 build/ws189/dynamic/libkeiland.so build/ws189/bin/wayland build/ws189/bin/data-probe build/ws189/bin/terminal build/ws189/bin/files`: rc 0、warning 0（main c671551fe を merge の後）。
- `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws189-linux all`: rc 0、warning 0。
- `sh plan/ws189/tests/run-host-dnd-state.sh`: ok（165 checks）。`python3 userland/desktop/libkeiland/exports.py --check`: ok。
- `sh plan/tools/keiland-os-boundary/check.sh`: PASS。`python3 plan/tools/aat/check-scenarios.py`: PASS（115）。
- `python3 plan/tools/style-check.py`（変えた file）: 新しい指摘 0（残りは元からの blank-after-brace: clipboard.c 3・compose.c 4・shell.c 5・ui.c 2）。
- 未実施: QEMU の AAT（T1）。FreeBSD の build（Makefile.freebsd に 2 file を足しただけ）。実機。

### T1-433・T1-434 の結果と直し（2026-10-08、Q1 の判定の伝達）

- 確認できた: early-receive、refused-mark、dock-spring、text-between-windows 1〜4、photo-to-notes 1〜2、browser image-drag 1（Q1 も PNG を目視）。
- **F1（直した）**: 押して動かしすぐ離すと `state=refused` のまま cancel になった（T1 は 1.5 秒止めてから離す必要があった）。原因: release の時点で target の offer の
  答え（accept・set_actions）がまだ届いておらず、`kwl_data_drag_release` が「受けていない」と判断した。直し（`wayland/data.c`・`kwl.h`・`data.h`・`main.c`）:
  offer に `dnd_answered`（accept が来たか）を持ち、答えの無い offer の上の release は `dnd_releasing` にして待つ（log `KWL DATA drag release wait`）。
  `kwl_data_tick`（main loop の毎回）が、答えが来たら（型を受けて action が決まった時、または断った時）、または 500 ms で、release を決める（log
  `KWL DATA drag release decided answered=0|1`）。待つ間の pointer の動きは target を変えない。build（amd64 wayland・Linux all）warning 0、境界 PASS。
- F2〜F4 は q896 の後（F2: same-program-windows の devices=1、F3: Browser の画像の url が path、F4: シナリオの status: draft と未実施の項目）。

### T1-436 の結果と直し（2026-10-08、Q1 の伝達）

- 確認できた: F1 の直し（text・refused-mark の即離しで release wait → decided answered=1 → drop / cancel）。
- **F6（直した）**: Photos から画像を drag して 40 ms で離すと Photos が compositor に切られた（`KWL DATA drag refused` の後 `KWL CLIENT gone reason=error`）。
  原因は start_drag そのものではない（ボタンが上がっていれば source に cancelled を送り 0 を返していた）。続けて client が icon の surface に
  `attach(buffer, -hot_x, -hot_y)` を送り、protocol.c が「今の drag の icon でない surface の 0 でない offset」を EPROTO にしていた（ws189-p002 で足した規則）。
  直し: start_drag が icon に名指した surface に `drag_icon` の印を付け（drag が始まっても断られても）、その surface の offset は今の drag の icon の時だけ数え、
  それ以外（断られた・終わった drag の icon）は無視して client を切らない（`kwl.h`・`data.c`・`protocol.c`）。経路は libkeiland の `kl_window_start_drag_icon`
  の 1 つなので、Notes・PDF Viewer・Browser・Files の画像の drag も同じく直る。client 側（clipboard.c）は cancelled で `clipboard_drag_end` するので変更なし。
  シナリオ `desktop.dnd.photo-to-notes` に手順 5（Photos と Notes の 40 ms の即離しを 3 回ずつ、`KWL CLIENT gone` が無い）を足した。
- **F5（記録、実装は設計どおり）**: 設計 §3.3 は「窓が 1 つの app は icon の上 700 ms で前に、2 つ以上は preview を出し preview の上 700 ms で前に」。T1-436 の
  撮影 07 では Text Editor の窓が 2 つあった（前の手順の窓が残っていた）ので preview になった。シナリオ `desktop.dnd.dock-spring` の準備に「Text Editor の
  窓を 1 つだけ」と書き、窓が 2 つの場合を手順 4 として足した。
- 確認: build（amd64 wayland・Linux all）warning 0、`run-host-dnd-state.sh` ok（165）、style の新しい指摘 0、`check-scenarios.py` PASS。QEMU は T1 へ。

### F2〜F4（2026-10-08、q896 の後）

- **F2（期待が誤り、試験を直した）**: same-program-windows の `devices=1` は正しい。Terminal の New Window（Ctrl+Shift+N）は fork と exec で別の process を起動する
  （terminal/main.c `main_new_window`）ので、その client の data device は 1 つ。設計 §0 の欠け 1 の「Terminal・Notes の 2 つ目の窓が該当」は読み違いで、今の Kei の
  program に 1 つの process で窓を 2 つ持つ物は無い（`kl_app_window_create` の呼び手は各 1 窓）。§3.6 の経路（1 つの client の複数の device への enter・
  motion・leave、型を受けた最初の device への drop、他の device の leave）は、`data-probe --two-devices`（同じ seat に 2 つ目の wl_data_device、log
  `DATAPROBE drag enter|leave|drop device=2`）で確かめるよう `desktop.dnd.same-program-windows` を書き直した。libkeiland の「自分の surface でない enter を断る」側は
  今の program では起きない（2 窓の program ができた時の試験は Future Work の候補）。
- **F3（実装を直した）**: Browser の画像の drag の text が path（`/tmp/aat-work/photo.png`）だった。`page_image_at`（libbrowser/page/link.c）が読み込み用の
  `page_resolve_location`（file: の URL を path にする）を使っていた。drag では `link_resolve` と `net_url_serialize` で絶対の URL（`file:///tmp/aat-work/photo.png`）にする。
- **F4**: ws189 の 10 のシナリオ（desktop.dnd.* の 8、apps.pdfviewer.drag-out、apps.browser.image-drag）を自己レビュー（log の行が source にあること、F2・F5・F6 の
  直し）して `status: active` にした。Files・Terminal の DnD の回帰として `apps.files.drag-between-windows`（Files の窓の間の移動、Terminal への path、即離し）を足した（active）。
- 確認: amd64 の data-probe・libbrowser・browser と Linux all の build warning 0、境界 PASS、style の新しい指摘 0、`check-scenarios.py` PASS（122）。QEMU は T1 へ。

### F7（2026-10-08、T1-439 の desktop.dnd.same-program-windows）

- 観察（T1-439、撮影 `/home/awe/zedBSD-worktrees/t1/build/t1-439/shots/`）: enter は `devices=2` で両方の device に届いたが、drop は device 2 に行った
  （`DATAPROBE drag leave`（device 1）、`DATAPROBE drag drop device=2`、`received device=2 when=drop bytes=7`）。期待は device 1 の drop と device 2 の leave。
- 原因（**実装を直した**）: `drag_enter` は client の object の list（`objects.c` 119〜120 行: 新しい object を先頭に足す）を先頭から歩いて device を slot に入れていたので、
  新しい device（data-probe の 2 つ目）が slot 0 になり、両方が型を受けた時に `drag_pick` の「型を受けた最初の組」が device 2 を選んだ。
  設計 §3.6 の「最初の組」は作られた順（program の最初の窓）の意味で、data-probe の `--two-devices` の説明（「2 つ目は後に告げられ、最初の device が drop を受ける」）と
  シナリオもその前提。Wayland の規格は複数の data device の間の drop の宛先を決めていない（wlroots は全部の device に drop を送る）ので、この compositor の
  「落とした 1 つだけ」（ユーザーの原則「データは落とした窓にだけ」）の中で、順を作られた順に固定するのが筋。libkeiland の program では自分の surface の窓だけが型を受けるので、
  順は今の program の振る舞いを変えない。
- 直し: `userland/desktop/wayland/data.c` に `drag_devices_in_order`（生きた data device を古い順に、`KWL_DND_DEVICES` を越える時は古い 16 を残す）を足し、
  `drag_enter` はその順で enter を送り slot に入れる。`drag_pick`・motion・leave・drop は slot の順のまま。シナリオと data-probe は変えない。
- 確認: `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws189 build/ws189/bin/wayland` warning 0、
  `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws189-linux all` warning 0、`plan/tools/style-check.py userland/desktop/wayland/data.c` 指摘 0、
  `git diff --check` 0。host の試験は無い（data.c の drag は compositor 全体が要る）。QEMU は T1 へ（desktop.dnd.same-program-windows 1〜2 の再試験）。
