<!-- awesome-plan project=zedbsd record=ws190-p001 -->
# ws190-p001: 設計 — 文字の欄と Text Editor の指での選択（ダブルタップ・端の drag・編集の bar）

Status: in-progress（第 2 版: design-reviewer の review 1 を反映。§7 の Q1・Q2 は 2026-10-08 Q1 が回答。p002 は進めてよい（Q1））
Disposition: normal
Parent: [WS190](../ws.md)
Queue: q899（P1、2026-10-08 Q1 の依頼）

## 範囲

WS190 の要望と決定（ws.md）を、libkeiland の口・欄（kl_field・text area）の振る舞い・Text Editor・試験に落とす。コードは書かない。
p002（libkeiland の欄）と p003（Text Editor）の受け入れの条件をここで決める。

## 0. 今あるもの（2026-10-08 の読み、P1。review 1 の照合の後）

| # | 事実 | 所在 |
| --- | --- | --- |
| 1 | `kl_text_touch` は view の 3 つの答え（`position_at`・`caret_rect`・`word_at`、content の座標）で選択を持つ: 1 回の tap は caret、2 回は語を選び handle を出す、1 本指の drag は触れた所から選ぶ、handle の上で始まる drag はその端を動かす（anchor の handle は両端を入れ替えて caret の端として動かす）、指が離れると選択に handle が残る、端に近い指は content を自動で scroll（`kl_text_touch_edge`）、long press は `KL_TEXT_TOUCH_MENU`。handle の判定は knob の中心から半径 `KL_TEXT_HANDLE_REACH / 2` の円 | `libkeiland/ui/text-touch.c`、`include/keiland/keiland.h` 3066〜3143 行 |
| 2 | kl_ui は記録の種類 HIT・SCROLL・TEXT を持つ。指が触れた時に一番上の HIT（`touch_hit`）と一番上の SCROLL か TEXT（`touch_region`）を選ぶ（region の scroll に `kl_scroll_press`、NULL を確かめない）。TEXT の region の 1 本指の drag は `UI_DRAG_SELECT`、tap は HIT が無ければ `kl_text_touch_tap`、long press は TEXT の region なら touch の menu・他は app の `KL_EVENT_LONG_PRESS`、double tap は HIT が無く TEXT でなければ app の event。HIT の widget は drag を取らない（`KEIUI_DRAGGABLE` 以外）ので、scroll の中の欄の上の drag は頁を scroll する。mouse の press は `ui_focus_press` が HIT でない記録の上なら focus を外す。wheel と touch pad の scroll は `ui_find(…, 1)` の記録の scroll へ | `libkeiland/ui/ui.c` 486〜530・545〜640・641〜700・2095〜2290・2335〜2365 行 |
| 3 | 欄（`kl_field`）と text area（`kl_text_area`）は HIT の widget（`keiui_ui_widget(…, KEIUI_FOCUSABLE)`）。click か tap は caret を点に置き、2 回（`KL_HIT_DOUBLE`、`ui_click` が 400 ms で格上げ）は**全文**を選ぶ。kl_text_touch を使わず、handle も popup も無い。IME の preedit がある間は選択を描かず、text に preedit を挟んで割り付ける | `ui/field.c` 166〜223 行、`ui/text-area.c` 156〜227 行 |
| 4 | 欄の編集の命令（ws177-p013）: Ctrl+C・X・V・A・Z・Y を `keiui_ui_take_input` の key（`struct keiui_input`）として取り、`field_edit`／`area_edit` が clipboard（`keiui_edit_copy`・`keiui_edit_paste`。kl_ui が持つ窓と関数 pointer、`kl_ui_window_text` が `keiui_ui_set_window` で結ぶ。ui.c は host 試験のため window の code に link しない）と undo の履歴で行う。秘密の欄は copy・cut をしない | `ui/field.c` 572〜700 行、`ui/ui.c` 1561〜1670 行、`ui/internal.h` 87〜135 行 |
| 5 | 欄に読み取り専用の状態は無い。Text Editor にも無い | header 4008〜4038 行、textedit の grep |
| 6 | 貼れる物: `kl_window_can_paste(window)`。compositor の clipboard が無い時（`data_device == NULL`）は `kl_window_paste` が自分の copy を返すのに `can_paste` は 0（不整合） | `ui/clipboard.c` 206〜255 行 |
| 7 | Text Editor は `kl_ui_text_region(main_input, …, &main_app.touch)` を描画と別の入力だけの frame（`main_fingers`、dialog がある時は早く return）で記録し、handle は `main_frame` で描く。long press は compositor の context menu。編集は `te_app_action`。touch の選択を editor に戻すのは `te_app_event` の中だけ（条件 `touch.handles`）、`te_edit_select` は `kl_text_touch_set_selection` を呼ばない | `textedit/main.c` 700〜760・1800〜1850 行、`app.c` 300〜323・600〜630 行、`edit.c` 282〜304 行 |
| 8 | 画面 keyboard の inset: kl_ui の `ui_inset`（process に 1 つ、最後に聞いた窓の物）。窓ごとの値は `kl_window_keyboard_inset(window, &right, &bottom)` | `ui/ui.c` 285〜295 行、`ui/window.c` 2528〜2566 行 |
| 9 | widget を使う app は `kl_ui_begin` → widget を描く → `kl_ui_end` の順。例外: Settings の 1×1 の canvas の入力の frame（`settings/widgets.c` 737〜744 行）、Text Editor の描かない入力の frame（`main.c` 1823〜1833 行）、Notes の box（別の canvas で box の矩形だけを合成）。Mailer・Phone・Calendar は damage の clip を push したまま `kl_ui_end` を呼ぶことがある。`kl_canvas_clip_push` は今の clip との共通部分を取る | 各 app、`canvas.c` 119〜147 行 |
| 10 | 試験の注入: `/dev/input-inject` の touch screen（`INPUT_INJECT_KIND_TOUCH`）。**aat-input と aat の touch の命令は p002 の先行で足した**（`tap`・`double-tap`・`touch-drag`・`touch-down/move/up`、commit b7338ec54、Q1 が main に統合。Q1 の依頼「aat-input の touch」） | `userland/tests/aat-input/main.c`、`plan/tools/aat/` |
| 11 | kuidemo（試験の image だけ）の Controls の頁の Settings の card に名前の欄（初期値 "Kei"）と Password。**p002 の先行で Notes の text area（3 行）、`kl_ui_window_text`、`KUIDEMO SELECT id=name\|password\|notes anchor= caret=`・`KUIDEMO FIELD notes bytes= text=`（改行は `\|`）を足した**（commit 0ec34f32c、統合済み） | `userland/tests/kuidemo/main.c` |
| 12 | 翻訳: library の widget の文字は素の英語。`kl_tr_open`・`kl_tr_follow` を呼ぶのは Files・Settings・compositor だけで、共有の domain `keiland` は program の domain を開いた時だけ読まれる | `translate.c` 190〜205 行 |
| 13 | **窓に結ばれていない app**: Mailer（`mailer/main.c` 712 行）・Phone（595 行）・Calendar（616 行）は `kl_ui_window_text` を呼ばず `kl_ui_text_wanted` だけ（clipboard 無し）。Settings は touch を pointer に変える（`settings/window.c` 437〜450 行）。Files の改名の欄は touch を kl_ui に渡さない（`files/window.c` 389 行） | 各 app |
| 14 | double tap の時間: gesture は 300 ms（`gesture.c` 43 行）、HIT の widget は `ui_click` が 400 ms で 2 回目を格上げ | `gesture.c`、`ui.c` 37・2039〜2042 行 |

