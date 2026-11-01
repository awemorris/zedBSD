<!-- awesome-plan project=zedbsd record=ws070-titlebar-design -->

# WS070 設計: Titlebar Presentation（`keiland_titlebar_v1`）

Parent: [WS070](ws.md) / Phase: ws070-p007
Status: 設計（2026-09-27、ws070-p007）。仕様案は [titlebar-spec.md](titlebar-spec.md)（ユーザー提供、§番号は仕様案の節）。既存の
System Menu は [design.md](design.md)（`xdg_toplevel_menu_v1`、p001〜p005）。仕様案と違える所・仕様案に無い所は各節に**決定**と
理由を書いた。人間の判断が要る点は §13 に置き、戻せる既定を選んで先へ進める。

## 0. 前提（再確認しない決定）

- タイトルバーは zdesktop が描く system-owned な面。client は意味（mode と model）だけを渡し、pixel・font・色・位置を指定しない
  （仕様案 §1、§24、§28）。client は zdesktop の非標準の拡張を **libkeiland の API だけ**で使う（WS070 の決定と同じ）。
- `MENU`・`CONTROLS`・`TABS` は排他（仕様案 §5）。MENU の model は既存の `xdg_menu_v1`（仕様案 §23）。
- 2026-09-27 ユーザー:「今のファイラーのはウィンドウ内部の上部にナビゲーションバーを持っていますが、これをウィンドウのフローティング
  タイトルバーにマージします。」「ファイラーはこのcompositorでしか使えなくてOKです。」→ **files の窓の中の toolbar
  （戻る・進む・Home・パンくず・検索・表示の切替・preview・進みの輪）は CONTROLS の model に移し、窓の中の bar は消す。
  files は fallback（仕様案 §27）を持たない**: 拡張が無ければ起動で `ZFILES FAILED operation=titlebar` を出して終わる。
- 既に zdesktop にある所（WS035 p059〜p071、WS070 p003）: 浮いたタイトルバー（高さ 44、本体との間 8）、システムバー（高さ 34）の
  zone（左から launcher と「zedBSD」＝System Identity、線、docked の窓の印・題名・menu＝Application Zone、窓の 3 つの button＝
  Window Management、仮想デスクトップ＝Workspace、signal・battery・時計＝System Status）、最大化で docked（本体が広がり題名の bar が
  上へ動いて消える 220 ms の animation）、docked の題名の double click・restore button・下への pull で戻る（仕様案 §17〜§20 の大部分）。
  この設計はそれらを保ち、「題名の後ろの menu の場所」を **Presentation の場所**に一般化する。

## 1. 全体の形

```
application（files 等）
   │  libkeiland: keiland_titlebar_*（mode・controls・tabs の鏡と局所の検査、transaction）
   ▼
libwayland-client: keiland_titlebar_manager_v1 / keiland_titlebar_v1（非公開 header）
   ▼
zdesktop
   protocol.c ─▶ titlebar.c（request、model、transaction、寿命、event）
   shell.c    ─▶ titlebar-shell.c（Presentation の配置・縮退・描画・hit・入力・text の欄・overflow の popup）
                   ├─ MENU     → menu-shell.c（既存の top-level の項目と popup）
                   ├─ CONTROLS → titlebar-shell.c
                   └─ TABS     → titlebar-shell.c
   glass.c    ─▶ role の icon を glyph atlas に（icons の rasterize）
```

## 2. Protocol（version 1）

**決定: 新しい global `keiland_titlebar_manager_v1`**（既存の `xdg_menu_manager_v1` の version を上げない）。理由: mode・controls・tabs は
menu と独立した model で、menu だけを使う client（terminal）は変えずに済む。名前は zedBSD 固有の拡張の慣例（`keiland_gpu_buffer_v1`）。

### 2.1 `keiland_titlebar_manager_v1`（global、version 1）

| opcode | request | 引数 |
| --- | --- | --- |
| 0 | `destroy` | — |
| 1 | `get_titlebar` | `new_id<keiland_titlebar_v1> id`、`object<xdg_toplevel> toplevel` |

error: `already_exists = 0`（その toplevel に生きた `keiland_titlebar_v1` がある）。

### 2.2 `keiland_titlebar_v1`（窓の titlebar の presentation）

request（変更は `begin_update`〜`commit` の間だけ。menu と同じ transaction の規則）:

