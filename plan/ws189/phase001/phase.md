<!-- awesome-plan project=zedbsd record=ws189-p001 -->
# ws189-p001: 設計 — app の間の drag and drop（画像・受け入れの見た目・dock・desktop・画面をまたぐ）

Status: in-progress（q892、P1、2026-10-08: 設計と design-reviewer の反映まで済み。clearance は Q1 の判定）
Disposition: normal
Parent: [WS189](../ws.md)
Queue: q892（P1 の新しい世代、2026-10-08 Q1 の依頼）

## 範囲

WS189 の決定（ws.md「決定」）を、libkeiland の口・compositor の振る舞い・app の受け渡し・試験に落とす。コードは書かない。
p002（libkeiland と compositor）と p003（app）の受け入れの条件をここで決める。design-reviewer を通す。

## 0. 今あるもの（2026-10-08 の読み、P1）

- compositor（`userland/desktop/wayland/data.c`）: wl_data_device_manager v3。start_drag はボタンか指が押されている時だけ。
  target は glass の look で titlebar の breadcrumb の部分 → 窓の body（`kwl_glass_body_at`、出力ごとの `window_at`）→ desktop の surface（`kwl_desktop_at`）。
  action は Ctrl でコピー・Alt で ask・target の好み・copy→move→ask の順。drag の icon の surface があればその角を pointer に、無ければ glass の look で「紙の badge」（`kwl_glass_draw_drag_badge`、shell.c）。
  Esc で取り消し。log `KWL DATA drag start|enter|accept|action|leave|drop|finish|cancel`。
- libkeiland（`libkeiland/ui/clipboard.c`、KL_VERSION 44・46）: `kl_window_accept_drops(KL_DROP_TEXT|KL_DROP_URIS)`、
  入力 `KL_WINDOW_DROP_ENTER/MOTION/LEAVE/DROP/ACTION`、`kl_window_answer_drop(actions, preferred)`、`kl_window_receive_drop`（上限 1 MiB）、
  `kl_window_finish_drop`、`kl_window_start_drag`（型 4 つまで、icon 無し）、`kl_window_drag_text`、`KL_WINDOW_DRAG_DONE`。
  受ける型の順は file 名（text/uri-list）→ 文字。自分の drag の drop は pipe を通さず直に読む。
- 使う app: Files（file 名の drag の元と受け、desktop も Files の `--desktop`）と Terminal（文字の元と受け）だけ。
- dock bar = system bar の app の icon（`apps-bar.c`、ws142-p004・ws113-p015、出力ごとの bar）。icon の上で 400 ms 止まると preview、
  click で `kwl_glass_switch_to`（最小化から戻し、最前面と focus）。drag and drop の間は `kwl_seat_motion` が data.c に渡すだけで bar は何も聞かない。
- 画面をまたぐ: pointer は論理の平面（`kwl_pointer_relative`）を出力をまたいで動き、`window_at` は pointer の出力の窓を探し、
  head の描画（heads.c）も `kwl_compose_cursor` で drag の icon・badge を描く。desktop の surface は anchor（主の出力）だけにある。

### 読みで見つかった既存の欠け（この WS で直す）

1. **同じ app の 2 つ目の窓が drop を受けられない**: `kl_app` の窓は 1 つの接続の上で窓ごとに wl_data_device を作る（window.c の
   `keiui_clipboard_start`）。compositor の `drag_device_of` は client の最初の device にだけ enter を送るので、2 つ目以降の窓の上の drag は
   最初の窓の listener が「別の surface」として断る（clipboard.c `clipboard_enter`）。Terminal・Notes の 2 つ目の窓が該当（Text Editor は 1 process に 1 窓、textedit/main.c:119）。
   加えて、同じ program の別の窓からの drag は、受ける窓が自分の program の source の send を pipe で待って 2 秒で空になる（review B1）→ §2.5。
2. **大きな data**: `CLIPBOARD_DROP_MAX` は 1 MiB。画像には足りない。
3. **SIGPIPE**: 元の app の `clipboard_send` は pipe へ write する。読み手が途中で閉じると SIGPIPE で元の app が終わる（Terminal だけが SIG_IGN）。
4. **wl_surface.attach の offset**: protocol.c は 0 以外の offset を EPROTO にする。drag の icon の hotspot を Wayland の決まり（attach の dx・dy）で渡せない。
5. **受け入れが変わっても画面が描き直されない**: `offer_accept`・`offer_set_actions` は `server->dirty` を立てない（pointer が止まっている間の答えが見えない）。

## 1. 原則（ws.md より）

- 受け渡しは Wayland の wl_data_source・wl_data_offer（型の一覧と pipe）。clipboard の中身は変えない。
- 型の一覧は drag が通る窓に見せる（wl_data_offer.offer）。**データは落とした窓にだけ渡す**: compositor は `receive` を
  `dnd_dropped` の offer（drop の後で finish の前）からだけ元へ回す（§3.6）。
