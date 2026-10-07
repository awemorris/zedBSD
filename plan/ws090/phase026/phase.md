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

## 層 3 の設計（compositor、2026-10-08 P1、design-reviewer の review に出す版）

### 今の形（読んだ所）

- `wayland/damage.c` の `kwl_damage_commit` は image が adopt される時に呼ばれる。
  - toplevel の窓（mapped、fullscreen でない、同じ大きさの image）で、glass の look なら `kwl_glass_body_damage`（look が still で、その窓より上の窓が body の DAMAGE_REACH = 96 px 以内に無い）が真の時に body の矩形だけを damage にする。plain の look は窓の矩形。
  - 他の全て（desktop の surface を含む）は `server->dirty`（出力の全体）。
- `compose.c` は damage を swapchain の image の buffer age で広げ、その中だけを描く。その矩形の中では全ての層（壁紙・desktop・窓・popup・bar・menu・OSK・通知など）を clip して描くので、重なる物は正しく描き直される。
- 矩形の外の見た目を変えうるのは、矩形の中の画素を読む効果だけ。
  - 窓の glass は既定で blur した壁紙（静的）を読む。`set_blur` の窓と docked の空間の中央の窓は backdrop（下の scene を 1/8 に縮めて blur した物）を読む。
  - 整列の menu も backdrop を読む（`kwl_arrange_showing`）。
  - bar・popup・通知・volume などは blur した壁紙の glass。
- client の damage は `protocol.c` の `add_damage`・`commit_damage` が `surface->committed_damage`（buffer の pixel、wl_shm の行の写し用）に集めている。GPU の image では消されずに積もる。

### 変更の案

1. `kwl_object` に `adopt_damage[4]`・`adopt_damaged` を足す。`commit_damage` が committed と同じく合わせ、`kwl_damage_commit` が使って消す。commit が adopt の前に重なっても（mailbox の置き換え）合わさる。
2. toplevel: 今の「alone」の条件のまま、body の矩形を `body ∩ (body の原点 + adopt_damage)` に絞る。条件: body の大きさが image の大きさと同じ（拡大・縮小・animation でない。`body_rect` が `surface->x,y` と image の大きさの時だけ）。違えば今の body の全体。
3. desktop の surface（`kwl_desktop_surface` が返す物、今は全体）: R = desktop の位置 ＋ adopt_damage（image に clip）。次が全て真の時だけ R を damage にし、どれかが偽なら今までどおり全体にする:
   - (a) `server->glass` で `kwl_glass_still` が真。
   - (b) image の大きさが前と同じ。
   - (c) 今の desktop の mapped で最小化されていない窓のうち、backdrop を読む物（`kwl_panels_blur` が真、または docked の空間の中央の窓）の body ＋ title bar が R ＋ DAMAGE_REACH にかからない。
   - (d) 整列の menu が出ていない。
   - (e) fullscreen の窓が出力を覆っていない（覆う時は desktop は描かれず、damage は要らない。全体にしてよい）。
   - plain の look（`!server->glass`）は R だけ（blur が無い）。
4. adopt_damage が無い commit（client が damage を送らない、または全体）は今までどおり。

### 危ない所（review で見てほしい点）

- client の damage が実際の変化を覆わない client（damage を小さく送る誤り）は描き残しになる。今の libkeiland と libvulkan の WSI は覆う（層 1・2 の host 試験）。古い client と wl_shm の client は全体か正しい damage を送る（wl_shm は既に damage の行だけを写している）。
- buffer age・head（mirror の写し）・capture は compose の既存の仕組みに乗る。
- 影: 窓の影は窓と一緒に clip の中で描かれる。desktop の変化は影の形を変えない。
- cursor・drag の icon・OSK・通知・corner は still でない時に全体になる（`kwl_glass_still`）。
## design-reviewer の review（2026-10-08、層 3 の設計）

高の 4 件:
- **H1**: glass の look では、desktop の上の pointer の移動が calm にならない（`window_at` は role の無い desktop の surface を除く）。範囲選択の最中は、毎回出力の全体が描き直される。
- **H2**: docked の空間の中央の窓の cover は、出力の全体で backdrop を読む。
- **H3**: 絞った damage が空になると frame が出ず、frame callback が返らないので FIFO の client が止まる。adopt は必ず frame を生む、を不変条件にする。
- **H4**: 描き残しを見つける試験（絞った frame と全体の frame の pixel の比較、絞りを切る switch）が無い。

中:
- **M1**: overflow と上下が逆の矩形。int64 にし、先に buffer に clip する。
- **M2**: viewport の付いた surface は全体にする。
- **M3**: attach だけで damage の無い commit は不明として全体にする。adopt_damage は adopt のたびに必ず消す。
- **M4**: apps bar の preview・switcher・整列の menu を still の条件に足す。
- **M5**: compositor の時間が支配的かを先に測る。

低: 書き方（docked の原点）、形式の変化、DAMAGE_REACH の根拠の comment、client 側の古い flag（L4）、既存の import 待ちと拡張の head の性能。

- 判断: L4 は層 1 の不具合なので直した。present が失敗した時は、名指した part を空の part で取り消す（`wayland_damage` は空の part で `damaged` を消す）。
- 層 3 は H1〜H4 を含めて作り直しが要る。案: 先に M5（compositor の KWL LAT・PERF の測定）を T1 で行い、層 3 の範囲を Q1 と決める。

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

## Q1 の判定（2026-10-08、T1-400）

層 1・2・4 の回帰: AAT の files・settings・notes・accent の 23 本で fail 0（pass 8・needs-person 14・not-run 1）、files-desktop-guest PASS、boot-test PASS。PNG（build/t1-400/aat-out/png/ の 69 枚）の全ての目視はまだ（Q1 は fail 0 と accent の log で判定）。
測定（M5）: band の間 `ZFILES DESKTOP band frames=4 mean_ms=93 longest_ms=112 draw_ms=10 present_ms=112`。前後の compositor の `KWL PERF`: compose draw_ms 97〜101（acquire 4〜13、submit+present 68〜73）、frame_ms 112〜113。つまり QEMU の Venus では compositor の 1 frame が約 100 ms で、app の present の待ちはそれに引きずられている（app の写しは 10 ms）。
判断（Q1）: 層 3 は今は進めない。理由: (1) H1 のとおり範囲選択の最中は pointer の移動ごとに出力の全体を描くので層 3 は効かない、(2) 測った約 100 ms は QEMU の Venus の submit+present（host との往復）が大半で、5330 の i915 の native の経路の数字ではない。5330 で `DESKTOP band frames=`・`KWL PERF` を測ってから、compositor の全体の再描画の速さ（Venus でない経路）を見て決める。
