<!-- awesome-plan project=zedbsd record=ws113-p015 -->
# ws113-p015: 2 つ目以降の display の窓の状態（dock・floating・整列）、bar、リサイズ、App Home の背景

Status: test-wait（T1 の番号は Q1 が返す。2026-10-08 P1: head の dock bar をユーザーの決定どおりに実装、下の「head の dock bar（決定 2026-10-08）の実装」。前の T1-376 (a) `displays-p015.sh` PASS。残り: この QEMU 試験、head の上の press（リサイズ・bar・dock・bar の widget）の 5330 実機の確認）
Disposition: normal
Parent: [WS113](../ws.md)

## 由来（2026-10-08 ユーザーの UAT、5330 の実機、HDMI）

「・2つめのディスプレイにカーソール移動でき、ウィンドウも移動できました。
・2つめのディスプレイではウィンドウのリサイズができませんでした。
・2つめのディスプレイでウィンドウの最大化をどうするか悩みますね。ドックするためにバーを足すか、最大サイズにして終わるか。ドックを追加して、ドッキング可能にするのがよさそうです。ディスプレイごとに、ドック状態かフローティング状態かアレンジメント状態かを、状態として持つのがよさそうです。また、App Home画面にしたとき、2つめ以降のディスプレイはApp Homeの背景だけにするのがよさそうです。」

## 範囲（案、P1 が詰める）

1. 2 つ目以降の display の窓のリサイズ（縁の drag）を効くようにする（今は効かない、BUG）。
2. display ごとに bar（dock の bar）を持ち、その display で docking できる（p007 の「head の窓は dock の前に anchor へ戻る」を置き換え）。
3. display ごとに窓の配置の状態（docked・floating・整列（WS181 の arrangement））を持つ。
4. App Home を開いた時、2 つ目以降の display は App Home の背景（暗い stage・壁紙の blur）だけにする。
5. 試験: host、QEMU（Venus 2 出力、head 1 の bar・dock・整列・リサイズ・App Home の背景）、5330 の実機（ユーザー）。

## 2026-10-08 実装（q858 の後、P1）: 範囲 1・4

Q1 の ACK: 範囲 1〜5、1 → 4 → 2・3 の順。2 の head の帯に出す物はユーザーに確認中（Q1）。

### 1. head の窓のリサイズ

調べたこと（読み取り）: head での press は p007 の `remote` でも `window_at` → `HIT_FRAME` → `kwl_toplevel_resize_start` に届く。効かない原因として見つけた物:
- `frame_under_pointer`（shell.c）が anchor の帯（system bar・左右の desktops の swipe・下の Wiseview）を平面の座標のまま当て、`pointer_x >= server->width - DESKTOP_EDGE` で head の全ての点を外していた → head の窓の縁で resize の矢印が一度も出ない（縁が見つからない）。
- `resize_limit`（toplevel.c）が上限を anchor の幅・高さに、上端の限りを anchor の system bar の下（`window_lowest`）にしていた → anchor より大きい head では anchor の大きさで止まり、head の y が anchor と違うと上端の限りがずれる。

直し: `frame_under_pointer` の帯は pointer が anchor にある時だけ。`resize_limit` と `window_lowest` は窓の出力（`kwl_outputs` の slot、表示していなければ anchor）の大きさと `outputs[slot].y + kwl_output_top(slot)`（head は title bar の分だけ）。client の move の始まり（`move_anchor`）の上端の限りも同じ関数で窓の出力の物に。
- 実機で効かなかった原因がこの 2 つで全部かは、QEMU で見られない（usb-tablet は絶対座標で anchor だけ）。5330 でユーザーが確かめる。

### 4. App Home の時の head

- `home.c`: stage（黒い glass と上の中央の光）を `home_stage(x, y, width, height)` に分け、`kwl_home_draw`（anchor）と新しい `kwl_home_draw_head`（head の平面の矩形 `server->view_*`、icon・時計・検索は描かない）が使う。
- `shell.c` の `kwl_glass_draw_head`: App Home の progress が 0 より大きい間（開く・開いている・閉じる）は stage だけを描き、窓・popup を描かない（pointer は heads.c が描く）。head での press は App Home が見えている間は食べる（見えない窓を押さない）。release は従来どおり `kwl_home_button` が取る。
- `heads.c` の `heads_shows`: App Home が見えている間は mask の 4 を立て、head を毎 frame 描く（閉じた後の 1 frame も前の mask で描く）。
- 開く・閉じる動きの間、head は stage が出て窓が消えるだけ（anchor の窓が奥へ下がる動きは head には無い）。