- 同じ app の中の drag は app の中で処理する（下の §4.1 の「app の中の drag を外へ出す」）。Text Editor の自分の窓への drop は今は何もしない。
- X11 の app との橋渡し（XDND）は入れない。
- **Wayland からの意図した逸脱**（review I4）: Wayland は enter の後ならいつでも `receive` を許すが、この compositor は drop の後の offer にだけ data を回す
  （ユーザーの原則「データは落とした窓にだけ渡す」）。drop の前に中身を読む client（例: GTK4 の DropTarget の preload）は空を読む。zedBSD の app は drop の後にだけ読む。
- 規約は plan/coding-style.md の全文。compositor は libvulkan だけ・OS の操作は backend（Guardrail）。この WS は OS の操作を足さない。

## 2. libkeiland の口（KL_VERSION 70、仮。merge の時に Q1 が main の次の空きを確かめる）

### 2.1 画像の型

- `#define KL_DROP_IMAGE 4U` — "image/png"。`kl_window_accept_drops` が受ける。
- 受ける型の順: **file 名（URIS）→ 画像（IMAGE）→ 文字（TEXT）**。窓が受ける型と drag の型の積の中で最初の物を読む
  （`clipboard_drop_type`）。例: Photos の drag（uri-list と png）は Files・desktop・Mail には file として、Notes には画像として届く。
- `KL_WINDOW_DROP_ENTER` の code は今と同じく「drag が持ち、窓が受ける型」の bit。
- 上限: `kl_window_receive_drop` は型ごとに上限を持つ: 文字 16 MiB、file 名 1 MiB（今と同じ）、画像 64 MiB。越えたら E2BIG（今と同じ）。
- 元の側: 画像の drag は `kl_window_start_drag` に `{ "image/png", bytes, length }` を入れる。PNG を作るのは app（§4）。
  libkeiland は PNG の encoder を持たない。encoder は app の共有の新しい `userland/desktop/picture/png-write.c`（§4.0）。
- **data を後で渡す**（review I11）: `kl_window_drag_fill(window, type, data, length)`。`kl_drag_data` の data を NULL にした型は、drag を始めた後
  （ボタンが押されている間に start_drag が通る）に encode して埋める。compositor は drop の後にしか data を求めず、program はそれを次の dispatch で聞くので、
  dispatch に戻る前に埋めればよい。encode が遅くても drag の始まりは遅れない。

### 2.2 drag の icon

```c
/* A drag's picture (KL_VERSION 70): premultiplied 0xAARRGGBB pixels, no padding, and the point of it under the pointer. */
struct kl_drag_icon {
	const uint32_t *pixels;
	int width;
	int height;
	int hot_x;
	int hot_y;
};

int kl_window_start_drag_icon(struct kl_window *window, const struct kl_drag_data *data, size_t count, unsigned actions, uint32_t serial, const struct kl_drag_icon *icon);
```

- `kl_window_start_drag` は `icon == NULL` の `kl_window_start_drag_icon`（今と同じ動き）。
- libkeiland は icon の surface（role 無しの wl_surface）と wl_shm の buffer を作り、長い辺を **KL_DRAG_ICON_MAX = 160 px** に縮め
  （hot も同じ比で）、全体の不透明度を 0.85 にし、1 px の淡い縁（黒 25%）と角の丸め（6 px）を付けて、
  `wl_data_device_start_drag(..., icon, serial)` の**後に** `wl_surface_attach(icon, buffer, -hot_x, -hot_y)`・damage・commit（Wayland の決まり:
  icon の角は start_drag の時に pointer にあり、その後の attach の offset が動かす。review I1）。
  icon の surface と buffer は drag の終わり（`clipboard_drag_end`）で壊す。
- 返り値は `kl_window_start_drag` と同じ（EINVAL・ENOTSUP・EBUSY・ENOMEM）。icon が作れない時（shm 無し）は icon 無しで drag する（失敗にしない）。

### 2.3 受け入れの答え（既存の口の意味を決める）

- 新しい関数は足さない。`kl_window_answer_drop(window, actions, preferred)` を「この場所で受けるか」の答えにする:
  `actions == 0` は「ここでは受けない」（compositor は不可の印を出す）。app は `KL_WINDOW_DROP_ENTER` と `KL_WINDOW_DROP_MOTION` のたびに、
  pointer の場所で受けるかを答えてよい（同じ答えは libkeiland が送らない: 前の答えを `drop_answer_actions`・`_preferred` に持ち、変わった時だけ送る）。
- 答える前は今と同じく copy で受ける（受ける型があれば）。自分の drag にも自動で copy と答える（Terminal は自分への drop を貼り付ける、ws035-p093）。
  自分への drop を受けない app（Text Editor）は、自分の drag の間 `kl_window_accept_drops(window, 0)` にし（enter で即座に断られ、印は中立のまま。
  review I9 の「copy の印の一瞬の表示」を避ける）、`KL_WINDOW_DRAG_DONE` で元の型に戻す。

### 2.4 落とす場所を光らせる口

app は enter・motion・leave の入力で自分で描く。見た目を揃えるため、libkeiland の canvas に 2 つの描画を足す:

```c
/* KL_VERSION 70: a drop's place lit as every application lights it, in the theme's accent. */
void kl_drop_frame(struct kl_canvas *canvas, const struct kl_theme *theme, float x, float y, float width, float height, float radius);
void kl_drop_caret(struct kl_canvas *canvas, const struct kl_theme *theme, float x, float y, float height);
```

- `kl_drop_frame`: accent の 2 px の輪（不透明度 0.9）と、中の accent の 12% の塗り。画像の枠・file の置き場所・添付の欄に使う。
- `kl_drop_caret`: 文の挿入点。accent の 2 px 幅の縦の線と、その左右 3 px の accent の 25% の glow。文字の caret（点滅）とは別に、点滅しない。
- kl_canvas で描かない app（自前の pixel）は同じ色と寸法（`KL_DROP_RING`=2、`KL_DROP_FILL_ALPHA`=31/255、`KL_DROP_CARET`=2）で描く。定数は keiland.h に置く。

### 2.5 そのほか

- SIGPIPE: `clipboard_send` は write の間だけ SIGPIPE を SIG_IGN にして、終わったら前の処理に戻す（sigaction で保存・復元）。sigaction は process 全体に効くので、
  その間に別の thread（Photos の thumbs.c）が pipe へ書いて読み手が居なければ、その thread も SIGPIPE でなく EPIPE を受ける（害は無い）。primary.c は今回は変えない。
- 同じ program の別の窓の drag（review B1）: `clipboard_own_data` は窓自身か `window->app->windows` の中で drag している窓（drag は一度に 1 つ）の data を直に読む。
- 文字の型（review I3）: offer が UTF-8 を持てば "text/plain;charset=utf-8"、無ければ "text/plain" を読む（`drop_utf8`）。
- `kl_ui_pointer_cancel(ui)`（review I5）: press が drag になった時に kl_ui の widget の押下を click なしで忘れる（compositor が release を持っていくため）。
  app は start_drag が通った時と `KL_WINDOW_DRAG_DONE` で自分の press の状態も消す。
- 型の数: `KEIUI_DRAG_TYPES` を 4 から 6 に（画像・file 名・文字 2 つ・予備）。
- exports.map・exports.py に `kl_window_start_drag_icon`・`kl_drop_frame`・`kl_drop_caret` を足す。KL_VERSION の注に 70 を足す。

## 3. compositor（`userland/desktop/wayland/`）

### 3.1 受け入れの印（badge）

drag の間、compositor は「今 drop したら何が起きるか」を印で示す。印は drag の icon（または紙の badge）の右下、pointer から (+18, +18) を中心に、直径 18 px の円:

| 状態 | 条件 | 印 |
| --- | --- | --- |
| 中立 | target が無く、pointer が元の窓の上（drag の始まり）／target が元の surface で、まだ答えが無いか受けない | 印なし |
| コピー可 | target の offer が型を受け、action が COPY | 緑の円（#30B050、不透明度 0.95）に白い「+」（10×2 と 2×10） |
| 移動 | action が MOVE | 印なし（運ぶ物そのもの、macOS と同じ） |
| 選ぶ | action が ASK | accent の円に白い 3 つの点 |
| 不可 | 上のどれでもない（target が無い・型を受けない・action が NONE） | 赤の円（#D03030、0.95）に白い横棒（10×3）——進入禁止の形 |

- 判定は server を知らない純な `kwl_dnd_mark(&facts)`（新しい `dnd-state.c`、host 試験できる）で、data.c の `kwl_data_drag_mark(server)` が facts を集めて
  `server->dnd_state`（KWL_DND_STATE_*）に持つ。描画は shell.c の `kwl_glass_draw_drag_mark(server, command, state)`（紙の badge の後、icon の後）。
  icon がある時は icon を描いた後に印だけ、無い時は紙と印。glass の look だけ（plain の look は今と同じく icon だけ、印は無し）。
- 状態が変わったら描き直す: drag の motion・start・`offer_accept`・`offer_set_actions`・object の消滅で状態を比べ、変われば `server->dirty = 1` と
  log `KWL DATA drag state=<neutral|copy|move|ask|refused>`（AAT が読む。log は glass と plain の両方の look で出る。印を描くのは glass だけ）。
- spring-loaded の間（§3.3）は dock の icon の上なので「不可」の印を出さず中立にする（そこで drop しても何も起きないが、待てば窓が前に出るため）。

### 3.2 drag の icon の hotspot

- protocol.c の `surface_request` の attach: **今の drag の icon**（`dnd_active` かつ `surface == server->dnd_icon`）だけが 0 以外の offset を受け、
  `pending_dx`・`pending_dy` に足す。commit で `offset_x += pending_dx`、`offset_y += pending_dy`（Wayland の決まり: icon の位置は pointer + 累計の offset）。
  ほかの surface（窓・cursor・sub-surface・まだ role の無い surface）は今と同じく EPROTO（review I1）。start_drag で icon の累計と pending を 0 にする
  （start_drag の前の offset は数えない、使い回しの icon の古い offset も）。