## 1. 振る舞い

### 1.1 指での選択の mode（欄・text area。Text Editor の違いは下）

| 操作 | 結果 |
| --- | --- |
| 文字の上の**ダブルタップ**（`KL_HIT_DOUBLE` かつ `KL_HIT_TOUCHED`） | その語を選び（§2.4 の語。語の文字でない所なら caret だけ）、選択の mode に入る。選択が空でなければ両端に handle。bar を出す |
| handle の 1 本指の drag | その端を動かす（今の kl_text_touch のまま、越えると両端が入れ替わる）。drag の間 bar は隠す。指を離すと bar を新しい位置に出す |
| handle の上の tap・double tap・long press | 何もしない（選択も bar も保つ）。app の event にもならない |
| handle の上で始まる 2 本指 | 下の region（頁の scroll）の物 |
| bar の button の tap（指か mouse の click） | §1.3。focus を奪わない |
| bar の上で始まる drag | 捨てる（選択も頁の scroll も始めない） |
| 選択の中の 1 回の tap | bar を出す（隠れていれば）・出ていれば隠す（他の OS の普通）。選択と handle は保つ |
| 選択の外の 1 回の tap（その欄の中） | caret をそこへ、mode を出る |
| 欄の外の tap・click（別の widget、何も無い所） | mode を出る（選択は欄に残る、handle と bar が消える）。別の欄なら focus が移る（今のまま） |
| key・画面 keyboard の文字・IME の commit・**IME の preedit の始まり** | 今の編集のまま行い（preedit は今の表示）、mode を出る |
| focus を失う（Tab、別の widget、`kl_ui_clear_focus`）、窓の keyboard の focus を失う、欄がその frame に描かれない、欄の text が kl_ui の写しと違う（app が `kl_field_set` などで変えた） | mode を出る |
| 欄の long press | 今のまま（app の `KL_EVENT_LONG_PRESS`）。mode に影響しない |
| 頁の scroll（欄の handle 以外の所の drag、wheel、touch pad） | 今のまま頁が scroll する。指の drag の間 bar は隠し、離したら出し直す。bar と handle は毎 frame 欄の今の位置から描くので scroll に付いて動く |
| mouse の click・double click | 変えない（double click は欄では全文）。bar は指の選択だけに出す |
| 欄の後に記録された modal の dialog、欄を覆う HIT の widget がある frame | bar と handle を描かず記録しない（§2.3） |

- 秘密の欄（`secret`）の「語」は全文。
- **Text Editor だけの違い**（今のまま保つ）: 1 本指の drag はどこからでも選択を始める（text view は頁の scroll の中に無い、2 本指で scroll）。指を離した時に選択が空でなければ bar を出す。long press は今の context menu を開き、bar を隠す。2 本指の scroll の間も bar は出したまま（公開の drag の口を足さない）。double tap は gesture の 300 ms（欄は `ui_click` の 400 ms、事実 14。揃えない。AAT は 120 ms で叩く）。