| opcode | request | 引数 | 意味 |
| --- | --- | --- | --- |
| 0 | `destroy` | — | 窓は既定の見た目（MENU、menu があれば menu）に戻る |
| 1 | `begin_update` | `uint serial` | |
| 2 | `commit` | `uint serial` | 変更をまとめて表示（途中を見せない、仕様案 §22） |
| 3 | `set_mode` | `uint mode` | `menu = 0`（既定）、`controls = 1`、`tabs = 2` |
| 4 | `add_control` | `uint id`、`uint role`、`uint priority`、`uint group`、`string label` | 末尾に control を足す。**version 6**（2026-11-02、[BUG-396](../bugs/BUG-396.md)）: role `reload = 17` を受ける。version 6 未満の client の role 17 は `invalid_value` |
| 5 | `remove_control` | `uint id` | |
| 6 | `set_control_label` | `uint id`、`string label` | 表示と tooltip・overflow の行の名前 |
| 7 | `set_control_state` | `uint id`、`uint enabled`、`uint checked` | |
| 8 | `set_control_value` | `uint id`、`uint value` | PROGRESS の千分率（0〜1000、1001 は不定）。**version 5**（2026-10-14、[ws074-p229](../ws074/phase229/phase.md)）: BREADCRUMB にも使え、値 1（`KWL_BREADCRUMB_BOXED`）で編集していない時も角丸の箱で見せる（browser の Location）、0 で段の表示。version 5 未満の client の BREADCRUMB の値は受けない |
| 9 | `set_control_text` | `uint id`、`string text`、`string placeholder` | SEARCH の文字列と空のときの薄い文字 |
| 10 | `set_breadcrumb` | `uint id`、`array segments` | BREADCRUMB の段（NUL で区切った UTF-8 の列） |
| 11 | `add_tab` | `uint id`、`string title` | 末尾に tab |
| 12 | `remove_tab` | `uint id` | |
| 13 | `set_tab` | `uint id`、`string title`、`uint flags` | flags: `active = 1`、`attention = 2`、`closable = 4` |
| 14 | `set_tabs_options` | `uint options` | `new_tab_button = 1` |
| 15 | `focus_control` | `uint id`、`uint mode` | keyboard を control へ（`mode`: SEARCH は `focus = 0`、BREADCRUMB は `edit = 1` で path の欄に）。transaction の外で可 |
| 16 | `set_suggestions` | 候補の文字列の列（breadcrumb の path の候補、最大 12） | **version 4**（2026-10-05、[ws127-p010](../ws127/phase010/phase.md)）。compositor が欄の下に一覧を出す（全ての上、menu の popup の後に描く）。↑↓・Enter・click/tap で text を欄へ入れて `text_changed`、Esc はまず一覧を消す。transaction の外で可 |

event:

| opcode | event | 引数 | 意味 |
| --- | --- | --- | --- |
| 0 | `control_activated` | `uint id`、`uint detail`、`?object<wl_seat> seat`、`uint serial` | 押された（BREADCRUMB は `detail` = 段の番号、他は 0） |
| 1 | `text_changed` | `uint id`、`string text` | SEARCH・BREADCRUMB の欄の文字が変わった（打つたび） |
| 2 | `text_done` | `uint id`、`string text`、`uint how` | 欄が終わった: `submitted = 0`（Enter）、`cancelled = 1`（Esc）、`left = 2`（外の click で focus が去った） |
| 3 | `tab_activated` | `uint id`、`uint serial` | tab の click |
| 4 | `tab_close_requested` | `uint id` | tab の × |
| 5 | `new_tab_requested` | `uint serial` | ＋ |
| 6 | `overflow_menu_opened` | — | overflow の popup が開いた（client が menu を最新にする機会、menu の `opened` と同じ役） |

- **role**（仕様案 §8）: `back = 1`、`forward = 2`、`home = 3`、`up = 4`、`breadcrumb = 5`、`search = 6`、`view_grid = 7`、`view_list = 8`、
  `view_columns = 9`、`sort = 10`、`filter = 11`、`sidebar = 12`、`preview = 13`、`progress = 14`、`primary_action = 15`、`generic = 16`、
  `reload = 17`（version 6 から）。
  zdesktop は role で icon・大きさ・形（丸い button、欄、segment、輪）を決める。`generic` は label の text の pill。
  `reload` は page の再読み込みの四角い button（回る矢印の icon、`icons.c` の `GLASS_ICON_RELOAD`）で、back・forward・up・home と同じ左に並ぶ。
  libkeiland の `KL_CONTROL_RELOAD`（17、KL_VERSION 106）は、version 6 の compositor には 17 を、それより古い compositor には `generic` を label 付きで送る。