- compose.c の `compose_cursor` は icon を `(pointer_x + offset_x, pointer_y + offset_y)` に描く（今の `pointer_x, pointer_y` から）。
- 損傷（damage.c）: drag の間は今と同じく全画面を描き直す（`dnd_active` で damage が全体になる、damage.c:59・187）ので、icon の位置の変更は追加の対処不要。

### 3.3 dock の spring-loaded

- drag の間、`kwl_data_drag_motion` は target を探す前に `kwl_apps_bar_drag_motion(server)`（apps-bar.c、新）を呼ぶ。pointer の出力の bar で、
  pointer の下の icon を見る。
- 状態（`struct kwl_apps_bar` に足す）: `spring_key`（icon の app の key）、`spring_since_ms`、`spring_tile`（preview の上の窓）、`spring_tile_since_ms`。
  - icon の上に入る: key を記録し時計を始める（icon を `draw_light` で 0.5 の強さで光らせ、時間とともに 1.0 へ）。別の icon へ動けばやり直し。
  - **SPRING_MS = 700 ms** 止まると（`kwl_apps_bar_tick`、drag の間も呼ばれる）:
    - 窓が 1 つの app: `kwl_glass_switch_to(server, window, "spring")`。log `KWL APPS spring app=KEY surface=S`。spring の状態を消す（同じ icon の上に居続けても繰り返さない。一度 icon を離れて戻ればもう一度）。
    - 窓が 2 つ以上: preview の panel を出す（`kwl_apps_bar_show(..., KWL_APPS_VIA_SPRING)`）。panel の preview の上で SPRING_MS 止まると、その窓を `switch_to` し panel を閉じる。log 同じ（surface は選んだ窓）。
  - icon と panel の外へ出ると（LEAVE_MS 300 ms）panel を閉じる。
  - 最小化だけの app（icon が淡い）も同じ（`switch_to` が最小化から戻す）。"+N" の場所は spring しない。
- drag が終わる（drop・取り消し・Esc）と spring の状態と VIA_SPRING の panel を消す（`drag_end` から `kwl_apps_bar_drag_end`）。
- 窓が前に出た後は、pointer は bar の上のままなので、使う人がその窓の上へ動かすと普通に target になる。
- bar の icon・spring の panel の上では target を作らない（`kwl_apps_bar_drag_motion` が 1 を返すと `drag_update` は surface を NULL にし、
  下の窓は enter を聞かない。drop すると取り消し、review I2）。印は §3.1 のとおり中立（`dnd_on_bar`）。
- spring で前に出た窓は focus を得るが、drag の間は `wl_pointer.enter` を送らない（`kwl_seat_pointer_update` は `dnd_active` の間何もしない。drag の終わりで pointer が戻る、review I2）。
- `kwl_apps_bar_tick` は IDLE の早い return の**前に** drag の間の spring の時計を進める。
- dock された窓（docked mode）の bar の題の上・全画面の窓（bar が無い）では spring しない（bar に icon が無い時は `kwl_apps_view_build_on` が 0）。

### 3.4 desktop への drop

- desktop の surface（Files の `--desktop`）は今と同じく、窓の無い所で target になる（`kwl_desktop_at`）。受けるか・何を作るかは Files（§4.6）。
- head（主でない出力）には desktop の surface が無い。head の窓の無い所は target 無し（印は不可）。

### 3.5 画面をまたぐ drag

- 今の作り（§0）で窓の target・icon と印の描画は出力をまたいで動く。p002 で足す・確かめること:
  - `drag_place` の座標: head の窓の body の原点（`kwl_glass_body_origin`）が平面の座標で正しく引かれることを host 試験か AAT で確かめる（違えば直す）。
  - spring-loaded は pointer の出力の bar（`server->pointer_output`）で動く（§3.3）。
  - log `KWL DATA drag enter ... devices=D output=N`（enter の行に device の数と target の出力を足す。今の行を読む試験・道具は無い、2026-10-08 grep）。
  - head の描画（heads.c `heads_shows`）: drag の間は全部の head が pointer の分を描く（icon は 160 px と印まで届き、`HEADS_CURSOR_REACH` 64 を越えるため、review I7）。
- 出力の端で pointer が止まる所（隣の出力が無い辺）は今と同じ。

### 3.6 同じ client の複数の data device（§0 の欠け 1）

- `drag_enter` は target の surface の client の**全部の生きた data device** に、device ごとに新しい offer（同じ型と action）と enter を送る。
  `struct kwl_server` に `dnd_offers[DND_DEVICES_MAX]`・`dnd_devices[...]`・`dnd_device_count`（DND_DEVICES_MAX = 16、越えた device には送らない）。
