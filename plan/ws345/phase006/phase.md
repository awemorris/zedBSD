<!-- awesome-plan project=zedbsd record=ws345-p006 -->

# ws345-p006 描画の口

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: cleared（2026-11-02 P8 q1984-i01。結果は下。iframe の分離（file の `page/frame-load.c`）は p009 へ移す提案、Q1 の判断待ち）

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

## 結果（2026-11-02 P8、q1984-i01、基点 7cb56a9eb7）

commit: 7089de07b6（実装と試験）、3dc6d96dea（probe・design.md）、この記録の commit。

### 実装

- `paint/scene.c`（新）・`paint/paint.h`: `struct paint_scene`（atomic の参照の数）。`paint_scene_make` は頁の display list をそのまま取り（頁の list は `paint_list_init` で空に）、item の画像を list の arena に写した bitmap の記録にして pixel を `img_hold` で持ち、text system の face ごとの font の bytes（`text_bytes`）を持つ。`paint_frame` の参照の数を atomic に（scene が描画の thread で放す）。`paint/list.c`: backdrop・group の filter を build の時に list の arena へ写す（`url` は外す）。item は木を指さない。
- `image/`: `img_bitmap` に `hold`（pixel と atomic の参照の数、`img_bitmap_create` が作る）。`img_hold_retain`・`_release`、`img_bitmap_writable`（他が持つ pixel は写してから書く）。`page/media.c` は video の絵を描く前に `img_bitmap_writable`。
- `text/`: `text_face.data` を `struct text_bytes *bytes`（変えない bytes、atomic の参照の数）に。`text_face_bytes`・`text_bytes_release`・`text_system_use_face`（別の system の bytes で face を開き、変われば glyph の cache を捨てる）。
- `page/`: `page->paint_generation`（process で一意の list の番号）・`paint_taken`・`frames_presented`・`frames`。`page_set_frames_presented`・`page_frame_presented`・`page_frame_count`（`bind_host.frame_count`）。`page/frame-load.c` は `paint_frame` の参照を atomic に足すだけ。
- `bind/`: `bind_host.frame_count`（iframe の window にも写す）、`bind_timer.frame`。`requestAnimationFrame` は present を数える window では次の present を待ち（`timer_waits_frame`）、待つ間は次の timer の時刻に入らない。
- `view/front.h`・`front.c`: p005 の `front_picture`・`front_paint`・`view_front_render_pixels` を消し、`struct view_commit`（scene・scroll・大きさ・番号、atomic の参照の数）を front の `latest` に置く（`front_commit`）。`view_front_commit`（参照を足して返す）・`view_commit_release`・`view_front_presented`（`opportunity` を立てて script の thread を起こす。script の thread は次の round の前に `view_frame_opportunity`）。
- `view/view.c`: `view_commit_scene`（`view_update` の後、新しく paint した list なら新しい scene、そうでなければ前の scene。history から戻った頁の取られた list は描き直す）。描画の thread の状態（GPU の renderer・framebuffer・`render_text`・最後に描いた確定 `drawn`）を `render_lock` の下に。`browser_view_render`（新、確定を command buffer に記録し番号を返す）・`browser_view_presented`（新）・`browser_view_render_pixels`（確定を呼んだ thread の CPU と `render_text` で描く）。`set_gpu`・`release_targets` は `_OWN` でも働く（描画の thread の物）。`_CALLER` の道は変えていない（`set_gpu`・`release_targets` が lock を取るだけ）。shell の GPU の道も変えていない（`_OWN` を使う shell はまだ無い）。
- `include/browser/browser.h`: `browser_view_render`・`browser_view_presented` と描画の thread の側の説明。`BROWSER_API_VERSION` は 5 のまま（呼び出しを足しただけ、v5 は未公開）。
- design.md §6.1 に p006 の実装と入れなかった物を書いた。

### 入れなかった物（Q1 の判断を求める）

1. iframe を子の更新として別に layout・確定する事（手順 6、`page/frame-load.c`）は p009（iframe ごとの script の thread）へ移す提案。理由: 段 A では iframe の文書も top の script の thread にあり、今の形（子の list を親の list に写す）で確定は変えず自己完結（子の glyph・gradient は scene が持つ `paint_frame` の arena、画像は scene の hold）。frame の item と frame の id ごとの箱が要るのは iframe が別の thread で確定する時（p009）。
2. 「確定は present の一つ先まで」はしなかった: 新しい確定が未描画の確定を置き換える（描画の thread が止まっても script の thread は待たない）。rAF は present を待つので、描画が止まれば animation も止まる（試験 (j)）。
3. R5 の「確定した木の checksum」の debug の試験は作らなかった: item は木を指さない（filter は arena へ写し、画像は記録の写し、glyph は arena）ので、確定の後に `page_sticky_sync` などが木を書いても描画は変わらない。

### 試験