### 1.2 bar

- 横一列の button の帯（丸い角の pill、影付き）。button は左から **切り取り・コピー・貼り付け・すべて選択**（決定の 4 つ）。出す条件（Q1 の推しを採る）:

| button | 出す条件 |
| --- | --- |
| Cut | 選択が空でない、秘密でない、読み取り専用でない、clipboard の窓が結ばれている |
| Copy | 選択が空でない、秘密でない、clipboard の窓が結ばれている |
| Paste | 貼れる物がある、読み取り専用でない（秘密の欄でも出す: password manager の貼り付けを妨げない） |
| Select All | text が空でなく、選択がまだ全文でない |

  - 読み取り専用: 今は無い（事実 5）。印 `KL_TEXT_BAR_READ_ONLY` だけを用意する。
  - 窓が結ばれていない kl_ui は Cut・Copy・Paste を出さない（Select All だけ）。どの app で効くかは §1.5。
  - 出す button が 1 つも無い時は bar を出さない。
- 文字: "Cut"・"Copy"・"Paste"・"Select All"、**素の英語**（library の widget の今の決まり。翻訳は Future Work、§6）。
- 見た目: 地は `theme->panel`、縁は `theme->control_edge`、影は `kl_canvas_shadow`、文字は `theme->text`（14 px）、button の間に縦の細い線、mouse で押されている button（`KL_HIT_ACTIVE`）の地は `theme->selection`（指の tap は `active` を立てないので押されている見た目は出ない）。高さ 36 px、button の左右の余白 12 px。

### 1.3 bar の button を押した後

| button | 行い | その後 |
| --- | --- | --- |
| Copy | 選択を clipboard へ | 選択と handle は残る、bar は消える（選択の中の tap で出し直せる） |
| Cut | 選択を clipboard へ写して消す（undo の履歴に 1 つ） | caret だけ、mode を出る |
| Paste | clipboard の text を選択の代わりに（履歴に 1 つ、欄の limit まで） | caret は入れた text の後、mode を出る |
| Select All | 全文を選ぶ | 両端に handle、bar を新しい位置に出す |

### 1.4 bar の位置（純粋な関数 `kl_text_bar_layout`、host 試験で確かめる）

入力: 選択の矩形 `selection`（窓の座標。最初の行の上から最後の行の下まで、1 行なら左右は両端の x、複数行なら text の箱の左右）、見えている矩形 `visible`（欄の clip）、置ける範囲 `bounds`、bar の幅 `width`（cell の幅の和）と高さ 36。定数: 間 `GAP` 8、knob の下 `KNOB` = `KL_TEXT_HANDLE` 12。実装は if で書く（`?:`・`max`・`min` の macro を使わない）。

1. **見えている所**: `selection` を `visible` との共通部分にする。空なら bar を出さない（0 を返す）。`bounds` の高さが 36 + 2·GAP より小さい時も出さない。
2. **縦**: 上 `y = selection.y − GAP − 36`。`y < bounds.y` なら下 `y = selection.y + selection.height + KNOB + GAP`。下も `y + 36 > bounds.y + bounds.height` なら、選択の上の端の内側 `y = selection.y + GAP`（`bounds.y + GAP` より上なら `bounds.y + GAP`、`bounds` の下を越えるなら `bounds.y + bounds.height − 36 − GAP`）。この 3 段目では bar が handle を覆うことがあり、bar を handle の後に記録するので bar が勝つ（handle は bar の外の部分で掴める）。
3. **横**: `x = selection.x + selection.width / 2 − width / 2`、`[bounds.x + GAP, bounds.x + bounds.width − GAP − width]` に収める。幅が範囲より広い時は `x = bounds.x`（右が切れる）。
4. **`bounds`**: 描く canvas の全体から、画面 keyboard の inset（窓が結ばれていれば窓の `kl_window_keyboard_inset` を関数 pointer で、無ければ `ui_inset` を窓の大きさが canvas と同じ時だけ。reason が NONE なら 0）が覆う下と右を除いた矩形。app が `kl_ui_set_text_bar` で範囲を絞れる（§2.5）。

### 1.5 効く app（review 1 の M4）

| app | 欄・area の指の選択 | Cut・Copy・Paste |
| --- | --- | --- |
| kuidemo | 効く | 効く（事実 11） |
| Mailer・Phone・Calendar | 効く | **p002 で `kl_ui_window_text` に結ぶ**（今の `kl_ui_text_wanted` と `kl_window_text_input`・`kl_window_text_cursor` の行を置き換える、§7 Q1） |
| Text Editor（本文） | §3 | 効く |
| Text Editor の Find・Replace の欄 | 効く（main_input は窓に結ばれている） | 効く |
| file chooser・Files の改名の欄 | chooser は効く。Files の改名は touch を kl_ui に渡さないので効かない | — |
| Settings | touch を pointer に変えるので効かない | — |
| Notes の box | **使わない**（`kl_ui_set_text_bar(ui, NULL, 0)`、§2.5。box の外に描けないため） | — |

Settings・Files の touch の経路を kl_ui に渡す直しは範囲の外（§6、§7 Q2）。

## 2. libkeiland（p002）