- **priority**（縮退の順、仕様案 §12）: `primary = 0`（戻る・進む等、最後まで残る）、`normal = 1`、`secondary = 2`（先に overflow へ）。
- **group**: 0 以外の同じ値の続く control は 1 つの segment の pill に並べる（`view_grid`・`view_list` の切替、checked が選ばれた面）。
- **overflow**（仕様案 §6 の common element の overflow button）: control ではなく zdesktop が CONTROLS・TABS の Presentation の右端に
  出す「…」。**決定**: その popup は (1) 幅が足りずに隠れた control の行、(2) separator、(3) 窓の menu（`xdg_toplevel_menu_v1` の model）
  の top-level を submenu の行として並べる。menu が無く隠れた control も無ければ出さない。理由: CONTROLS・TABS の窓も menu と
  shortcut を失わない（files の File・Edit・View・Go・Window・Help、p008）。§13-1 でユーザーに示す。
- **MENU との関係**（仕様案 §23）: menu の model は今まで通り `xdg_toplevel_menu_v1.set_menu` で窓に付ける。mode が `menu` なら
  top-level の項目を Presentation に並べ（今と同じ）、`controls`・`tabs` なら overflow の popup に入れる。**shortcut は mode によらず
  効く**（menu-shell.c の照合はそのまま）。`keiland_titlebar_v1` を持たない窓は `menu` mode（terminal は何も変えない）。
- **決定（仕様案 §21 の「不整合は error としてもよい」）: error にしない**。controls と tabs の model は mode によらず持て、表示は
  active な mode のものだけ。理由: client が次の mode の model を先に作り、1 つの transaction で `set_mode` だけ変えられる（§22 の
  atomic な切替が簡単になる）。
- **error**: `invalid_id = 0`（0、重複、無い ID）、`invalid_value = 1`（role・priority・mode・flags・focus の範囲外、BREADCRUMB 以外への
  `set_breadcrumb` 等の role の不一致）、`not_updating = 2`、`already_updating = 3`、`bad_serial = 4`、`too_large = 5`（control 64、
  tab 128、段 32、文字列 1023 byte を超える）。
- **文字列**: UTF-8。表示は glass の glyph（§8）。SEARCH の文字列の入力は zdesktop の US 配列（IME は無い、Future Work）。
- **serial**: `control_activated` 等の serial は menu の activation と同じく zdesktop がその入力に振った新しい値。

### 2.3 寿命

toplevel が先に消えると titlebar は不活性（`destroy` 以外は無視）。titlebar が消えると窓は `menu` mode（既定）。client の切断は
zdesktop の object の退場の hook（menu.c と同じ `zwl_object_destroy` の知らせ）で片付け、開いている popup・欄の focus を閉じる。

## 3. libwayland（client 側）

`userland/desktop/libwayland/titlebar-protocol.c`（新規: 2 interface の表、wrapper、listener の dispatch）と非公開 header
`zed-titlebar-v1-client-protocol.h`（install しない。`xdg-toplevel-menu-v1-client-protocol.h` と同じ扱い）。`exports.map` に
`zed_titlebar_*`。

## 4. libkeiland の API（`include/libc/keiland.h`、`KEILAND_VERSION` を 4 に）