- motion・leave・drop も全部の device へ。libkeiland の窓は自分の surface でない enter の offer を `accept(NULL)` して壊す（今の動き）。
- 選び方（`drag_pick`、review I6）: `server->dnd_offer`・`dnd_target_device`（action・drop・leave の相手）は、型を受けた offer の最初の組。
  無ければ offer がまだある最初の組。source のある drag で offer が全部消えたら組は無し（drop しても取り消し）。source の無い drag は最初の device。
  accept・set_actions・offer や device の消滅（`drag_forget`）のたびに選び直す。`SOURCE_TARGET` は選ばれた offer の accept の時だけ送る
  （別の窓の accept(NULL) で source に「target 無し」を言わない）。`KWL_DND_DEVICES`（16）を越える device には enter を送らない。
- drop は選ばれた device にだけ送り、その offer だけが `dnd_dropped` になる。同じ client のほかの device は leave を聞く。
- receive の制限（データは落とした窓にだけ）: `offer_request` の receive は、offer が drag の物（`offer->dnd_offer`）の時、
  `offer->dnd_dropped`（drop を受け、finish・destroy の前）の時だけ元へ回す。drop の前の target・通っただけの窓の receive は descriptor を閉じる（読み手は空を読む）。
  log `KWL DATA receive ... refused=not-target`。clipboard の selection の offer（`dnd_offer == 0`）は今と同じ。
- `kwl_data_object_gone`: 壊れた device・offer を配列から外す。

## 4. app（p003）

（p003 の §4.0 `png-write.c` と試験 `plan/ws189/tests/host-png-write.c` は p002 と並べて先に書いた、2026-10-08。）

### 4.0 共有: PNG の書き出し

- 新しい `userland/desktop/picture/png-write.c`・`png-write.h`（libz-compat の compress2 だけに依る。picture.c の jpeg・gif の依存を持ち込まない）:
  `int kl_picture_png(const uint32_t *pixels, int width, int height, size_t stride, unsigned char **png, size_t *size);`
  入力は premultiplied の 0xAARRGGBB。全部の画素が不透明なら 8 bit の RGB（color type 2）、そうでなければ premultiply を戻した RGBA（type 6）。
  filter は行ごとに 0（None）、compress2 の Z_BEST_SPEED。返り値 0・EINVAL（大きさが 0 か 16384 を越える）・ENOMEM。
- 画像の drag の元は、長い辺を **2048 px** までに縮めた絵を PNG にする（drag の始まりで encode する。2048×1536 の RGB で数十 ms〜百数十 ms の見込み、p003 で測って記録）。
  元の file がある時は text/uri-list も一緒に出す（file として受ける所は元の file を得る）。
- drag の icon は元の絵の縮小（§2.2、libkeiland が 160 px に縮める）。hot は press の点の、絵の中の位置。

### 4.1 app の中の drag を外へ出す

- 既に app の中の drag がある所（Notes の画像の移動）は、app の中の drag のまま動く。pointer が**窓の外**へ出た時（compositor は押している間も
  focus の窓へ motion を送る、seat.c `kwl_seat_pointer_update`。窓の外の座標の motion か `KL_WINDOW_LEAVE`）に、app の中の drag を取り消して
  （絵は元の場所に戻る）`kl_window_start_drag_icon(..., kl_window_press_serial(kui))` で外の drag にする。compositor は buttons_down で始める（serial は押した時の物）。
- 外の drag が自分の窓へ戻って drop された時（`KL_WINDOW_DROP_ENTER` の pressed = 自分の drag）の扱いは app ごと（下）。

### 4.2 Text Editor（文字の元と受け）

- 元: 選択の中で左を押して **6 px** 動くと、選択の文字を `kl_window_drag_text`（COPY だけ）で drag する（Terminal の main.c:1921・2047 と同じ形）。
  drag の間は `kl_window_accept_drops(0)`（自分への drop は受けない、§2.3）、`KL_WINDOW_DRAG_DONE` で `KL_DROP_TEXT` に戻し、`drag_armed` を消す。
  選択の外の press と、動かずに離した press は今と同じ（選び直し・caret の移動）。
  `app_press`（app.c:992）の左の 1 click で、press の位置（`te_edit_position_at`）が選択の中なら `drag_armed` にし、選び直しを release まで遅らせる。
  motion で 6 px を越えたら drag、越えずに release したら今の 1 click の動き（caret をそこへ）。serial は `kl_window_press_serial`。
- 受け: `kl_window_accept_drops(KL_DROP_TEXT)`。drag の間、pointer の下の文字の位置（`te_edit_position_at`）に `kl_drop_caret` を描く
  （`te_app_caret_rect` の形の rectangle を位置から求める）。drop で `te_edit_select(pos,pos)` → `te_edit_insert_text`（1 回の undo）。
  読み取りの専用の file では `kl_window_answer_drop(0,0)`。
- 自分の drag（pressed=1）は `kl_window_answer_drop(0,0)`（今は移動を作らない、決定）。印は中立（§3.1）。
- 2 つ目の窓（`kl_app` の複数の窓）でも受ける（§3.6 の直しで）。

### 4.3 Notes（画像の元と受け）

