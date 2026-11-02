<!-- awesome-plan project=zedbsd record=ws345-p006 -->

# ws345-p006 描画の口

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

描画（GPU の記録と submit）を shell の描画の thread で行う口（D5、design.md §6）。

## file

- `include/browser/browser.h`: `browser_view_render`（target と command buffer、最新の確定を記録）、`browser_view_render_pixels`、`browser_view_presented`、`set_gpu`・`release_targets` を描画の thread の物に。
- `view/front.c`・`view/view.c`: frame の箱（frame の id ごとの最新の確定、lock と atomic の参照の数）、`frame_wake`、描画の機会（`presented` → script の thread）と確定の一つ先までの約束。
- `paint/paint.h`・`paint/list.c`: 確定した frame（`paint_frame` の参照の数を atomic に）、frame の item（iframe を id で引く）と iframe の枠の表（front の入力の routing 用、§5.3）、item が指す配列（`box->style.filters` など）の確定の arena への深い写し、face の表の写し（`text_font.face` → 共有の font の bytes）、sticky の位置を list に書く（確定の後に木を書かない、R5）。
- `page/frame-load.c`: iframe の文書を親の layout の中でなく、その frame の更新として別に layout し確定する（親は枠の大きさだけ）。
- `paint/vulkan.c`・`paint/software.c`: renderer の状態を描画の thread の物に、glyph の face を描画の thread が自分で開く（font の bytes の共有、R9）。
- `page/images.c`・`image/`: 解いた pixel を変えない参照の数の物に。
- `bind/timer.c`: `requestAnimationFrame` を描画の機会に結ぶ。

## 試験

- `plan/ws345/tests/host-threaded-view.c` に足す: 描画の代わりの thread（offscreen の lavapipe、`browser_offscreen_*`）が描いた画が、同じ頁の `_CALLER` の `draw_pixels` と画素で同じ（既存の gpu-compare の頁の部分集合）。rAF が present ごとに 1 回。描画の thread が止まると rAF も止まる。script が 5 s 回る間も描画の thread は最後の確定を描ける（resize の後も）。
- debug の build で確定した木の checksum が描画の後に変わらない。
- ThreadSanitizer の variant。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通り、GPU と CPU の画の比べが前と同じ。