### 2.1 kl_text_touch の追加（public、KL_VERSION 74 の仮）

- `struct kl_text_touch` の**末尾**に `int bar;`。header に「末尾の追加: KL_VERSION 74 より前の header で build した program の touch はこの field を持たない。そういう program は kl_ui_text_region に touch を渡す前に library の KL_VERSION を確かめる」と書く（今の使い手は Text Editor だけ、同じ tree）。
- 規則（text-touch.c）:
  - `kl_text_touch_tap(…, twice=0)`: `bar = 0`。`twice=1`: `bar = 1`（語が無くても）。
  - `kl_text_touch_drag_begin`: `bar = 0`。`kl_text_touch_drag_end`: 選択が空でなければ `bar = 1`、空なら 0。
  - `kl_text_touch_set_selection`・`kl_text_touch_long_press`: `bar = 0`。
  - 新 `kl_text_touch_select(touch, anchor, caret)`: 指の選択として置く（空でなければ handle、`bar = 1`）。
  - 新 `kl_text_touch_hide_bar(touch)`・`kl_text_touch_toggle_bar(touch)`（選択の中の tap）。
  - 内部の新 `keiui_text_touch_hold(touch, end, x, y)`（internal.h）: 端（anchor か caret）を指定して handle の drag を始める（円の判定をやり直さない）。
  - `bar` の変化は `changes` の新しい bit `KL_TEXT_TOUCH_BAR`（4U）。

### 2.2 bar の部品（public、新しい file `ui/text-bar.c`）

```c
#define KL_TEXT_BAR_CUT		1U
#define KL_TEXT_BAR_COPY	2U
#define KL_TEXT_BAR_PASTE	4U
#define KL_TEXT_BAR_SELECT_ALL	8U
#define KL_TEXT_BAR_BUTTONS	4U

/* What the bar is decided from (bits for kl_text_bar_buttons). */
#define KL_TEXT_BAR_SELECTED	1U
#define KL_TEXT_BAR_WHOLE	2U
#define KL_TEXT_BAR_EMPTY	4U
#define KL_TEXT_BAR_SECRET	8U
#define KL_TEXT_BAR_READ_ONLY	16U
#define KL_TEXT_BAR_CLIPBOARD	32U
#define KL_TEXT_BAR_CAN_PASTE	64U

struct kl_text_bar {
	unsigned buttons;
	struct kl_rect rect;
	size_t count;
	unsigned kinds[KL_TEXT_BAR_BUTTONS];
	struct kl_rect cells[KL_TEXT_BAR_BUTTONS];
};

unsigned kl_text_bar_buttons(unsigned facts);
int kl_text_bar_layout(struct kl_text_bar *bar, struct kl_text *text, unsigned buttons, const struct kl_rect *selection, const struct kl_rect *visible, const struct kl_rect *bounds);
unsigned kl_text_bar_hit(struct kl_ui *ui, uint32_t id, const struct kl_text_bar *bar, unsigned *held);
void kl_text_bar_draw(const struct kl_text_bar *bar, const struct kl_style *style, unsigned held);
```

- `kl_text_bar_hit`: 各 cell を HIT で、新しい内部の flag `KEIUI_KEEP_FOCUS`（`ui_focus_press` が focus を外さない）と `KEIUI_NO_DRAG`（`ui_gesture` の DRAG_BEGIN が `touch_hit` にこの flag を見たら drag を捨てる。下の TEXT の region を SELECT にしない）で記録する。前の frame からの click の button（1 つ）を返し、`*held` は mouse で押されている button。
- id: app は自分の id と重ならない id（Text Editor は `MAIN_TEXT_BAR`）。library の欄は予約の `KEIUI_TEXT_BAR_ID` 0xfffffffdU、handle は `KEIUI_TEXT_HANDLE_ID` 0xfffffffcU（`KEIUI_ANY` 0xfffffffeU の隣。header に「app は 0xfffffff0U 以上を使わない」）。

### 2.3 欄と text area の選択（kl_ui の中、内部、新しい file `ui/text-select.c`）

**状態の置き場所と寿命**（review 1 の B2・M1）: `struct kl_field`・`struct kl_text_area` は変えない。kl_ui が 1 つの `struct ui_select` を `kl_ui_create` から `kl_ui_destroy` まで持つ。**view の答えは kl_ui の写しだけを使い、app の memory を frame の間に触らない。**

| field | 意味・寿命 |
| --- | --- |
| `active` | mode の中か |
| `owner`・`widget` | 欄の id と index、欄の struct の address（照合だけに使い、frame の間に deref しない） |
| `kind` | 欄か text area か |
| `touch` | `struct kl_text_touch`（§2.1） |
| `view` | kind ごとの `kl_text_view`（static const、text-select.c） |
| `scroll` | `struct kl_scroll`。`kl_ui_create` で `kl_scroll_init`（失敗は kl_ui_create の失敗）、`kl_ui_destroy` で release。mode の終わりでは release しない（前の frame の HANDLE の記録と `touch_region` が指す） |
| `box`・`clip`・`height` | text の箱（窓の座標）、欄が描かれた時の canvas の clip、欄の高さ |
| `text`・`length`・`secret` | 欄の text の写し（表示の text: 秘密は点、preedit を除く）と長さ、秘密か。text area は最大 `KL_TEXT_AREA_MAX` |
| `font`・`style` | 測る font（`style->text`）と、描く `struct kl_style` の値の写し |
| `layout` | text area の行の割り付け（写しから作る。`kl_ui_create` で malloc、失敗なら text area は mode に入らない） |
| `drawn`・`order` | この frame に owner が登録したか、その記録の番号（後から描かれた物を見る、M3） |
| `bar` | 今の frame の `struct kl_text_bar` |