- 元: Select の道具で画像を移動している間（`MAIN_DRAG_MOVE`、main.c:3379・3433）に §4.1 で外へ出す。data は image/png:
  `NOTES_IMAGE_PNG` で長い辺が 4096 以下かつ 64 MiB 未満なら元の bytes をそのまま、ほか（大きい PNG・JPEG・RGBA・PDF の行）は `notes_image_source` の RGBA を
  2048 px までに縮めて `kl_picture_png`（data は NULL で start し `kl_window_drag_fill` で埋める）。icon は描いた頁の上のその画像の見た目の縮小。
  actions は **COPY | MOVE**（ほかの窓は copy を好んで答え、Notes 自身は MOVE と答える。COPY だけだと自分への MOVE の答えと共有の action が無く取り消しになる、review B2）。
- 受け: `kl_window_accept_drops(KL_DROP_IMAGE)`。drag の間、落とすと置かれる枠（drop の点を中心に、`app_put_image` と同じ大きさの規則: 見えている頁の
  半分まで）を `kl_drop_frame` で描く。頁の外（頁の間の余白）では `kl_window_answer_drop(0,0)`。
  drop で `kl_window_receive_drop` → 新しい `notes_picture_load_bytes(document, bytes, size, &image)`（picture-file.c の picture_png・picture_jpeg を bytes から呼ぶ。
  `notes_picture_load` は file を読んでからこれを呼ぶ形に）→ `app_put_image` の置く処理を「中心の点」を引数に取る形に分け、drop の点に置く。1 回の undo。
- 自分の drag が自分の窓へ戻って drop: 画像を drop の点へ移す（app の中の移動の commit と同じ編集）。answer は MOVE。
- 文字の drop は受けない（Notes には自由な文字の選択が無く、文字は文字の box だけ。Future Work の候補）。

### 4.4 Photos（画像の元）

- grid の cell（`view_grid`、view.c:1256）と 1 枚の表示（`view_whole`、view.c:1436）で、左の press が **8 px** 動くと drag（今は press-drag の動きが無いので衝突しない。
  kl_ui の `KL_HIT_ACTIVE` と `kl_ui_drag_offset` で測る）。
- data: text/uri-list（`file://` と `ph_photo.path`）と image/png。drag は image/png の data を NULL にして**先に始め**、その後に grid は `ph_decode` で読んで
  2048 px に縮め、1 枚の表示は `view->picture`（既に 2048 px）から encode して `kl_window_drag_fill` で埋める（ボタンを早く離しても drag は始まっている、review I11）。
  actions は COPY だけ（Files の既定の move を避ける）。icon は thumb（`view->thumbs[i].image`、160 px）か、1 枚の表示の絵の縮小。
  drag が始まったら `kl_ui_pointer_cancel`（cell の押下が残って次の motion で drag をやり直さないように、review I5）。
- Photos は drop を受けない（取り込みは Import がある。Future Work の候補）。

### 4.5 PDF Viewer（画像と文字の元）

- 文字: 選択（`pv_select_*`、find.c）の中で press して 6 px 動くと、`pv_select_copy` の文字を `kl_window_drag_text`。選択の外は今と同じ（文字の上なら新しい選択、外なら pan）。
- 画像: 頁の画像の object（libpdf の `pdf_page_editor_hit`・`_object` で press の点の画像の quad）の上で **長押し 400 ms**（動きが 6 px 未満）の後に動くと drag。
  短い press と動きは今と同じ pan（全面の画像の scan の PDF でも pan が使えるように）。長押しが成り立った時に、その quad を描く枠を出して drag を待つ合図にする。
  data は image/png: quad の範囲を `pdf_display_list_rasterize` の offset で 2 倍（長い辺 2048 まで）に描いて `kl_picture_png`（重なる文字も入る。元の画像の bytes を抜く口は libpdf に無い）。
- PDF Viewer は drop を受けない。

### 4.6 desktop（Files の `--desktop`、受け）

- desktop の窓は `KL_DROP_URIS | KL_DROP_IMAGE | KL_DROP_TEXT` を受ける（folder の窓は今と同じ URIS だけ）。`drop_find`（ui-desktop-drag.c:434）の
  「file 名の無い drag は断る」を、desktop では画像・文字も通す形に。光らせ方は今の cell の縁（`DESKTOP_TARGET_*`）。folder の item の上の画像・文字は断る（answer 0）。
- 今の関門（review の minor）: Files の window.c:506 は `input->focused = 1` で drop の型を見ていない。`fm_dnd_receive`（dnd.c:132）は URIS 以外を断り、
  main.c:1055〜1067 は path の一覧を前提にする。p003 は DROP_ENTER の型を FM_EVENT に運び、desktop だけ画像・文字の受けの道を足す（folder の窓は今のまま）。
- drop: 画像は `~/Desktop/<Image>.png`、文字は `~/Desktop/<Text Clipping>.txt`（名前は `kl_tr` の訳、在れば「 2」「 3」…を付ける。
  文字は UTF-8 のまま、末尾の改行は付け足さない）。書いてから `fm_desktop_layout_set`（desktop-layout.c:448）で drop の cell に置く（`fm_desktop_dropped` の cell の選び方）。
  file 名（URIS）は今と同じ（copy・move の task）。
