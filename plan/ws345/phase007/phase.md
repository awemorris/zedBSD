<!-- awesome-plan project=zedbsd record=ws345-p007 -->

# ws345-p007 shell（三つの platform）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

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
