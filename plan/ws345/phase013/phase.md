<!-- awesome-plan project=zedbsd record=ws345-p013 -->

# ws345-p013 libkeiland の resize の口（描画の thread のため）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md) §6.3。
- 状態: cleared（2026-11-02 P4 q1974-i01。受け入れの 3 点は host・Wine で確かめた。`cross-build.sh` の PASS の行は host の負荷による winarm64 の時間切れで得ていない。winarm64 は別に build して warning 0。Q1 が main で流し直す）

## 目的

shell の描画の thread が窓の thread と別に present しても、resize が正しく見えるようにする libkeiland の口（design.md の敵対的レビューの指摘 4）。

## file

- `userland/desktop/include/keiland/keiland_window.h`・`libkeiland/ui/window.c`（Wayland）: configure の ack を listener の中でなく、program が新しい大きさの最初の present の直前に頼める口（`kl_window_configure_take`・`_ack`。ack は窓の thread の post で行い、描画の thread はその完了を待ってから present する）。口を使わない program は今のまま listener で ack する。
- `libkeiland/win32/window.c`: system の modal loop の間の大きさ（`WM_SIZE`・`WM_SIZING`）を `KL_WINDOW_RESIZE` で知らせる口（選ぶ program だけ）。
- `KL_VERSION` を上げ、既定は今の動き。libkeiland は他の WS の物でもあるので、Q1 が所有を調整する。

## 試験

- libkeiland の host の試験（Wayland の configure の順: ack の前に新しい大きさの commit が無い）、Win32 の Wine で modal loop の間の resize の通知。
- `sh plan/ws334/tests/cross-build.sh`、Keiland の app の回帰（Q1 が選ぶ既存の試験）。

## 受け入れ

- 口を使わない program の動きが前と同じ。使う program で、Wayland の ack と present の順が守られ、Win32 で modal loop の間に大きさが届く。

## 結果（2026-11-02 P4 q1974-i01）

### 口（KL_VERSION 109、`keiland_window.h`・`keiland_version.h`・`exports.map`）

- `kl_window_set_configure_deferred(window, deferred)`・`kl_window_configure_take(window, &configure)`・`kl_window_configure_ack(window, serial)`、`struct kl_window_configure`（serial・width・height）。窓の thread（app の窓は app の thread）で呼ぶ。描画の thread は `kl_window_post` で窓の thread に ack を頼み、その完了を待ってから present する（libkeiland の中で待たない: 閉じる時の join と待ちが絡まないように、待ちと止めの印は shell の物）。
- Wayland（`ui/window.c`・`ui/window.h`）: 遅らせる窓では、大きさ（か maximized）を変える configure を listener で ack せず待たせる（KL_WINDOW_RESIZE は前のとおり出る）。大きさを変えない configure は、待つ物が無ければすぐ ack、待つ物があればそれと一緒に（最新の serial で）ack。最初の configure は常にすぐ ack。take の後にまた大きさが変われば、ack は take した serial だけを送り、後の大きさは次の take と ack を待つ。遅らせを止めると待つ物をすぐ ack。desktop の surface は ENOTSUP。ack は `wl_display_flush` までして、後の別の thread の commit より前に compositor に届く。
- `kl_window_on_resize(window, callback, data)`（`kl_window_resize_fn`）: Win32（`win32/window.c`・`window-win32.h`）の system の modal size loop（WM_ENTERSIZEMOVE〜WM_EXITSIZEMOVE）の間、content の大きさが変わるたびに窓の thread で callback を呼び、`kl_window_size` もその大きさを返す。loop の終わりの KL_WINDOW_RESIZE は前のとおり出る。NULL で止める。Wayland では呼ばない（resize の間も入力を取れる）。WM_SIZE（`window_layout`）で知らせ、WM_SIZING は使わない（WM_SIZING は変わる前の提案の矩形で、content の実際の大きさは WM_SIZE の後に決まる）。Win32 の configure の 3 つは set=0、take=EAGAIN、ack=EINVAL。
- 口を使わない program の動きは変えていない（遅らせの既定は off、callback の既定は NULL）。