### 確認（host・build）

- build: `make -j16 BUILD=build/p1-wl ZEDBSD_CONFIG=plan/ws113/tests/config-amd64-p005.mk build/p1-wl/bin/wayland` exit 0、warning 0。`make keiland-linux`（gcc）warning・error 0。
- host: `host-plane.sh` PASS（plain、ASan/UBSan）、`host-output-switch.sh` PASS。style-check は変えた file で増えない（shell.c 5→5、home.c 3→3、heads.c・toplevel.c 0）。
- 未実施: QEMU（T1、範囲 2・3 の後にまとめて依頼）: Super+Shift+Right で head 1 へ移した窓で App Home を開閉して head の PNG（stage だけ・閉じて窓が戻る）。head の縁の drag は実機（5330）。

### 3. display ごとの状態（docked・floating・整列）と 2. head の bar（2026-10-08、P1）

範囲 2 の head の帯に出す物は Q1 がユーザーに確認中。今の案（docked の窓の title bar だけ、時計・status・App Home・通知・desktops の pill は anchor だけ）で作った。答えが変われば直す。

- 状態を出力ごとに（`kwl.h`）: `layout_mode[KWL_PLANE_SLOTS]`（docked・windowed）、`dock_owner[desktop][slot]`・`dock_owner_gone[desktop][slot]`。整列（`arrange-shell.c`）は `arrange_desktops[desktop][output]`、swap・join・menu に output。新しい `kwl_output_top_window(server, slot)`（display.c）。
- shell.c: `layout_set`・`layout_leave`・`layout_front_follow`・`layout_log_windows` が slot を取り、`layout_follow` は docked の出力ごとに `layout_follow_output`。`layout_hides`・`layout_takes_press`・`docked_window`・`bar_dock_follow`・unmaximize の「前の窓」はその出力の top の窓で決める（anchor の docked が head の窓を隠さない）。`docked_rect(slot)` は head では head の矩形の bar の下、`glass_fit_on(slot)` は head の中に収める（undock・float-quiet・dock の戻り先）。`kwl_glass_work_area(slot)`・`kwl_glass_leave_quiet(slot)`・`kwl_arrange_end_all(output)`。anchor の log の行は今までと同じで、head の行だけ ` output=N` を付ける。
- head への dock: `window_dock` は anchor へ移さない（p007 の why=dock を除いた）。title bar の double click・最大化のボタン・client の maximize・head の bar への drag（`kwl_glass_toplevel_move_end` は pointer の出力の bar で、別の出力から来た窓は放した所を戻り先に）で、その head で docked。pull（`pulled_rect`・pull の完了）は窓の出力の幅で。
- head の bar（2）: `kwl_output_top` は head も system bar と同じ（bar＋間＋title bar）。`head_bar_layout`・`draw_head_bar`（strip、docked の窓の mark・title・menu か controls（`kwl_titlebar_draw`、area は head の上端）・ボタンの pill）・`head_bar_press`（閉じる・戻す・最小化、title の double click で戻す、press は pull に）。`kwl_glass_draw_head` が窓の上に bar、その上に popup と shell の menu（`kwl_menu_draw_popups`）。menu の popup の置き場（menu-shell.c `shell_layout`）は開いた点の出力の中に。docking の title bar の滑りは head の bar の位置へ（`bar_title_slot` は bar の top、head の pass も bar を渡す。前は NULL で、head の窓の dock の動きで落ちる所だった）。`draw_bar_strip`・`draw_bar_group_at` は位置を取る。
- head が消える時（抜去・lost・mirror）: その出力の docked と整列を先に終え（`kwl_glass_leave_quiet`・`kwl_arrange_end_all`、why=retreat・mode）、窓を anchor へ。head が動く時は戻り先（restore）も一緒に動かす。
- 制限: head の整列は状態は持つが始める口が無い（menu は anchor の pill から anchor の窓だけ。head の帯に pill を出すかはユーザーの答え次第）。fullscreen は今までどおり anchor へ移してから（protocol.c）。Wiseview・App Home・切り替えは anchor だけ。head の bar の dock の hint（drag の間の空間の絵）は無く、strip が明るくなるだけ。

### 確認（host・build、範囲 1〜4）

