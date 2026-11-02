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

## 引き継ぎ（2026-11-02 P8 q1984-i01、実装の前の checkpoint）

q1984-i01 の P8 は q1977 の後で文脈が残り少なく、この Phase の実装（多くの file にまたがる）を一つの起動で終えられないので、調べた事と手順をここに置いて返した。実装は同じ名前の新しい起動で続ける。この起動で commit した物: design.md に p005 の案（§6.1 の暫定の CPU の絵と値段、§9 の `vm_agent`・`net_loader_client_abort_waited`・`pagehide` の 1 s の deadline）を入れた（817f669dfb）。source は変えていない。

### 調べた事（88019c4e61）

- p005 の暫定: `view/front.c` の `front_publish`（1255 行の辺り）が `front->dirty` か大きさの変化で `front_paint` を呼び、script の thread で `browser_view_draw_pixels` の CPU の絵を作って `front->picture`（参照の数、`front->lock` の下）に置き、`frame_wake` を呼ぶ。`view_front_render_pixels`（923 行）はどの thread からでもその絵を写す（足りない所は白）。p006 はここを「確定した frame」に替える所。
- display list（`paint/paint.h`）の item が指す物:
  - glyph: list の `arena` の中（`struct paint_glyph`）。list と一緒に写せば済む。
  - `font`（`struct text_font`）: `face` は page の `text_system` の `faces[TEXT_FACES]` の番号。web font は `text_add_face`（`text/font.c`）が後から足す。face の bytes は `struct text_face.data`（`wb_buffer`、参照の数は無い）。→ 確定には「face の番号 → 変えない font の bytes の参照（参照の数を足す）」の表が要る。描画の thread は自分の `text_system`（または truetype の face の cache）をその bytes で開く（R9）。
  - `image`（`const struct img_bitmap *`）: page の画像（`page/images.c`）が持つ。`img_bitmap` に参照の数は無い。→ 画像の pixel を変えない参照の数の物にする（decode の後は書かない、vector の描き直しは新しい bitmap）。
  - `filters`（`const struct css_filter *`、`box->style` の物）: 確定の arena へ深く写す（`list.c` 3141 行で item に入る）。
  - iframe: 今は `page_frame_paint`（`page/frame-load.c` 1464 行）が親の layout の中で子の文書を layout し `paint_build` し（`frame_make`）、`struct paint_frame`（tree と list、`int refs`、atomic でない）を親の list の `frames` に入れ、親の item が子の list の中を指す（`list.c` 1752 行）。p006 の形（frame の item で id を引く、子は子の更新で確定）はこれを置き換える。
- sticky: `page_sticky_sync`（`page/page.c` 857 行）→ `layout_sticky_apply` が scroll の時に box を書き換え、`view/view.c` 3705・3734 行が呼ぶ。確定の後は木を書かないので、scroll の変化は新しい確定にする（R5）。
- rAF: `bind/timer.c` の `bind_request_animation_frame` は `window->now + TIMER_FRAME` の timer（`timer.animation = 1`）。描画の機会（`browser_view_presented`）に結ぶ所。
- GPU: `paint/vulkan.c`（2207 行）の renderer は今 view（script の thread、`_CALLER` では窓の thread）の物。`set_gpu`・`release_targets` は `_OWN` で何もしない（p005）。

### 手順の案（新しい起動で）

1. 確定した frame の型（`paint/paint.h`・新しい `paint/commit.c`）: `struct paint_commit`（atomic の参照の数、item と glyph と filter を自分の arena に写した list、face の表の写し、画像の参照、大きさ・scroll・canvas の色・通し番号、iframe は frame の item（frame の id・枠・clip））。`paint_commit_make(list, text, ...)`・`_retain`・`_release`。
2. 画像と font の bytes を変えない参照の数の物に（`image/`・`page/images.c`・`text/font.c`）。
3. front の frame の箱（`view/front.c`）: frame の id ごとの最新の確定（lock と atomic）、`frame_wake`、確定は present の一つ先まで。p005 の `front_picture` と `front_paint` を消し、`front_publish` で確定を作る。
4. `browser_view_render_pixels`: 最新の確定を、呼んだ thread（描画の thread）の CPU の renderer（`paint/software.c`、その thread の face の cache）で描く。`browser_view_presented` と描画の機会、rAF をそれに結ぶ（`bind/timer.c`、描画の thread が止まれば rAF も止まる）。
5. `browser_view_render`（GPU）: `paint/vulkan.c` の renderer の状態を描画の thread の物にし、確定から記録する。`set_gpu`・`release_targets` を描画の thread の口に。
6. iframe を子の更新で別に layout し確定（`page/frame-load.c`）、親は frame の item。sticky を list に書く。
7. 試験（上の「試験」）と回帰、zedBSD の amd64・arm64（config-rpi5）・riscv64（config-visionfive2-keiland）の libbrowser.so の build（2026-11-02 Q1 の指示）。

- 所有（2026-11-02 Q1）: p008（style の thread）は p006 の merge まで延期。p006 は `view/front.c`・`view/view.c`・`paint/` を調整なしに変えてよい。display list は layout のある所で作って変えない形で渡す（p008 の style の thread と合う、design.md §6）。p008 の引き継ぎの覚えは main の `plan/ws345/phase008/phase.md`。
