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