- build: `make -j16 BUILD=build/p1-wl ZEDBSD_CONFIG=plan/ws113/tests/config-amd64-p005.mk build/p1-wl/bin/wayland`（`ZEDBSD_TEST_SCREEN_CAPTURE=y` も）exit 0、warning 0。`make keiland-linux` の gcc・clang warning・error 0。
- host: `host-plane.sh`・`host-output-switch.sh`・`host-arrange.sh` PASS（plain、ASan/UBSan）。style-check は変えた file で HEAD と同じ数（shell.c・arrange-shell.c の残りは前からの物）。
- QEMU の試験（T1 へ）: 新しい `plan/ws113/tests/displays-p015.sh`（image は新しい `config-amd64-p015.mk` = p006 ＋ wltest）。head 1 へ移した窓が head の bar の下、anchor で dock しても head の窓は隠れず anchor の数え方だけ、anchor の整列は anchor の窓だけ、App Home の間 head は stage だけ、抜去で戻る、PNG（head1-bar・head1-anchor-docked・home-head1 ほか）。head の上の press（head の bar・縁の resize・head での dock）は QEMU の tablet が anchor だけなので実機（5330、ユーザー）。

## T1-372 の判定（2026-10-08 Q1）

FAIL 2 行（retry も同じ）: `the anchor's count has b docked and nothing hidden (a is head 1's)`、`Side by Side arranges the anchor's b alone (found 0 of 1)`。他（head 1 の bar の下、anchor の docked mode、head 1 の render list、App Home、unplug の retreat、KWL FAILED 無し）は ok。PNG は D-Bus で `no surface`。回帰の displays-p007・p014・ws181 p009・ws142 p010・boot-test は PASS。log: /home/awe/zedBSD-worktrees/t1/build/t1-372-p015/・t1-372-p015b/。


## 決定 2026-10-08（ユーザー）: 2 番目以降の display の dock bar

「2番目以降のディスプレイのdock barには、その画面に置いた window の window icon を出し、時計・状態・App Home・切り替えのつまみも表示する。」（P1 の案「title bar だけ、時計等は anchor だけ」は不採用。）P1 が ws090-p015 より先に実装する（q862 の前）。

## head の dock bar（決定 2026-10-08）の実装（2026-10-08、P1、q862 の前）

head の bar を system bar と同じ並びにした（`draw_head_bar`、shell.c）: 帯、左に launcher（Kei の mark）と線、docked の窓が無ければその head に置いた窓の application の icon（apps-bar.c）、
docked の窓があればその title・menu・ボタン（前と同じ）、右に desktops の pill（切り替えのつまみ）・状態の pill（IME・media・network・volume・battery）・時計の pill。

- layout: `bar_layout` を `bar_layout_on(server, slot, bar)` に一般化（出力の矩形の上端・右端から。`shell_bar` に `output`）。system bar は `bar_dock` の動き、head は docked の窓がある間すぐにボタンの分を空ける。
  shell.c の static の `output_rect` は heads.c の公開の `kwl_output_rect` に移した（同じ意味）。`draw_desktops`・`draw_status`・`draw_battery` は bar の top を使う
  （Wiseview の青い縁と `KWL GLASS desktops` の log は anchor だけ）。
- head の bar の log（新しい行、試験と実機の確認用）: `KWL GLASS head bar output=N top=Y launcher=X desktops=X status=X clock=X`（最初と位置が変わった時）。
- window icon: `struct apps_view` に出力（`output`・`area`・`top`）、`kwl_apps_view_build_on(server, slot, view)`。head の bar はその head の窓だけ、
  system bar と switcher は今までどおり desktop の全ての窓。待ち・previews・press は `server->apps_bar.output` の bar のもの（previews はその出力の中、その出力の pass で描く）。
  `KWL APPS bar ...`・`KWL APPS icon ...` の行は head では末尾に ` output=N`（anchor の行は前と同じ）。`kwl_glass_apps_room(server, slot, ...)`: head は Wiseview・fullscreen の cover を見ない。
- 状態の widget（volume.c・network.c・input-method.c）: icon の場所を出力ごとに持つ（plane.c の新しい `struct kwl_plane_places`、`kwl_plane_place`・`kwl_plane_placed`）。
  press はその出力の icon で見て、volume の popup・network の menu（と Alt の詳細）はその icon の下、その出力の中に開き、その出力の pass だけが描く。
  draw の関数は bar の top を取る（`kwl_volume_draw_icon`・`kwl_network_draw_icon`・`kwl_ime_indicator_draw`）。`KWL VOLUME icon`・`KWL NETWORK icon`・`KWL IME indicator` の log は anchor だけ（前と同じ）。
