<!-- awesome-plan project=zedbsd record=ws345-p007 -->

# ws345-p007 shell（三つの platform）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: cleared（2026-11-02 P8 q1988-i01。Linux の Keiland は browser の build が無く、Q1 の判断で範囲の外: 未実施）

## 目的

browser の shell（zedBSD・Linux の Keiland、Windows の OmniGrammaton に共通の `browser/shell/`）を `BROWSER_THREADING_OWN` にし、窓の thread は event だけ、描画は shell の描画の thread で行う（D1・D5、design.md §6.3）。

## file

- `userland/desktop/browser/shell/shell.c`・`window.c`・`present.c`・`present-cpu.c`・`touch.c`・`mail.c`・`internal.h`、新しい `shell/render.c`（描画の thread、kl_thread で作る shell の thread）。
- `apps/browser-win32/`（必要なら。WM_COPYDATA の道は今のまま窓の thread）。
- libkeiland の resize の口は p013（この Phase は p013 に依存）。

## 手順

1. view を `_OWN` で作る。窓の loop から `browser_view_prepare`・`shell_frame` を外し、描画の thread へ。
2. surface は窓の thread で作り、device・swapchain・present は描画の thread。resize は p013 の口で、ack を描画の thread の新しい大きさの最初の present の直前に（R8）。止め方: acquire の時間切れ 100 ms、閉じる順（描画の thread の join → swapchain・surface → 窓 → view）、Windows は join の間も message を回す（design.md §6.3）。CPU の道は描画の thread が描いて窓の thread が shm の present。
3. drag の画像は `browser_view_request_image` と callback、mail は `_focus_field` の答えの callback、文字入力は session の番号付きの commit、touch の scroller は snapshot と命令。
4. Windows: system の modal loop の間も描画の thread が描く事を確かめる。

## 試験

- zedBSD の QEMU（Q1 が割り当てる名前・port）: `plan/tools/boot-test.sh` と browser の頁の画面（`plan/ws074/tests/browser-guest.sh` の道、画面の PNG で判定）。長い script の頁（probe の loop の頁を 30 s に）を開いて、その間に titlebar の reload・location の操作と resize が効く事を画面の PNG で。
- Windows: Wine で同じ頁（plan/ws334 の Wine の道）。本物の Windows の確認はユーザーが拾える試験の一覧に（Q1）。
- Linux の Keiland（WS105 の guest、Q1 が割り当てる）: 同じ。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 三つの platform で起動し、長い script と connect の掛かりの間も窓の操作（titlebar・resize・再描画）が効く（画面の PNG の証拠）。

## 引き継ぎ（2026-11-02 20:25、p006 の P8 の調べ。main で確かめてから使う）

- 今の描画: `shell/shell.c` の窓の loop（345 行付近）が `shell_frame()` を呼び、`shell_frame()` が `browser_view_prepare`（939 行付近）を呼ぶ。どちらも描画の thread へ移す。
- GPU の道（`shell/present.c`）: `browser_view_set_gpu`（123 行）、`browser_view_release_targets`（151 行）、`browser_view_record`（224 行）。描画の thread では `browser_view_render(view, &target, cmd, &serial)`、`vkQueuePresentKHR` の後に `browser_view_presented(view, serial)`。_OWN では set_gpu と release_targets は既に描画の thread の呼び出しとして動く。
- CPU の道（`shell/present-cpu.c`）: `browser_view_draw_pixels`（69 行）→ 描画の thread の `browser_view_render_pixels`。shm の present は窓の thread に残す（design §6.3）。
- 描画の thread: `shell/threads.c` が kl_thread_start・join・is_current を包んでいる。新しい `shell/render.c` がそれで thread を起こす。
- view の option の `frame_wake` が描画の thread の condition を鳴らす（script の thread から呼ばれる）。rAF は `browser_view_presented` の後にだけ走るので、描画の loop は present のたびに呼ぶ（呼ばないと頁の animation が止まる）。
- _OWN では program の thread の `_settle`・`_dump`・`_draw_pixels`・`_prepare`・`_draw`・`_record`・`_image_at` は ENOTSUP。drag の画像は `browser_view_request_image` と image の callback、mail の sign-in の道は `_focus_field` と field の callback、文字の確定は `browser_view_commit_text_in`（session の番号付き）。どれも p005 からある。
- 描画の loop の手本: plan/ws345/tests/host-threaded-view.c の `test_gpu_render`（command pool・render・submit・wait）、plan/ws345/tests/stall-probe.c の描画の thread（`frame_wake` の後に `render_pixels`）。
- 規模: shell の code は `userland/desktop/browser/shell/*` で約 5,900 行、3 つの platform（QEMU、Linux Keiland の guest、Wine）。

