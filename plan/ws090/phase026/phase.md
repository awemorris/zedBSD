<!-- awesome-plan project=zedbsd record=ws090-p026 -->
# ws090-p026: 変わった所だけを present する（damage の present、BUG-221 の残り）

Parent: [WS090](../ws.md)
Status: in-progress（2026-10-08 P1 q879: 層 1・2・4 を実装・host 試験・build warning 0。層 3（compositor）は design-reviewer の review の後）
Disposition: normal
Queue: q879（P1、2026-10-08 Q1 の承認の 1 番）
Related: [BUG-221](../../bugs/BUG-221.md)・[ws090-p018](../phase018/phase.md)（部分の再描画、frame の描画の側）

## 由来

T1-385（2026-10-08）: desktop の範囲選択の frame で draw は 11 ms、present は 124 ms（`DESKTOP band frames=4 mean_ms=101`）。q866 で描画は変わった所だけにしたが、present は窓の全体のまま。
Q1（q879）:「libkeiland の present を、変わった所（damage）だけを写す・送る形にする…共通の library なので全ての app に効く。」

## 調べたこと（2026-10-08 P1）

- T1-385 の log の `DESKTOP select-frame` の内訳: copy 5 ms・acquire 0・vkQueuePresentKHR 40〜50 ms・wait 5〜7 ms。canvas への memcpy は小さく、時間の大半は WSI の present と compositor にある。
- libvulkan の Wayland の WSI（`wsi-wayland.c` の `wayland_commit`）は、毎回 `wl_surface_damage(0, 0, INT32_MAX, INT32_MAX)` を送る。FIFO は前の frame の frame callback を待つ。
- compositor の `kwl_damage_commit`（`wayland/damage.c`）は、toplevel の窓の image なら窓の body を、desktop の surface（toplevel でない）の image なら出力の全体を描き直す。client の damage（`surface->committed_damage`）は wl_shm の行の写しにしか使っていない。
- 判断: Phase は p018（描画の側の部分の再描画）の続きではなく、新しい Phase にした。present の経路（libvulkan・libkeiland・compositor）に跨り、Phase の受け入れ（T1 の present の測定）が別だから。

## 設計（4 層）

1. **libvulkan**（層 1）: 標準の `VK_KHR_incremental_present` を device の拡張として出す。`vkQueuePresentKHR` の `VkPresentRegionsKHR` から target ごとに layer 0 の矩形の外接の箱（image に clip）を job に持たせる。worker が commit の前に新しい platform の口 `damage` で lease に渡す。Wayland の commit は、その箱を `wl_surface_damage_buffer`（surface の version 4 以上、それ未満は `wl_surface_damage`）にし、無ければ今までどおり全体にする。direct display の platform は NULL（全体のまま）。header `include/libc/vulkan/vulkan_core.h` に `VK_KHR_incremental_present` の型（registry の ABI）を足した。
2. **libkeiland**（層 2、KL_VERSION 63）: `kl_window_present_part(window, pixels, stride, part)` を足した。`kl_window_present` は part が NULL のもの。
   - canvas への写しは part の行だけにする（新しい `ui/present-copy.c`、Vulkan に依らない）。canvas は前の frame を保つ。新しい canvas（resize の後）の最初は全体を写す。
   - device が拡張を持つ時だけ有効にし、present に region を付ける。swapchain の image には毎回 canvas の全体を描くので、compositor が受ける image は全体が正しく、damage は「前の image との違い」の意味になる。
   - shm の present は全体のまま。
3. **compositor**（層 3、未実装、design-reviewer の review の後）: client の damage で描き直しを絞る。
   - toplevel の body と desktop の surface を、client の damage の矩形（出力の座標）に絞る。
   - glass・blur・影・bar・OSK・通知の popup などが近ければ、今までどおり全体を描き直す。
4. **app**（層 4）:
   - Files: desktop の部分の再描画（変わった cell と band の新旧の矩形 ＋ margin）と窓の部分の再描画（`app->damage`）の外接の箱を `fm_app.frame_part` に集め、`fm_present_frame(..., part)` に渡す。部分で描いて何も変わらなかった frame は 1 pixel。
   - Mail・Calendar・Phone: hover の部分の再描画（`kl_ui_take_damage` の part）をそのまま渡す。

## 確かめ（2026-10-08、層 1・2・4）

- host:
  - `sh plan/ws090/tests/host-present-copy.sh`（新規）→ 5 checks、0 failures。2000 個の乱数の frame で、part だけの写しが全体の写しと byte で一致した（canvas の pitch は frame より広い）。clip の 4 件も通った。
  - `sh plan/ws094/tests/host-desktop.sh` → PASS。`partial_same` に「描画が変えた pixel は全て frame_part の中」の check（17 回）を足した。
  - `run-host-phone.sh` は前と同じく PASS。
- build（warning 0）: `make BUILD=build/bug221 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk` で libvulkan.so・libkeiland.so・files・mailer・calendar・phone・settings・notes。`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/bug221-linux all`（rc 0）。
- QEMU（T1、層 3 の後にまとめて頼む案）:
  - `plan/ws094/tests/files-desktop-guest.sh OUTDIR install show input` の `DESKTOP band frames=… present_ms=…` を T1-385（present_ms 124）と比べる。
  - 回帰: Files（files-regress）・Settings（settings-regress）・Notes。
- 層 1・2・4 だけでは compositor が desktop の commit ごとに全体を描くので、present_ms はあまり下がらない見込み（compositor の負荷は層 3 で下がる）。

## 積み残し

- 層 3（上）。
- swapchain の image への GPU の写し（WSI の `present_copy_shared`）は全体のまま。image ごとの age を数えれば部分にできる。
- shm の present の部分化。