- 切り替えのつまみ（desktops の pill）: `kwl_glass_desktops_pill(server, slot, ...)`。head の pill の press で整列の menu がその head 用に開く（`arrange_menu.output`、
  head の中に置き、その pass で描く。log は ` output=N` 付き）。p015 の「head の整列は始める口が無い」の制限はこれで無くなる。swap の drag の上限は出力の bar の下（`kwl_output_top`）。
- App Home: head の launcher の press で `kwl_home_toggle`（App Home は anchor に開き、head は今までどおり stage だけ）。時計の press は Calendar（system bar と同じ）。
- press の経路（`kwl_glass_button`）: media・IME・volume・network・整列・apps の `!remote` の除外を外し（head では anchor の fullscreen の cover を見ない）、各 widget が出力ごとの場所で判定する。
  launcher と時計は `head_bar_press`。
- heads.c: `heads_shows` に 8（bar）。glass の session（login・lock・画面 off でない）では head を毎 frame 描く（時計・状態・icon の変化のため）。

人の判断を Q1 に送った点:
1. 「切り替えのつまみ」は desktops の pill（desktop の切り替え・整列の menu）と読んだ。ユーザーにも意味が分からず、答え待ち（2026-10-08 Q1）。今は desktops の pill のまま。
2. （決定 2026-10-08 ユーザー）「各番目の画面のdock barには、その画面のウィンドウのみを出してください。app単位ではなくwindow単位にします。でも、画面ごとのapp iconsは、ウィンドウがあればその画面のdockに表示され、プレビューはその画面のウィンドウだけにします。」
   Q1 の読み: anchor を含む全ての画面で、bar の app icon は「その画面に window を持つ app」だけ（window 単位: app の window のどれかがその画面にあれば出る）、previews もその画面の window だけ、
   同じ app が 2 つの画面に window を持てば両方の bar に出る、switcher は全ての window のまま。
   実装（P1）: apps-bar.c の `view_collect_on(server, slot, every, view)`: bar（`kwl_apps_view_build_on`）は全ての出力でその出力の窓だけ、上の窓もその出力の top、
   switcher（`kwl_apps_view_collect`）は every で全ての窓と desktop の top。`displays-p015.sh` に「the system bar has b's icon alone (a is head 1's)」を足した。
   1 つの display だけの時は今までと同じ（全ての窓が anchor）。

### 確認（host・build）

- build: `make -j16 BUILD=build/p1-wl ZEDBSD_CONFIG=plan/ws113/tests/config-amd64-p005.mk build/p1-wl/bin/wayland` exit 0、warning 0。
  Linux: `make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p1-linux-{gcc,clang} CC={gcc,clang} all` とも exit 0、warning 0。
  agent/p1 の ws090-p015 の途中（kl_scroll、KL_VERSION 61）の利用者: `build/p1-wl/bin/{phone,mailer,photos,music,calendar,textedit}` exit 0、warning 0。
- host: `host-plane.sh`（`test_places` を足した）・`host-arrange.sh`・`host-output-switch.sh` PASS（plain、ASan/UBSan）。
- style-check: 変えた C の file で HEAD と同じ数（新しい指摘 0）。
- QEMU（T1 へ依頼）: `displays-p015.sh` に head の bar の確認を足した（`KWL GLASS head bar output=1` の launcher・desktops・status・clock が head 1 の中に左から並ぶ、
  `KWL APPS bar count=1 ... apps=p015.a output=1`、head1-bar.png）。他の行は前と同じ。
- 未実施: head の bar の press（launcher・時計・icon・previews・desktops の pill の整列・volume・network・IME）は QEMU の tablet が anchor だけなので 5330 の実機（ユーザー）。

## 決定 2026-10-08（ユーザー、2 回目）: 画面ごとの dock の window

「各番目の画面のdock barには、その画面のウィンドウのみを出してください。app単位ではなくwindow単位にします。でも、画面ごとのapp iconsは、ウィンドウがあればその画面のdockに表示され、プレビューはその画面のウィンドウだけにします。」→ 1 番目（anchor）を含む全ての画面で、bar の app icon はその画面に window を持つ app だけ、icon の previews はその画面の window だけ。switcher は全ての window のまま。P1 に送った（q862 の前に入れる）。
「切り替えのつまみ」（上の決定の文）はユーザーにも意味が分からなかった（前のセッションの Q1 の書いた選択肢の文の見込み）。P1 は desktops の pill と読んで実装した。ユーザーの答え待ち。