## 結果（2026-11-02 P8 q1988-i01）

### 実装

- `userland/desktop/browser/shell/shell.c`: view を `BROWSER_THREADING_OWN`（`frame_wake` は描画の thread の condition）で作る。窓の loop は event を view の口に渡し、`browser_view_process` と snapshot を読むだけ（`browser_view_prepare`・`shell_frame`・`browser_view_timeout` を外した）。FRAME の行は描画の thread の present（GPU）か窓の thread の shm の present（CPU）の後。文字入力は snapshot の通し番号が変わった時に追う。
  - drag の画像: 押して 8 px 動いたら `browser_view_request_image`、答え（`image` の callback）が来た時に押したままなら drag を始める（離せば捨てる）。
  - 文字の確定・削除は `browser_view_commit_text_in`（窓の text input を頼んだ時の session の番号）。
  - 開けなかった最初の頁（BUG-320）: OWN では `browser_view_load` が積むだけなので、最初の頁が出る前の load の FAILED の callback で失敗の頁を頼み、その頁が committed になった時に URL を覚える。
  - READY の時点で頁がまだ committed でなければ NAVIGATE を書かない（Wine の M4 が 2 行目を読むため）。
  - 閉じる順: 診断の clock → 描画の thread（止めて、待つ間も窓の event を回す）→ presenter → titlebar → mail → 窓（`shell_window_drop`、application は残す）→ view → 描画の thread の残り → instance → application。
- `shell/render.c`（新）: 描画の thread `browser-render`（kl_thread）。GPU: present.c の swapchain を持ち、新しい大きさで作り直し、`browser_view_render` で記録、submit、present、`browser_view_presented`。acquire は 100 ms の時間切れ（止めを見る）。Wayland の configure は窓の thread が `kl_window_configure_take`（窓に `kl_window_set_configure_deferred`）、描画の thread は新しい大きさの最初の present の前に `kl_window_post(SHELL_POST_ACK)` で窓の thread に ack を頼み、答えを待ってから present（新しい take があれば EINVAL でその大きさを描かない）。CPU: 3 つの frame（描く・最後に描いた・見せる）に `browser_view_render_pixels`、窓の thread が `kl_window_present`（古い大きさの frame は捨てる）。Win32 の modal の size loop の間は `kl_window_on_resize` の callback が view の resize と描画の thread の大きさを渡す（GPU の道。CPU の道は loop の後）。
- `shell/latency.c`（新）: 診断の口 `BROWSER_LATENCY`（環境変数、設定した時だけ）。`browser-latency` の thread が 5 ms ごとに前の tick が取られていれば `kl_app_post(SHELL_POST_TICK)`、窓の thread が取るまでの待ちを測り、100 ms を越えた待ちはその場で、終わりに `ZBROWSER LATENCY samples= longest_ms= over_20ms= over_100ms=` を書く（p011 の 3a の行）。
- `shell/present.c`（`browser_view_render`・時間切れの acquire）、`present-cpu.c`（窓の thread の shm の present だけ）、`window.c`（SHELL_POST_ACK と SHELL_POST_TICK）、`mail.c`（`_focus_field` の答えの `field` callback で打つ／clipboard）、`internal.h`、`sources.mk`。
- libbrowser（Q1 の許可、`view/front.c` の通知の queue だけ、別の commit 7ca1c4fdbe）: committed・load・title の通知を、その状態の snapshot が publish されるまで待たせる（`held`、`front_publish` が published を替える lock の中で外し、`view_front_process` は最初の held の前まで順に伝える）。前は navigation の committed の callback が古い頁の url・title を読み、NAVIGATE と titlebar が古い頁のまま残った（QEMU の 3 つの場面と Wine の M4）。

### 試験（2026-11-02、全て P8 が流した）