**欄の 1 frame**（field.c・text-area.c、選択を描く前に）:

1. `KL_HIT_CLICKED` かつ `KL_HIT_TOUCHED`: `KL_HIT_DOUBLE` なら `keiui_select_begin`（写しを作り、`kl_text_touch_tap(…, 1)`）。1 回なら、mode の中で選択の中の tap は `kl_text_touch_toggle_bar`、外は `kl_text_touch_tap(…, 0)` と mode を出る。mouse の click は今のまま、mode の中なら出る。
2. key・文字・commit・preedit を取った: mode を出る。ただし bar の命令（`keiui_input.from_bar`）は §1.3 の「その後」に従う。
3. mode の中なら毎 frame `keiui_select_frame(ui, id, widget, box, clip, style, text, length, secret, scroll)`:
   - 写しと text・length が違う、または `widget` が違う → mode を出る（app が text を変えた・別の欄）。touch の anchor・caret は length で clamp。
   - drag の間（`touch.selecting`）は kl_ui の `scroll` の位置を欄が受け取り、欄の「caret を見える所に」の規則をその frame は止める（`select.scroll` と描画の位置をずらさない）。drag でなければ欄の scroll を kl_ui の `scroll` に写す。
   - touch の `KL_TEXT_TOUCH_SELECTION` を欄の `caret`・`anchor` に移す（選択を描く前）。

**kl_ui の中の流れ**:

- **handle の記録**（`kl_ui_end` の始め、frame の入れ替えの前）: mode の中で `drawn` のとき、各端の knob の中心の周り `KL_TEXT_HANDLE_REACH` の四角を、`clip` ∩（`box` を左右と下に `KL_TEXT_HANDLE` 広げた矩形）で切って、新しい記録の種類 `UI_KIND_HANDLE`（id `KEIUI_TEXT_HANDLE_ID`、index は端: anchor 0・caret 1、`box`・`scroll`・`touch` を持つ）で記録する。空なら記録しない。owner の後に `KEIUI_MODAL` の記録か owner の矩形と交わる HIT の記録がある frame は、handle も bar も記録・描画しない（M3）。
- **`ui_find` と HANDLE**（M2）: pointer・wheel・axis の `ui_find` は HANDLE を飛ばして下の記録へ。指の down は、一番上が HANDLE で knob の中心から半径 `REACH / 2` の中なら `touch_hit` 無し・`touch_region` は HANDLE（`ui_content` が使う矩形は記録の `box`）、加えて HANDLE を飛ばした下の region を `touch_below` に覚える。円の外なら HANDLE を飛ばす。
- **`ui_gesture` と HANDLE**: TAP・DOUBLE_TAP・LONG_PRESS は何もしない。DRAG_BEGIN は 1 本指なら `UI_DRAG_SELECT` で `keiui_text_touch_hold(touch, 記録の端, x, y)`、2 本指以上なら `touch_below` の scroll（`UI_DRAG_SCROLL`）。CANCEL は今の SELECT と同じ。
- **mode を出る他の時**: 欄の外の tap（HIT が owner でも bar でもなく region が HANDLE でない）、focus が owner から移った、窓の keyboard の focus を失った（`KL_WINDOW_FOCUS` の偽を `kl_ui_window_input` が見る）、`kl_ui_end` で `drawn == 0`。
- **bar の描画と記録**（`kl_ui_end` の始め、HANDLE の後）: mode の中で `drawn`、`touch.bar`、`!touch.selecting`、`ui->drag` が `UI_DRAG_SCROLL` でない時:
  1. facts（選択の有無・全文か・空か・秘密、窓が結ばれていれば `KL_TEXT_BAR_CLIPBOARD`、`can_paste` の関数 pointer の答えで `CAN_PASTE`）→ `kl_text_bar_buttons`。
  2. `selection`（写しの view の `caret_rect` の両端から窓の座標）、`visible` = `clip`、`bounds`（§1.4 の 4）→ `kl_text_bar_layout`。
  3. `kl_text_bar_hit(ui, KEIUI_TEXT_BAR_ID, …)`（HANDLE の後に記録するので bar が上）。
  4. 写しの `style` の canvas に、**`kl_ui_end` の時の clip のまま** `kl_text_bar_draw` と `kl_text_touch_draw_handles`（handle は `clip` ∩ 広げた箱を push）。