### 試験（全て 2026-11-02、host）

- `sh plan/ws345/tests/configure-test.sh build/ws345-p013-linux`（新規。`configure-test.c`・`configure-server.c`・`configure-server.h`）: 試験の中の Wayland server（wl_compositor 4・wl_shm・xdg_wm_base 4）が、ack は送った未消費の serial か、buffer の commit は最後に ack した configure の大きさか、を数える。app の窓（KL_PRESENT_NONE）を別の thread が自分の queue の wl_shm buffer で commit し（Vulkan の swapchain の代わり）、新しい大きさの前に `kl_window_post` で ack を頼む。既定（すぐ ack）・遅らせ（take の前は ack 無し、古い大きさの frame は許される、ack の後に新しい大きさ）・EALREADY・EINVAL・EAGAIN・待つ間の同じ大きさ・take の後の大きさ・待つ物の無い同じ大きさ・maximized・遅らせを止める、と対照（既定の窓で ack の後の古い大きさの frame を server が捉える）。PASS 3 回（`PASS acks=10 commits=10 resizes=8`）。TSan の libkeiland（`CC="cc -fsanitize=thread"`、gcc の `-Wtsan` が既存の audio.c の `atomic_thread_fence` で error になるので `KEILAND_LINUX_EXTRA_CPPFLAGS=-Wno-tsan`）で PASS 3 回、ThreadSanitizer の警告 0。変異の確かめ: listener の待ちの条件を壊すと 17 件の FAIL（`a frame 500x350 after the ack of 102 (640x480)` など）、戻して PASS。
- `sh plan/ws345/tests/resize-wine.sh`（新規。`resize-main.c`）: x86_64 の Windows build の試験の program を Wine・Xvfb で、xdotool で frame の右端を 3 回 drag（system の modal size loop、WM_ENTERSIZEMOVE・WM_SIZE・WM_EXITSIZEMOVE を WH_CALLWNDPROC の hook で見る）。1 回目 callback 無し: 呼ばれない、loop の後に KL_WINDOW_RESIZE。2 回目 callback 有り: loop の中で窓の thread に 8 回、大きさは増えるだけ、callback の中の `kl_window_size` が同じ、最後の 880x400 が窓の大きさ、loop の後に KL_WINDOW_RESIZE。3 回目 NULL: 呼ばれない。`resize-wine: PASS`（2 回）。Wine の WM_SYSCOMMAND SC_SIZE の keyboard の loop は Xvfb で大きさを変えなかったので使わない。
- 既存の回帰: `sh plan/ws314/tests/host-test.sh post build/ws345-p013-linux` PASS、`… process …` PASS。
- build: Linux の Keiland 全体（`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws345-p013-linux all`）exit 0・warning 0。zedBSD の `build/ws345-p013/dynamic/libkeiland.so`・`bin/browser`・`bin/terminal`（`ZEDBSD_CONFIG=plan/ws314/tests/config-amd64-browser.mk`）exit 0・warning 0。`exports.py --check` は exports.map を書き直した後。
- `sh plan/ws334/tests/cross-build.sh`: 1 回目は win64 が全て通り、winarm64 の openssl の build が script の 900 s の時間切れで止まった（host の load 15、code の失敗ではない）。2 回目も同じ所で時間切れ（load 22）。代わりに `make browser-winarm64 BROWSER_WIN32_BUILD=build/tmp/p013/winarm64`（時間 3600 s）で exit 0・warning 0、OmniGrammaton.exe ができた。winarm64 の import と公開の関数の確かめ（cross-build の 3〜7）は未実施（win64 では通った、source は同じ）。

### 未実施

- QEMU（zedBSD の Keiland の compositor）・実機・本物の Windows での resize。Keiland の compositor そのものの configure の扱いは試していない（試験の server だけ）。
- Keiland の app の回帰（phase.md の「Q1 が選ぶ既存の試験」）は Q1 の指定が無く、host の ws314 の 2 本だけ。
- shell が口を使う所は p007。