```c
struct keiland_titlebar;
struct keiland_titlebar_listener {
	void (*control_activated)(void *data, struct keiland_titlebar *titlebar, uint32_t id, uint32_t detail, struct wl_seat *seat, uint32_t serial);
	void (*text_changed)(void *data, struct keiland_titlebar *titlebar, uint32_t id, const char *text);
	void (*text_done)(void *data, struct keiland_titlebar *titlebar, uint32_t id, const char *text, unsigned how);
	void (*tab_activated)(void *data, struct keiland_titlebar *titlebar, uint32_t id, uint32_t serial);
	void (*tab_close_requested)(void *data, struct keiland_titlebar *titlebar, uint32_t id);
	void (*new_tab_requested)(void *data, struct keiland_titlebar *titlebar, uint32_t serial);
	void (*overflow_menu_opened)(void *data, struct keiland_titlebar *titlebar);
};
struct keiland_titlebar *keiland_titlebar_create(struct wl_display *display, struct xdg_toplevel *toplevel,
	const struct keiland_titlebar_listener *listener, void *data);			/* 無ければ NULL・ENOTSUP */
void keiland_titlebar_destroy(struct keiland_titlebar *titlebar);
int keiland_titlebar_begin(struct keiland_titlebar *titlebar);
int keiland_titlebar_commit(struct keiland_titlebar *titlebar);
int keiland_titlebar_set_mode(struct keiland_titlebar *titlebar, unsigned mode);
int keiland_titlebar_add_control(struct keiland_titlebar *titlebar, uint32_t id, unsigned role, unsigned priority, unsigned group, const char *label);
int keiland_titlebar_remove_control / set_control_label / set_control_state / set_control_value / set_control_text(...);
int keiland_titlebar_set_breadcrumb(struct keiland_titlebar *titlebar, uint32_t id, const char *const *segments, size_t count);
int keiland_titlebar_add_tab / remove_tab / set_tab / set_tabs_options(...);
int keiland_titlebar_focus_control(struct keiland_titlebar *titlebar, uint32_t id, unsigned mode);
```

- 定数 `KEILAND_TITLEBAR_MODE_*`、`KEILAND_CONTROL_*`（role）、`KEILAND_PRIORITY_*`、`KEILAND_TAB_*`、`KEILAND_TEXT_*`（how）。
- menu の API と同じく、戻り値は 0 か errno、鏡で局所に検査して protocol error になる呼び出しを送らない（`EINVAL`・`EEXIST`・
  `ENOENT`・`EBUSY`・`E2BIG`）。service は display ごとに 1 つを内部に持つ（menu の service と同じ発見の queue）。

## 5. zdesktop の model（titlebar.c）

- object の種類 `ZWL_TITLEBAR_MANAGER`・`ZWL_TITLEBAR`、globals に `keiland_titlebar_manager_v1`、`zwl_object` の toplevel に
  `titlebar`（→ ZWL_TITLEBAR）。model は mode、control の配列（id、role、priority、group、label、enabled、checked、value、text、
  placeholder、段の列）、tab の配列（id、title、flags）、options。transaction は menu.c と同じ写しの差し替えと `generation`。
- log（試験が読む）: `ZWL TITLEBAR commit client=C surface=S mode=M controls=N tabs=T generation=G`、
  `ZWL TITLEBAR activate client=C id=I detail=D via=pointer|key`、`ZWL TITLEBAR text client=C id=I how=H length=L`、
  `ZWL TITLEBAR tab client=C id=I event=activated|close|new`。

## 6. 配置と縮退（titlebar-shell.c、仕様案 §3、§9、§10、§12、§16）

- 浮いたタイトルバー: `[印 題名] | Presentation | [— □ ×]`。Presentation の場所は題名の後ろ 18 px から button の前 12 px まで
  （今の menu の場所と同じ）。docked: システムバーの Application Zone（今の docked の題名と menu の場所）。どちらも同じ配置の関数を
  場所の幅で呼ぶ（client は場所を知らない、仕様案 §2.3、§15）。
- 題名の幅: MENU は今の規則（自分の幅と使える幅の 40% の小さい方）。CONTROLS・TABS は **決定: 題名を短く**（使える幅の 20% と
  自分の幅の小さい方、最小は印だけ）。理由: CONTROLS の BREADCRUMB が場所の名前を出すので、題名（アプリ名）は識別の印で足りる。
- **CONTROLS の並び**: 左に寄せる control（back・forward・up・home、BREADCRUMB）、右に寄せる control（search、segment、sidebar・
  preview・sort・filter、progress、primary_action、generic）、その間を BREADCRUMB が伸びて埋める。
  大きさ（pointer mode）: 丸い button 30×30、segment は 1 面 30、SEARCH の欄は 232（最小 140）、BREADCRUMB は段ごと（text + 16、
  区切りの › 18）。
- **縮退**（仕様案 §12 の順）: 入らないとき (1) BREADCRUMB の先頭の段を「…」に畳む（最後の段は残す）、(2) SEARCH を虫眼鏡の
  button に（押すと欄が Presentation の幅いっぱいに開き、他の control を覆う）、(3) `secondary` の control から後ろ向きに overflow へ、
  (4) `normal` を overflow へ、(5) `primary` だけ。題名は印だけまで縮む。TABS は (1) tab の幅を最小 96 まで縮める、(2) title を
  「…」で切る、(3) 端の矢印で scroll（見える tab を送る）、(4) 全部を overflow の popup の行に（tab overview の代わり）。