- **押された button**（B1）: `kl_ui_end` の key の loop（app に渡す未取得の key を選ぶ所）の**後で**、owner への key を `ui->keys` に積む（`target = owner`、`from_bar = 1`）: Cut = Ctrl+X、Copy = Ctrl+C、Paste = Ctrl+V、Select All = Ctrl+A。`moving = 1` を返す。`UI_KEYS` が満杯なら捨てる。`from_bar` の key は、次の frame で owner が描かれず取られなくても app に渡さず捨てる。`struct keiui_input` に `from_bar` を足し、`keiui_ui_take_input` が写す。欄は今の `field_edit`・`area_edit`・Ctrl+A の処理で行い、`from_bar` を見て §1.3 の「その後」にする（Copy → `kl_text_touch_hide_bar`、Cut・Paste → mode を出る、Select All → `kl_text_touch_select(0, length)`）。
- **描画の約束**（header の kl_ui の説明に足す）: 「指の選択の mode の欄があれば、`kl_ui_end` はその欄を描いた canvas に、その時の clip の中で handle と bar を描く。app が狭い damage の clip を push したまま `kl_ui_end` を呼ぶと bar が欠ける（Mailer などの部分の描き直しは lit の変化だけの frame で、mode の変化は全体の frame になる）。`kl_ui_end` の後に描く物は bar の上に来る」。
- **関数 pointer**（minor 2・9）: `keiui_ui_set_window` に `can_paste` と `keyboard_inset` の関数 pointer を足す（ui.c は window の code に link しない、ws177-p013 の決まり）。

### 2.4 view の 3 つの答え（写しから）

- 欄（content の x は text の始めから、y は箱の上から）: `position_at` は今の `field_at` と同じ測り方を写しの表示の text と font で。`caret_rect` は x = その位置までの幅、y = 8、幅 2、高さ = `height − 16`。
- **語**（`word_at`、M7。`keiui_edit_word` を使わず独立に定義）: 位置の右の 1 文字（無ければ左）が語の文字なら、同じ種類の文字が続く間を前後に伸ばす。語の文字でなければ start = end（caret だけ）。語の文字: ASCII の英数字と `_`、ASCII 以外の文字のうち区切り（U+3000〜U+303F の CJK の記号と句読点、U+FF01〜U+FF0F・U+FF1A〜U+FF20 の全角の記号）でない物。種類は ASCII の語と ASCII 以外の語の 2 つ（"abc日本" は "abc" と "日本" に分かれる）。秘密は全文。空白の無い日本語の文は句読点までが 1 語（制限、§6）。
- text area: 写しの割り付けで、`position_at` は行（y / AREA_LINE）と `area_at_x`、`caret_rect` は `area_x_of` と行の上、`word_at` は欄と同じ。

### 2.5 app の口と境界

- 新 `void kl_ui_set_text_bar(struct kl_ui *ui, const struct kl_rect *bounds, int enabled);`: `enabled = 0` で指の選択の mode を使わない（double tap は今の全文、Notes の box）。`bounds` は bar を置ける範囲（NULL で canvas の全体）。
- KL_VERSION 72 → **74**（仮。P2 の WS191 が 73。merge で Q1 が揃える）。version の行に「74: the fingers' selection of the fields and the text area with handles and a bar, kl_text_bar_*, kl_text_touch_select, kl_text_touch_hide_bar, kl_text_touch_toggle_bar, kl_text_touch's bar, kl_ui_set_text_bar」。
- exports.map: 上の public の関数。
- `kl_window_can_paste` を compositor の clipboard が無い時に自分の copy の有無を返すよう直す（事実 6）。
- libkeiland は OS の header・ioctl を足さない（keiland-os-boundary PASS）。新しい file は zedBSD・Linux・FreeBSD の Makefile に足す。

## 3. Text Editor（p003、p002 の §2.1・§2.2 に依存）

- `te_app` に `struct kl_text_bar bar` と `unsigned bar_held`。
- `main_fingers`: 始めに、dialog・choosing の時は `bar.count = 0` にしてから今の早い return（M8）。`kl_ui_text_region` の後、`touch.bar && !touch.selecting && main_widgets_text` なら facts（選択・全文・空・`KL_TEXT_BAR_CLIPBOARD`・`kl_window_can_paste`）、`selection`（`app_text_view.caret_rect` の両端を text の矩形と scroll で窓の座標に）、`visible` = text の矩形、`bounds` = 窓 − `kl_window_keyboard_inset` → `kl_text_bar_layout` → `kl_text_bar_hit(main_input, MAIN_TEXT_BAR, …)`。
- 押された button: Copy → `te_app_action(TE_ACTION_COPY)` と `kl_text_touch_hide_bar`。Cut・Paste → `te_app_action` の後に **`kl_text_touch_set_selection(&touch, anchor, cursor)` を明示**（bar と古い handle を消す）。Select All → `TE_ACTION_SELECT_ALL` の後 `kl_text_touch_select(&touch, 0, length)`。`dirty = 1`。
- `app.c` 320 行の条件を `touch.handles || touch.bar` に（key で bar を消す）。
- `main_frame`: handle の後に `bar.count != 0` なら `kl_text_bar_draw`（`main_handles`、style は `main_overlay` と同じ）。
- `te_app_touch`: `KL_TEXT_TOUCH_BAR` で `dirty = 1`。
- log: `TOUCH bar shown buttons=… rect=X,Y,W,H`、`TOUCH bar hidden`、`TOUCH bar press button=copy|cut|paste|select-all`。

## 4. 試験

### 4.1 host（短いもの）

`plan/ws190/tests/run-host.sh`（host の cc、ASan・UBSan、libkeiland の file を直に compile、ws177 の host 試験の形）:

1. `kl_text_bar_buttons`: §1.2 の全ての組。
2. `kl_text_bar_layout`: 上・下・重ねる（handle との重なり）・左右の端・範囲より広い・範囲が低すぎる・keyboard の inset・選択が見えない・半分見える。
3. kl_text_touch: §2.1 の規則の全て、`keiui_text_touch_hold`。
4. 語（§2.4）: `hello world` の位置 0・3・5・6・11、空白の上、`abc日本`、`日本語、です`、秘密。
5. 欄と kl_ui（偽の canvas と font）: 指の double tap → 語と mode、handle の drag（down・motion・up）、handle の上の tap・long press で変わらない、handle の上の 2 本指は頁の scroll、bar の Copy・Cut（undo で戻る）・Paste・Select All、bar の key が app に渡らない（B1）、欄の外の tap・key・preedit・focus の移動・`kl_field_set` で mode を出る、frame の後に欄の struct を壊しても次の handle の drag が app の memory に触らない（ASan、B2）、秘密の欄、mouse の double click は全文、modal の dialog が後にある frame は bar 無し、`kl_ui_set_text_bar(…, 0)`。
6. text area: double tap、下の handle を次の行へ、Select All。

既存の host 試験（変える code を通る、回帰）: `plan/ws090/tests/host-input.sh`・`host-widgets.sh`、`plan/ws177/tests/host-text-edit.sh`・`host-field-limit.sh`、`plan/tools/textedit/host-core.sh`。

### 4.2 build と検べ

- amd64 の `$(BUILD)/dynamic/libkeiland.so`・`textedit`・`kuidemo`・`mailer`・`phone`・`calendar`・`notes` と、Linux の keiland（`keiland-linux.mk` の all）で warning 0。target を名指す。
- `plan/tools/keiland-os-boundary/check.sh` PASS、`plan/tools/style-check.py` で新しい指摘 0、`git diff --check`。

### 4.3 AAT（T1 に依頼）

- image: `plan/ws190/tests/config-amd64-aat-touch.mk`（AAT の config ＋ kuidemo、統合済み）。
- シナリオ `tests/scenarios/desktop/widgets/touch-select.md`（suite の full への追加は Q1 に依頼）:
  - 準備: kei で `kuidemo --height=820` を開く（stderr の `KUIDEMO` の行は session.log）。窓の位置は `aat windows`、欄の位置は撮影から読む（Controls の頁の Settings の card の Name・Password・Notes）。
  1. Name を tap、`aat key ctrl+a`・`aat key backspace`、`aat type 'hello world'`。`world` の上を `aat tap X Y --count 2` → `KUIDEMO SELECT id=name anchor=6 caret=11`、撮影で両端の handle と bar（Cut・Copy・Paste は clipboard 次第・Select All）。
  2. 左の handle（`world` の前の knob）を `aat touch-down 0 …`・`touch-move 0 …`（`hello` の始めへ）→ 撮影で bar が無い → `touch-up 0` → `KUIDEMO SELECT id=name anchor=11 caret=0`、撮影で bar。
  3. Copy を tap → bar が消え handle は残る（撮影）。Password を tap、`aat type x`、double tap → 撮影で bar に Copy・Cut が無く Paste がある。Paste を tap → `KUIDEMO FIELD password length=12`。
  4. Notes を tap、`aat type 'one two'`・`aat key enter`・`aat type 'three'`、`two` を double tap、下の handle を `three` の後へ → `KUIDEMO SELECT id=notes anchor=4 caret=13`。Select All → `anchor=0 caret=13`。Cut → `KUIDEMO FIELD notes bytes=0`。`aat key ctrl+z` → `bytes=13 text=one two|three`。
  5. 頁の何も無い所を tap → 撮影で bar と handle が無い。
  6. 画面 keyboard が出る image なら、Name で double tap → bar が keyboard に重ならない（撮影）。出ない時は not-run と注記。
- `tests/scenarios/apps/textedit/touch-select.md`: 3 行を打ち、語の double tap → `TOUCH bar shown`、Copy → `TOUCH bar press button=copy`・`TOUCH bar hidden`、1 本指の drag で 2 行を選ぶ → up で `TOUCH bar shown`、Cut → 2 行が消え、Ctrl+Z で戻る、long press → context menu と `TOUCH bar hidden`。
- 合否: 各手順の log の正解と撮影（人が見る項目は「人が見る」と書く）。

## 5. 受け入れの条件と Phase

- p002: §2、§1.5 の Mailer・Phone・Calendar の結び（§7 Q1 次第）、Notes の box の `kl_ui_set_text_bar(…, 0)`、§4.1 の 1〜6 と既存の host 試験 PASS、§4.2、§4.3 のシナリオ、T1 の依頼文。
- p003（p002 に依存）: §3、§4.1 の Text Editor の部分（`plan/tools/textedit/host-core.sh` の形で bar の facts と button の後の状態）、build、Text Editor のシナリオ、T1 の依頼文。
- **p004（新、規約の全文の見直し）**: WS の変えた C の全文を coding-style の全文と照らす（Awesome Plan §6）。
- WS の最後に T1 の AAT の PASS（Q1 の判定）。

## 6. 範囲の外・制限