- 書けない時（容量・権限）は Files の今の error の通知と同じ扱い。log `ZFILES DESKTOP drop-file name=... bytes=N type=image|text`（接頭辞は Files の今の log に合わせる）。

### 4.7 Browser（画像の元）

- shell（`userland/desktop/browser/`）が、press の点が画像の要素の上で 8 px 動いたら drag。data は image/png（decode 済みの `img_bitmap` を 2048 px までに縮めて PNG、
  `kl_window_drag_fill`）と、画像の src の URL を "text/plain;charset=utf-8" と "text/plain" の両方で（review I3）。text/uri-list は出さない（http の URI を Files が file として扱わないため）。actions は COPY。icon は画像の縮小。
- libbrowser に公開の口を足す（2026-10-08 Q1: ユーザーは「libbrowser に口を足してよい」と決めている。B1 が動いていないので P1 が最小の差分で）:
  `int browser_view_image_at(struct browser_view *view, int x, int y, struct browser_image *image)` — view の座標の点の画像の要素の
  decode 済みの画素の複写（premultiplied の 0xAARRGGBB、`browser_image_release` で放す）・大きさ・src の URL（絶対の URL）。画像が無ければ ENOENT。
  内部は replaced の box を当てる探し方（`layout_hit` は文字だけに当たる、hit.c:28〜53。`layout_hit_node` か box の木の replaced の box の走査）と要素の src の属性。
  `img_bitmap` は straight の alpha なので、返す時に premultiply する。browser と pdfviewer は libz-compat を link に足す（png-write.c、3 つの Makefile と browser の build）。規則は `plan/standards/browser-component.md`（libbrowser は Wayland を使わない、
  shell が入力を渡す）。`BROWSER_API_VERSION` は上げない（ws074-p057 の規則「struct を変えるときは version を上げる」: `browser_view_options` の形は変えず、新しい関数と新しい struct `browser_image` を足すだけ。shell と library は同じ tree で build される）。
- リンクの上の press-drag と文字の選択は今と同じ。

### 4.8 Mail（添付の受け）— p004

- 読み: Mail の作成には添付が無かった（compose.c は単一の text の本文、view.c:1130「Attachments and drafts are not kept yet.」）。
- 決定（2026-10-08 ユーザー、Q1 経由）: **WS189 で添付も作る** → p004「Mail の作成の添付（multipart/mixed・base64、添付の一覧の UI）と drop の受け」。
- p004 の中身（p004 の着手の時に phase004/phase.md で詳しくする）:
  - `ml_compose` に添付の配列（名前・MIME の型・bytes、または file の path）。添付がある時は multipart/mixed（本文の text/plain と、各添付の base64、
    `Content-Disposition: attachment; filename=`、名前が ASCII でなければ RFC 2231 の `filename*=UTF-8''…`）。無い時は今と同じ単一の本文。
  - 作成の pane の添付の一覧（名前と大きさ、外す button）と「Attach…」（kl_file_chooser）。送信の大きさの上限（例 25 MiB）。
  - drop: 作成の pane が `KL_DROP_URIS | KL_DROP_IMAGE` を受ける。file は path を添付に、画像は `image.png`（在れば `image 2.png`）の添付に。
    drag の間は添付の欄を `kl_drop_frame` で光らせる。読む pane（受信の表示）では受けない（answer 0）。
  - 試験: 偽の IMAP・SMTP の server（`plan/tools/mail/fake-mail-server.py`）で送った multipart の構造の host 試験、AAT `apps.mailer.attach-drop`。

## 5. 試験

### 5.1 host（担当が流す、短い物）

- `plan/ws189/tests/host-png-write.c`: `kl_picture_png` の書き出しを libpng-compat で読み戻して画素が一致（不透明の RGB、半透明の RGBA、1×1、2048×1）。
- `plan/ws189/tests/host-dnd-state.c`: compositor の印の判定（`kwl_dnd_state_of(has_target, target_is_origin, accepted, action, spring)` を data.c から
  server を知らない純な関数に分ける）の表の全部の組。
- build（warning 0）: `$(BUILD)/bin/wayland`、`$(BUILD)/dynamic/libkeiland.so`、各 app の `$(BUILD)/bin/<app>`、Linux の keiland の build。

### 5.2 AAT（T1、draft で書き、p002・p003 の後に active）

`tests/scenarios/desktop/dnd/` に置く（areas: [compositor, dnd]、paths は data.c・clipboard.c・apps-bar.c と各 app）。