- 1 frame ごとに配置を計算し、hit の矩形を記録する（menu-shell.c と同じく描いた通りに当たる）。log:
  `ZWL TITLEBAR control client=C surface=S where=floating|docked id=I x=X y=Y width=W height=H shown=0|1`（配置が変わった frame で）。

## 7. 描画

- 見た目は menu の項目と同じ語彙: pointer の下は白い pill、押している間は濃い pill、checked（segment の選ばれた面、preview・
  sidebar の on）は白い knob と accent（`0x2f7cf6`）の icon、無効は薄い。SEARCH の欄は角丸の淡い地（`#f1f4f8`）に虫眼鏡と
  placeholder、focus の間は accent の縁と text の cursor・選択。BREADCRUMB は段の text（最後は太字で濃い）、区切りの ›、
  pointer の下の段は pill。PROGRESS は MODE_RING の輪（淡い全周と accent の割合）。TABS は pill の tab（active は白い面、他は
  淡い、attention は小さい accent の点、× は pointer の下か active のとき）と＋。
- **icon**（決定）: role の icon（‹ › ↑ ⌂ 虫眼鏡 ▦ ≡ ◫ 並べ替え 漏斗 ＋ ×、…）は zdesktop が起動時に小さな多角形の表（24 単位の
  格子の折れ線と面）から CPU で rasterize して glyph atlas に足す（`GLASS_*_GLYPH` の後ろの番号）。理由: font（Inter）に無い形が
  多く、icon theme も無い（WS070 §11-4 の「icon は後で」は menu の item の icon のこと。control は icon が形の本体）。
- 文字: glass の text（SIZE_BAR）。**非 ASCII は §8 の glyph cache が要る**（今の atlas は ASCII だけで、日本語の folder 名が
  パンくずに出ない）。

## 8. 非 ASCII の文字（glass の glyph cache）

- 今の glass の atlas は ASCII（と 3 つの記号）だけ。窓の題名・menu の label・パンくず・tab の題名の日本語が描けない（WS070 §11-5）。
  files の窓の中のパンくずは fallback font で日本語を描けていたので、CONTROLS へ移すと**後退**になる。
- **決定: CONTROLS へ移す前に glass に動的な glyph cache を足す**: `glass_draw_text` が UTF-8 を復号し、ASCII 以外の文字を
  (face, 大きさ, codepoint) の cache（atlas の空き領域の slot、LRU）で引き、無ければ libtruetype で描いて atlas の slot に upload。
  face は Inter の次に fallback font（`/usr/share/fonts/keiland-fallback.ttf`、無ければ □）。glass.c は WS035 の file なので、
  Phase を始める前に main に file の一覧を知らせて merge の順を決める。

## 9. 入力（仕様案 §19、§20、§26）

- **pointer**: control の press で pill を濃く、release で `control_activated`（同じ control の上のとき。menu の項目と違い press では
  活性化しない: button の標準の操作）。BREADCRUMB の段の click は `detail` = 段。SEARCH の欄の click で focus（keyboard が欄へ、
  `text_changed` を打つたび）。欄の外の press で focus が去る（`text_done how=left`）。overflow の「…」の press は popup（menu-shell.c
  の popup の仕組みに、隠れた control の行と menu の top-level を入れた合成の model を渡す）。TABS: tab の press で `tab_activated`、
  × で `tab_close_requested`、＋で `new_tab_requested`、tab の drag での並べ替えは Future Work。
- Presentation の中の control・tab の上の press は window の drag・double click（最大化・restore）にならない。control の無い地の
  部分（題名、BREADCRUMB の余白、TABS の strip の空き）は今まで通り drag と double click（仕様案 §19、§20 の対称は既存の実装）。
- **keyboard**: 欄に focus がある間、key は全部 zdesktop の欄へ（文字の挿入、Backspace・Delete、←→・Home・End、Shift で選択、
  Ctrl+A、Enter で `text_done submitted`、Esc で `text_done cancelled`、Tab で欄を出る）。menu の shortcut（Ctrl+… の照合）は欄の
  focus の間も先に照らす（Ctrl+W 等は効く）が、Ctrl+A・C・X・V・Z は欄が先に取る。F10 は MENU mode なら今の menu、CONTROLS・
  TABS なら overflow の popup を開く。client の `focus_control` は Ctrl+F（検索）と Ctrl+L（パンくずを path の欄に）に使う。