- Notes の box の中の bar（app が範囲を与える口は作るが、Notes は使わない）。Future Work。
- Settings と Files の改名の欄の touch の経路（今は pointer か渡さない）。Future Work（§7 Q2）。
- 翻訳（bar の文字は英語。共有の domain を開かない app が多い、事実 12）。Future Work。
- 読み取り専用の欄の状態。mouse の選択への bar。欄への右 click の context menu。
- 空白の無い日本語の文の語は句読点まで（形態素の区切りはしない）。
- compositor の context menu（縦の menu）は Text Editor の long press のまま。bar を compositor の popup にしない理由: grab のある popup は出ている間の最初の touch を閉じるのに使うので、handle を続けて動かす操作と合わない。欄の library は窓の中に描く物で完結し、compositor の無い host 試験で確かめられる。

## 7. Q1 の判断（2026-10-08 Q1 の回答）

- **Q1 = 入れる**: Mailer・Phone・Calendar を p002 で `kl_ui_window_text` に結ぶ。
- **Q2 = 入れない**: Settings・Files の改名の欄の touch の経路は Future Work（下の F-087）。

Future Work の候補（Q1 が台帳に載せる）:

| ID | 内容 | 由来 | 再考の契機 |
| --- | --- | --- | --- |
| F-087 | Settings（touch を pointer に変える）と Files の改名の欄（touch を kl_ui に渡さない）で指の選択・bar を効かせる: touch を kl_ui に渡す経路の設計 | ws190-p001 review 1 M4、Q1 の判断 Q2 | Settings・Files の touch の UAT の指摘 |
| F-088 | Notes の文字の box の中に bar を置く（`kl_ui_set_text_bar` の bounds を box の中に、box の合成の範囲を広げる） | ws190-p001 review 1 M5 | Notes の box の touch の UAT |
| F-089 | bar の文字の翻訳（共有の domain `keiland` を program が開かなくても library が読む口） | ws190-p001 review 1 minor 6 | 日本語の UI の UAT |

以下は判断の前の問い（記録として残す）。

- **Q1**: Mailer・Phone・Calendar を `kl_ui_window_text` に結ぶ（数行ずつの置き換え、Cut・Copy・Paste が効くようになる）を p002 に入れるか。P1 の推し: 入れる（要望の中心になりそうな Mail の作成の欄で Copy が出ないのは不自然）。
- **Q2**: Settings・Files の改名の欄の touch の経路を kl_ui に渡す直しを WS190 に入れるか。P1 の推し: 入れない（Settings の touch の pointer 化は別の設計、Future Work）。

## 判断の記録

| # | 判断 | 理由 |
| --- | --- | --- |
| D1 | bar は窓の中に library が描く横の帯 | §6 の最後 |
| D2 | 欄の指の選択の状態は kl_ui が 1 つ持ち、view は kl_ui の写しだけを使う | mode は focus の 1 つの欄だけ。public の struct を保つ。app の memory を frame の間に触らない（review 1 B2） |
| D3 | 欄の handle と bar は `kl_ui_end` が、その時の clip の中で描く | 全ての app が widget の後に `kl_ui_end` を呼ぶ。例外の app は §1.5 と事実 9 |
| D4 | bar の命令は `kl_ui_end` の key の loop の後に owner への key として積む | clipboard・undo・limit・秘密の規則を二重に書かない。app に渡さない（review 1 B1） |
| D5 | 指の double tap は語、mouse の double click は今のまま全文 | 要望は指 |
| D6 | 秘密の欄でも Paste は出す | password manager の貼り付け |
| D7 | Copy の後は選択と handle を残し bar だけ隠す | 他の OS の普通 |
| D8 | 文字は素の英語（初版の `keiland.tr` をやめる） | 共有の domain は多くの app で読まれない（review 1 minor 6） |
| D9 | 語は独立の定義（§2.4） | `keiui_edit_word` は区切りを飛ばすので境界で 2 語になる（review 1 M7） |

## review 1（2026-10-08、design-reviewer）の反映

blocker 2・major 11・minor 13。B1 → §2.3「押された button」・D4。B2・M1 → §2.3 の表と「欄の 1 frame」・D2・§4.1 の 5。M2 → §2.3「ui_find と HANDLE」「ui_gesture と HANDLE」・§2.1 の hold。M3 → §2.3 の handle の記録・§1.1 の最後の行。M4 → §1.5・§7 Q1・Q2。M5 → §2.5 の `kl_ui_set_text_bar`・§1.5。M6 → §1.1・§2.3 の 2。M7 → §2.4・D9・§4.1 の 4。M8 → §3。M9 → §3 の依存・§5 の p004・事実 10・11（共有の道具の変更は Q1 の依頼と統合で記録）。M10 → §4.3。M11 → §1.1 の表。minor 1 → 事実 9・§2.3 の描画の約束。2・9 → §2.3 の関数 pointer・§1.4 の 4。3 → `from_bar`。4 → §1.4 の 2。5 → §1.2 の見た目。6 → D8。7 → §2.3 の表の scroll・layout と欄の 1 frame の 3。8 → 欄の 1 frame の「選択を描く前に」。10 → §2.5。11 → §1.4。12 → 事実 14・§1.1。13 → §2.1 の header の注記・§4.1 の既存の試験。