| 試験 | 結果 |
| --- | --- |
| zedBSD amd64 `bin/browser`・`libbrowser.so`（`plan/ws074/tests/config-amd64-browser.mk`、-Werror） | exit 0、warning 0（merge の後も） |
| zedBSD arm64（`config/ci/config-rpi5.mk`）・riscv64（`config-visionfive2-keiland.mk`）の `libbrowser.so`・`bin/browser` | exit 0、warning 0（front.c の直しの後） |
| `sh plan/ws345/tests/shell-guest.sh`（新、zedBSD の QEMU、Venus の GPU、image は config-amd64-browser.mk と test の頁、main の merge の後に作り直した image） | **15 passed、0 failed、PASS**。busy（30 s の loop）・xhr（192.0.2.1 への同期の XHR）・busy-cpu（--no-gpu）の各々で script の間に: titlebar の reload が 1〜2 s で効く、右下の角の drag で compositor が決めた 1050x640 の frame が 1〜2 s で出る（script はまだ走っている）、Ctrl+L で打った about.html が開く（NAVIGATE path=/usr/share/browser/about.html title=About browser）、窓の thread の待ち: longest 2.0 / 4.0 / 15.0 ms、over_20ms 0、over_100ms 0（約 3700〜4200 samples）、ZBROWSER ERROR 0。画面: `build/review/ws345-p007/qemu-*.png` |
| `plan/tools/boot-test.sh build/ws345-p007-image/hdd-image.img` | PASS（`build/review/ws345-p007/qemu-boot-login.png`） |
| `sh plan/ws334/tests/wine-run.sh`（front.c の直しの後） | 21 passed、0 failed、6 not run（本物の Windows）、PASS。直しの前は M4 だけ FAIL（committed の古い snapshot と READY の空の NAVIGATE） |
| `sh plan/ws345/tests/shell-wine.sh`（新、Wine・Xvfb・lavapipe） | **15 passed、0 failed、PASS**。busy・xhr・busy-cpu の各々で script の間に: Reload が 1 s で効く、Ctrl+L の about.html が開く、窓の thread の待ち longest 4.0 / 4.0 / 10.9 ms、over_20ms 0、over_100ms 0（約 2200〜2350 samples）。別の start で右の縁の drag（system の modal の size loop）: 898 幅の frame が script の間に出る（loop の中の画面 `build/review/ws345-p007/busy-resize-3-resizing.png` で 898 幅に描かれている）。その start の待ちは loop の長さの 1 回だけ（2978〜2984 ms、system の中）。画面: `build/review/ws345-p007/{busy,xhr,busy-cpu}{,-resize}-*.png` |
| `sh plan/ws334/tests/stage1.sh . plain 700`（front.c の直しの前） | host-build exit 0・warning 0・102、host-tests 82 exit 0・17 other、js 21/21、loader 11/11、http 15/15、http-async 19/20、fetch 38/38、component 83 checks PASS（p006 の値と同じ） |
| host-build plain・tsan（`BROWSER_HOST_BUILD=build/ws345-host`、main の merge の後） | 両方 exit 0、warning 0 |
| `sh plan/ws345/tests/host-threaded-view.sh plain`・`tsan`（main の merge の後） | plain 40 checks 0 failed。tsan 40 checks、ThreadSanitizer の報告 0、失敗 2 は p006 の記録と同じ tsan の遅さの時間の上限（(h) 1701 ms、caller の navigation-wait 708 ms）。新しい (q)（committed の callback が新しい頁の url・title を読む）は前の front.c で FAIL（url first.html、title First）、直しで PASS |
| host-terminate・host-loader-client・host-threads-table・`STALL_MODE=own stall-probe`（merge の前） | 39/39、23 checks 0 failed、16 checks 0 failed、窓の event の最長 0.1〜4.0 ms・over100 0 |
| `sh plan/ws334/tests/cross-build.sh` | exit 0、`cross-build: PASS`（win64・winarm64 とも: layer の warning 0、import が表の中、public の 63 関数と exports.map、遅延の vulkan-1.dll、libkeiland に pthread_ の名前なし） |

- 変えた試験の file: `plan/ws345/tests/host-threaded-view.c`（(q) と committed の callback）、新しい `plan/ws345/tests/shell-guest.sh`・`shell-wine.sh`・`pages/busy.html`・`pages/hung-xhr.html`。
- Win32 の modal の size loop の間は窓の thread が system の中にあるので、その間の tick は loop の長さ（約 3 s）待つ（system の物で browser の物ではない）。shell-wine はその start を別にし、待ちを summary に書くだけにした。

### 未実施・残り

- Linux の Keiland: browser の build が無い（`keiland-linux.mk` に libbrowser・browser が無い、2026-11-02 Q1 の判断で範囲の外）。shell の code は共通なので build があれば同じ道を通る。Future Work の案（Q1 へ）: 「Linux の Keiland の browser（libbrowser・libmedia・openssl・browser の `Makefile.linux` と WS105 の guest の試験）。WS345 の shell の `_OWN` の確かめもその時に」。
- 本物の Windows（modal の size loop の間の GPU の描画、DPI、IME）と実機は未実施（ユーザーが拾える試験の一覧に、Q1）。
- Win32 の CPU の道（--no-gpu）では modal の size loop の間に新しい大きさの frame は出ない（shm の present は窓の thread の libkeiland の呼び出しで、loop の間は system の中）。loop の後に出る。
- drag の画像・mail の sign-in code・touch の道は build と code の確かめだけで、guest の試験はしていない（AAT の dnd・mail の場面は未実施）。