| 試験 | 結果 |
| --- | --- |
| `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`tsan` | 両方 exit 0、warning 0 |
| `sh plan/ws345/tests/host-threaded-view.sh plain` | 29 checks、0 failed。前からの 18 に加え: (i) 確定を描画の代わりの thread が描いた画が `_CALLER` の view の画と画素で同じ（CPU: `render_pixels` 対 `draw_pixels`、GPU: lavapipe の offscreen に `browser_view_render` で記録して submit 対 `browser_view_draw`）、頁は commit.html（web font・dashed の角丸の枠・gradient・PNG・filter・backdrop-filter・opacity・iframe）と ws074 の first・blocks・flex、8 件とも差 0 画素。(j) present の無い 600 ms は rAF 0 回、present 10 回で rAF 10 回（title "n10"）。(k) 5 s 回る script の間に 1000 × 700 で最後の確定を 4.7 ms で描き、頁の外は canvas の色（ff336699）。窓の呼び出しの最長は (a) 0.10 ms・0.05 ms、navigation-wait own 0.03 ms |
| 同 `tsan` | ThreadSanitizer の報告 0（lavapipe と llvmpipe の LLVM の中の報告は `plan/ws345/tests/tsan-lavapipe.supp` で外した。engine の code は外していない）。時間の上限の 2 つ（(h) 1711 ms、上限 1600。navigation-wait の caller 754 ms、上限 400）は tsan の遅さで超える（plain は 1060 ms・80 ms で通る。p005 の tsan でも caller は 740 ms） |
| `sh plan/ws345/tests/host-terminate.sh`・`host-loader-client.sh`・`host-threads-table.sh`（plain） | 39/39、23 checks 0 failed、16 checks 0 failed |
| `STALL_MODE=own sh plan/ws345/tests/stall-probe.sh`（6 s） | 窓の event の最長: idle 0.1、loop 0.1、handler 0.2、sync-xhr-connect 0.1、sync-xhr-read 0.1、docwrite-connect 0.1、async-connect 0.1、resize-big 0.1、decode-big 0.2 ms、over50 は全部 0。probe の own の mode は描画を probe の描画の thread（`frame_wake` で `render_pixels`）に移した（窓の thread で描くと resize-big 38 ms・decode-big 24 ms） |
| `raster-cost`（800 × 600、11 回の中央値、2 回） | bug411.html: 描くだけ 0.85〜0.90 ms（描画の thread）、resize の後の layout と描画 8.9 ms、layout と list だけ 11.8〜12.1 ms。6000 個の box の頁: 描くだけ 17.9〜18.1 ms（描画の thread に移った分）、layout と描画 332〜335 ms、layout と list だけ 241〜244 ms（script の thread の分）。§6.1 の p005 の値（16.6 ms・318 ms、bug411 0.83 ms・8.1 ms）と比べ、script の thread は描く分（6000 個で約 90 ms）を持たなくなった。bug411 で list だけが layout と描画より長いのは 2 回とも同じで、原因は未調査 |
| `sh plan/ws334/tests/stage1.sh TREE plain`（前 `git archive 7cb56a9eb7`、後 `git archive 7089de07b6`） | 前後とも host-build warning 0・102、host-tests 82 exit 0・17 other（host-exits.txt の diff 無し）、js 21/21、loader 11/11、http 15/15、http-async 19/20（前後同じ）、fetch 38/38、component 83 checks PASS |
| zedBSD の `libbrowser.so`（`make ZEDBSD_CONFIG=config/ci/config-amd64.mk`・`config-rpi5.mk`・`config-visionfive2-keiland.mk`、`BUILD=build/p006-{amd64,rpi5,vf2}`、target `…/dynamic/libbrowser.so`） | 3 つとも exit 0、warning 0（-Werror） |
| `sh plan/ws334/tests/cross-build.sh` | exit 1（`cross-build: FAIL`）。失敗は 2 行だけで、win64・winarm64 とも「libbrowser.a の `WebPDecodeBGRAInto`・`WebPGetInfo` が allowed-imports に無い」: 9424d21b0f（17:28、基点より前）が Windows の build に libwebp-compat を足したが、cross-build.sh の未定義の照合の archive の一覧（89〜90 行）に `libwebp-compat.a` が無い。`image/webp.c` は基点から変わっておらず、p006 と関係しない（WS334 の試験の直しが要る、Q1 へ）。他の項目は win64・winarm64 とも PASS（layer の warning 0、compat と libtruetype の warning 0、libmedia・libkeiland・libbrowser-ui の import、public の 63 関数と exports.map、遅延の vulkan-1.dll、zip、Wine の delay-load・試験の program）。winarm64 は時間切れにならなかった |

- 変えた試験の file: `plan/ws345/tests/host-threaded-view.c`・`.sh`（(i)〜(k)、窓の代わりは own の view を描かない、tsan の suppression）、`plan/ws345/tests/tsan-lavapipe.supp`（新）、`plan/ws345/tests/stall-probe.c`（own の描画の thread）、`plan/ws345/tests/raster-cost.c`（layout と list だけの値）。

### 未実施・残り

- shell（zedBSD・Linux の Keiland・Windows）が `_OWN` と `browser_view_render`・`_presented` を使うのは p007。QEMU・実機・Wine での `_OWN` の描画は未実施。
- 描画の thread の text system は view の font の path で開き直す（base の face の bytes は頁の物と共有しない、view あたり font の file の大きさだけ増える）。web font の bytes は共有する。
- 頁の list を scene に取るので、`_OWN` の script の thread が `page->paint` を読む所は無い前提（`_draw_pixels`・`_dump` などは `_OWN` で ENOTSUP）。p008 で display list を style の thread で作る時もこの取り方のまま渡せる。