- **touch**（仕様案 §26）: zdesktop は `wl_touch` を持たないので pointer mode だけ作る。配置の定数を「大きさの組」（pointer・touch）の
  表にし、touch の組は値だけ置く（Future Work: 入力の装置に応じて切り替える）。
- **accessibility**（仕様案 §25）: model は zdesktop にあるので後で足せる。範囲外（screen reader が無い）。

- 2026-10-05（ws127-p010）: breadcrumb の欄に候補の一覧（request 16）が出ている間の keyboard の順: ↑↓ は候補の選択、Enter は選んだ候補を欄へ入れる（選んでいなければ欄の text で移動）、Esc はまず一覧を消し、2 回目で編集を終える。

## 10. docking と restore の animation（仕様案 §17、§18）

既存の animation（220 ms、本体の矩形と題名の bar が滑り、bar の glass が消える）を保ち、Presentation の中身も**同じ進みで**浮いた
bar の配置から docked の配置へ動かす: control ごとに両方の配置の矩形を計算し、進みで補間して描き、浮いた bar の glass が消える
のと同時に docked の地が濃くなる。幅が違って片方で隠れる control は進みの前半で薄く消え後半で現れる。restore は逆。

## 11. files の CONTROLS（WS071）

| control | role（priority） | 動作 |
| --- | --- | --- |
| Back・Forward | back・forward（primary） | 履歴。enabled は履歴に合わせる |
| Home | home（primary） | dashboard |
| パンくず | breadcrumb（normal） | 段の click でその folder、Ctrl+L で `focus_control(edit)` の path の欄、`text_done submitted` で移動 |
| 進みの輪 | progress（normal） | task があるときだけ（無いときは remove）、click で窓の中の task の一覧 |
| 検索 | search（normal） | `text_changed` を今の検索欄の入力として扱う（150 ms 後に検索）、Esc で元の場所、Ctrl+F で focus |
| Icons・List | view_grid・view_list（secondary、group 1） | checked は今の表示 |
| Preview | preview（secondary） | preview pane の出し入れ、checked |

- 窓の中の toolbar（design §3 の浮いた pill）と、その描画・hit・入力（`ui.c` の toolbar、`ui-field.c` の location と検索の欄の描画）は
  消す。content・sidebar・preview の panel は窓の上端の余白 12 から始まる。検索の scope の chip と task の一覧は窓の中のまま。
- 起動: `keiland_titlebar_create` が NULL なら `ZFILES FAILED operation=titlebar errno=…` で終わる（fallback なし、ユーザーの指示）。
- File・Edit・View・Go・Window・Help の menu（p008）は overflow の popup に入り、shortcut はそのまま効く。
- タブ（WS071 p013）は CONTROLS と排他の TABS には出さず、窓の中の tab bar（2 つ以上のとき）のまま。
- host の試験（`host-render`）は toolbar が無くなるので、titlebar の model（`fm_ui_titlebar_state` のような関数が作る control の列と
  状態）を text で出して確かめる。guest の試験（p002〜p008、p012）の toolbar の座標の click は、zdesktop の log
  （`ZWL TITLEBAR control ...`）から control の位置を引く形に直す。

## 12. 試験

- **titlebar-probe**（`userland/base/titlebar-probe`、menu-probe と同じ役の小さな client）: protocol の error（範囲外、transaction の外、
  重複）と libkeiland の局所の検査、mode の切替、controls・tabs の組み立て、event の log。
- **Venus（QEMU）**: WS070 の lean image に titlebar-probe と files。(1) CONTROLS の浮いた bar と docked の bar の画面、
  (2) 幅を縮めたときの縮退（パンくずの畳み、検索の icon 化、overflow）の画面、(3) control の click・パンくずの段・検索の入力の
  event、(4) overflow の popup（隠れた control と menu）、(5) TABS の strip（active・attention・×・＋・scroll）、(6) mode の切替が
  1 frame で変わる（途中の frame の log が無い）、(7) dock・restore の animation の途中の画面、(8) 日本語の題名とパンくず（glyph cache）。
- 回帰: WS070 の `menu-regress.sh`（terminal の MENU は変わらない）、WS071 の `files-regress.sh`（座標を log から引く形に直した後）、
  boot test。i915 実機は最後の Phase（任意）。