| id | 内容 | 合格（log と撮影） |
| --- | --- | --- |
| desktop.dnd.text-between-windows | Text Editor の選択を別の Text Editor（別 process）と Terminal へ drag | `KWL DATA drag state=copy`、`KWL DATA drag drop`、受けた窓に文字。撮影で挿入点の caret |
| desktop.dnd.same-program-windows | Terminal の 1 つ目の窓の選択を同じ process の 2 つ目の窓へ drag（§3.6・§2.5 の直し） | 2 つ目の窓の enter（`devices=2`）、drop、2 秒待たずに文字が貼られる |
| desktop.dnd.refused-mark | 文字の drag を Photos（受けない）の上で止める | `KWL DATA drag state=refused`、撮影で赤い印。離すと `drag cancel` |
| desktop.dnd.photo-to-notes | Photos の写真を Notes の頁へ | `state=copy`、Notes に画像（Notes の log）、撮影で枠の光と置かれた画像 |
| desktop.dnd.image-to-desktop | Notes の画像と Text Editor の文字を desktop へ | `~/Desktop/Image.png`（PNG の signature）と `Text Clipping.txt`（中身）、desktop の icon |
| desktop.dnd.dock-spring | Terminal の文字の drag を bar の Text Editor の icon の上で 1 秒止め、出た窓へ drop | `KWL APPS spring app=…`、Text Editor が最前面、bar の上では `state=neutral`、続く drop |
| desktop.dnd.across-displays | 2 つの出力で、主の出力の Text Editor から head の窓へ | `drag enter ... output=1`、drop。QEMU の 2 出力が runner で作れなければ `machine: hardware` |
| apps.mailer.attach-drop | Photos の写真と Notes の画像を Mail の作成の pane へ、送って偽の server で受ける | 添付の一覧に 2 つ、偽の server の受けた message が multipart/mixed で 2 つの添付 |
| apps.pdfviewer.drag-out | PDF Viewer の選択の文字と画像（長押し）を Text Editor・Notes へ | 各 drop の log |

- 自分の drag（Text Editor の自分の窓）: text-between-windows の中で、自分の窓へ戻すと `state=neutral` で drop しても何も起きないことも見る。
- 受け渡しの原則（review I4）: drop の前の receive を断ることは、enter で receive する小さな試験の client（既存の `userland/tests/data-probe` に p002 で drag の受けを足した:
  enter で receive して空を読むこと、compositor の log `refused=not-dropped`。AAT `desktop.dnd.early-receive`）で確かめる。zedBSD の app は drop の前に読まないので、app の AAT では見えない。
- across-displays は QEMU の 2 出力が runner で作れない時は p002 の clearance を塞がない（未実施として記録し、実機の UAT へ）。

## 6. Phase の受け入れ

### p002（libkeiland と compositor）

- §2（KL_DROP_IMAGE、型の順と上限、`kl_window_start_drag_icon`、`kl_window_drag_fill`、答えの重複の抑止、`kl_drop_frame`・`kl_drop_caret`、`kl_ui_pointer_cancel`、同じ program の窓の drag の直読み、文字の型、SIGPIPE、型の数、export、KL_VERSION 70 の注）。
- §3（印と状態の log、icon の hotspot、spring-loaded、画面をまたぐ enter の log、複数の data device と receive の制限、dirty）。
- §5.1 の host 試験のうち `host-dnd-state` の pass（`host-png-write` は p003）。build（wayland・libkeiland・Linux の keiland）warning 0。style-check の新しい指摘 0。
- data-probe の drag の受けと、§5.2 の text-between-windows・same-program-windows・refused-mark・dock-spring（・across-displays）の AAT を T1 に。
- 既存の Files・Terminal の drag and drop を壊さない（T1 の AAT で Files の drag を含む既存のシナリオがあれば一緒に流す）。

### p003（app）

- §4.0〜4.7（Text Editor、Notes、Photos、PDF Viewer、desktop、Browser と libbrowser の口）。
- 各 app の build warning 0、`kl_picture_png` の host 試験。
- §5.2 の AAT を T1 に依頼し、pass で active に。QEMU の撮影の PNG を Q1 へ。

### p004（Mail の添付と drop）

- §4.8。送った multipart の host 試験の pass、Mail の build warning 0、AAT `apps.mailer.attach-drop` を T1 に。依存は p002 と p003（試験の元が Photos・Notes）。

### 範囲の外（Future Work の候補）

- 画像の型の lazy な提供（drop の時に初めて encode する）。今は drag の始まりで encode する。
- image/jpeg の受け渡し（元の JPEG の bytes をそのまま）。今は PNG だけ（決定）。
- Notes の文字の drop、Photos の drop の受け、Files の folder の窓の画像・文字の drop、kl_ui の文字の widget（kl_field・kl_text_area）の一般の drop。
- Text Editor の自分の窓の中の移動（決定で「将来」）。

## 記録

- 2026-10-08 P1: design-reviewer（2 回目、1 回目は利用制限で中断）の指摘 B1・B2・I1〜I11・minor を反映した（各所に「review」と記した）。
  コードは p001 の review の前に p002 の一部を書き始めていた（Queue は p001〜p003 を承認済み）。review の指摘は書いたコードにも直した。
- 2026-10-08 Q1（ユーザーの決定の伝達）: Mail の添付は WS189 で作る（p004）。Browser の libbrowser の口は P1 が最小の差分で足してよい。
- 2026-10-08 P1: 既存の DnD（data.c・clipboard.c・apps-bar.c・heads.c・Files・Terminal）を読んだ。§0 の欠け 1〜5 を見つけた。