- 規約: 新しい file は `style-check.py` 0、既存の file（shell.c・menu-shell.c・glass.c・protocol.c・zwl.h、libwayland、libkeiland）は
  悪化させない（`plan/tools/titlebar/style-compare.sh`）。

## 13. 判断が要る点（既定で進める）

1. **CONTROLS・TABS の窓の menu の置き場**（**2026-09-27 ユーザー決定: A**。「CONTROLS / TABS モードのウィンドウのアプリメニューの置き場所は、Aの推奨でお願いします。」）: 仕様案は排他だけを決め、menu の行き先を決めていない。
   - A（既定）: Presentation の右端の overflow「…」の popup に menu の top-level を submenu として入れる（隠れた control の行も同じ
     popup）。shortcut は効く。Windows 11 の Explorer の「…」、GNOME Files の hamburger に近い。
   - B: 識別（印と題名）の click で menu の popup（macOS の app 名の menu に近い）。overflow は隠れた control だけ。
   - C: CONTROLS・TABS の窓は menu を出さない（shortcut だけ）。
2. **題名の幅**: CONTROLS・TABS では題名を短く（20%）。戻せる。
3. **不整合な model は error にしない**（§2.2）。仕様案が許す 2 つのうち client に易しい方。
4. **非 ASCII の glyph cache を CONTROLS への移行の前に**（§8）。WS035 の file（glass.c）に触れるので main と順を決める。
5. **touch と accessibility は範囲外**（zdesktop に `wl_touch`・screen reader が無い）。配置の定数は大きさの組の表にして後で足せる形。
6. **tab の drag での並べ替え**は Future Work（v1 は click・×・＋）。

## 14. Phase への分割

| Phase | 内容 | 主な file |
| --- | --- | --- |
| ws070-p008 | protocol と model: libwayland の `zed_titlebar_*`（表・wrapper・listener・非公開 header）、zdesktop の titlebar.c（request、model、transaction、error、寿命、log）、libkeiland の `keiland_titlebar_*`（鏡と検査）、titlebar-probe（error と検査の試験）。描画は変えない | libwayland/titlebar-protocol.c、zdesktop/titlebar.c・protocol.c・zwl.h・objects.c、libkeiland/titlebar.c、include/libc/keiland.h |
| ws070-p009 | glass の UTF-8 と動的 glyph cache（fallback font）と role の icon の rasterize（atlas に足す）。窓の題名・menu の label が日本語でも描ける | zdesktop/glass.c・compose.c・glass.h（**WS035 と調整**） |
| ws070-p010 | CONTROLS の presentation: 配置と縮退、描画（button・segment・欄・パンくず・輪）、pointer（click・hover）、SEARCH・BREADCRUMB の欄と keyboard、overflow の popup（隠れた control と menu）、docked の Application Zone、animation の補間、log。titlebar-probe の CONTROLS の場面 | zdesktop/titlebar-shell.c（新規）・shell.c・menu-shell.c・seat.c（**WS035 と調整**） |
| ws071-p014 | files: 窓の中の toolbar → CONTROLS の titlebar（toolbar の描画・入力を消す、control の model と event、Ctrl+F・Ctrl+L の focus、起動の失敗、host の試験の model の text、guest の試験の座標を log から） | files（ui.c・ui-input.c・ui-search.c・main.c・新規 titlebar.c）、plan/tools/files |
| ws070-p011 | TABS の presentation: strip、active・attention・×・＋、縮退（縮める、切る、scroll、overflow）、mode の atomic な切替の試験（titlebar-probe） | zdesktop/titlebar-shell.c |
| ws070-p006 | 既存: WS071 と共有する menu の file への規約の直し（p008〜p011 の変更の後に一緒に） | zdesktop/menu*.c 等 |
| ws070-p012 | 規約の全文との照合（titlebar の file 全部）、回帰（menu・files・zdesktop）、boot test、i915 実機（任意） | — |

順序（WS071 と合わせた計画）: ws070-p008 → ws070-p009 → ws070-p010 → ws071-p014 → ws071-p013（窓の中のタブ）→ ws071-p009
（context menu）→ ws071-p010（PNG のサムネイル・DnD）→ ws070-p011（TABS）→ ws070-p006 → ws071-p011 → ws070-p012。
理由: ユーザーの具体的な受け入れ（ファイラーの navigation bar のタイトルバーへの統合）を先に。TABS の最初の使い手は今は無い
（files は CONTROLS）ので、TABS は titlebar-probe で試し、後で terminal 等が使う。
