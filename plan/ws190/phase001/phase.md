<!-- awesome-plan project=zedbsd record=ws190-p001 -->
# ws190-p001: 設計 — 文字の欄と Text Editor の指での選択（ダブルタップ・端の drag・編集の bar）

Status: in-progress（設計の初版、design-reviewer に回す）
Disposition: normal
Parent: [WS190](../ws.md)
Queue: q899（P1、2026-10-08 Q1 の依頼）

## 範囲

WS190 の要望と決定（ws.md）を、libkeiland の口・欄（kl_field・text area）の振る舞い・Text Editor・試験に落とす。コードは書かない。
p002（libkeiland の欄）と p003（Text Editor）の受け入れの条件をここで決める。

## 0. 今あるもの（2026-10-08 の読み、P1）

| # | 事実 | 所在 |
| --- | --- | --- |
| 1 | `kl_text_touch` は view の 3 つの答え（`position_at`・`caret_rect`・`word_at`、content の座標）で選択を持つ: 1 回の tap は caret、2 回は語を選び handle を出す、1 本指の drag は触れた所から選ぶ、handle の上で始まる drag はその端を動かす（caret の端を動かす形に入れ替える）、指が離れると選択に handle が残る、端に近い指は content を自動で scroll（`kl_text_touch_edge`）、long press は `KL_TEXT_TOUCH_MENU` | `libkeiland/ui/text-touch.c`、`include/keiland/keiland.h` 3066〜3143 行 |
| 2 | kl_ui は記録の種類 HIT・SCROLL・TEXT を持つ。指が触れた時に一番上の HIT（`touch_hit`）と一番上の SCROLL か TEXT（`touch_region`）を選ぶ。TEXT の region の 1 本指の drag は `UI_DRAG_SELECT`（`kl_text_touch_drag_begin` など）、tap は HIT が無ければ `kl_text_touch_tap`。HIT の widget は drag を取らない（`KEIUI_DRAGGABLE` 以外）ので、scroll の中の欄の上の drag は頁を scroll する | `libkeiland/ui/ui.c` 641〜700・2095〜2290 行 |
| 3 | 欄（`kl_field`）と text area（`kl_text_area`）は HIT の widget（`keiui_ui_widget(…, KEIUI_FOCUSABLE)`）。click か tap は caret を点に置き、2 回（`KL_HIT_DOUBLE`）は**全文**を選ぶ。kl_text_touch を使わず、handle も popup も無い | `ui/field.c` 166〜176 行、`ui/text-area.c` 156〜170 行 |
| 4 | 欄の編集の命令（ws177-p013）: Ctrl+C・X・V・A・Z・Y を `keiui_ui_take_input` の key として取り、`field_edit`／`area_edit` が clipboard（`keiui_edit_copy`・`keiui_edit_paste`、kl_ui に `kl_ui_window_text` が結んだ窓の `kl_window_copy`・`kl_window_paste`）と undo の履歴で行う。秘密の欄は copy・cut をしない | `ui/field.c` 572〜700 行、`ui/ui.c` 1561〜1670 行 |
| 5 | 欄に読み取り専用の状態は無い（`struct kl_field`・`struct kl_text_area` に flag 無し）。Text Editor にも読み取り専用の mode は無い | header 4008〜4038 行、textedit の grep |
| 6 | clipboard に貼れる物があるか: `kl_window_can_paste(window)`（自分の text、または他の program の text の offer） | `ui/clipboard.c` 239 行 |
| 7 | Text Editor は `kl_ui_text_region(main_input, MAIN_TEXT_REGION, …, &main_app.touch)` を **描画と別の**入力だけの frame（`main_fingers`）で記録し、handle は `main_frame` で `kl_text_touch_draw_handles` を `main_handles` の canvas に描く。long press は compositor の context menu（`kl_window_popup_menu`、Undo・Redo・Cut・Copy・Paste・Select All の縦の menu）。編集は `te_app_action(TE_ACTION_COPY …)` | `textedit/main.c` 700〜760・1800〜1850 行、`app.c` 600〜630 行、`menu.c` 285 行 |
| 8 | 画面 keyboard の inset: kl_ui は `ui_inset`（窓の大きさ、右と下から覆う幅）を持つ（`keiui_ui_inset_note`）。app は `kl_window_keyboard_inset(window, &right, &bottom)` | `ui/ui.c` 285〜295 行、`ui/window.c` 2528 行 |
| 9 | widget を使う app はどれも `kl_ui_begin` → widget を描く → `kl_ui_end` の順（Settings・Mailer・Calendar・Notes の box・Phone・Music・Photos・Files の改名・file chooser・kuidemo）。`kl_ui_end` は canvas を知らない。Mailer は描き直しを一部の clip に絞ることがある（`kl_ui_take_damage`） | 各 app の grep |
| 10 | 試験の注入: `/dev/input-inject` は touch screen（`INPUT_INJECT_KIND_TOUCH`、10 slot）を宣言できる。`touchinject` は script で指を流す。**`aat-input` は mouse・絶対 pointer・keyboard だけで touch が無い** | `include/uapi/input-inject.h` 24〜35 行、`userland/tests/touchinject`、`userland/tests/aat-input/main.c` 8〜40 行 |
| 11 | kuidemo（試験の image だけの widget の見本）は名前の欄と秘密の欄（Password）を持ち、`KUIDEMO` の行を log に出す。text area は無い | `userland/tests/kuidemo/main.c` 957・967 行 |
| 12 | 翻訳: library の widget の文字は今は素の英語（file chooser の "Cancel"）。`kl_tr` は program の domain の次に共有の domain `keiland` を探す。catalog は `userland/desktop/locale/LANG/DOMAIN.tr` を wildcard で install（Linux・FreeBSD の mk）。`keiland.tr` はまだ無い | `chooser-view.c` 745 行、header 2378〜2392 行、`keiland-linux.mk` 157 行 |

## 1. 振る舞い（ユーザーの要望と決定から）

### 1.1 指での選択の mode（欄・text area・Text Editor で共通）

| 操作 | 結果 |
| --- | --- |
| 文字の上の**ダブルタップ** | その語を選び（語が無い所なら caret だけ）、選択の mode に入る。選択が空でなければ両端に handle。**指を離した時に** bar を出す |
| handle の drag | その端を動かす（もう一方は止まる。端を越えると両端が入れ替わる、今の kl_text_touch のまま）。drag の間 bar は隠す。指を離すと bar を出し直す（選択の新しい位置に） |
| bar の button の tap | 下の §1.3。button の押下は focus を奪わない |
| 選択の外の 1 回の tap（その欄の中） | caret をそこへ、mode を出る（handle と bar が消える） |
| 欄の外の tap（別の widget、何も無い所） | mode を出る（選択はそのまま残る、handle と bar が消える）。別の欄なら focus が移る（今のまま） |
| key・画面 keyboard の文字・IME の commit | 今の編集のまま行い、mode を出る |
| focus を失う、欄がその frame に描かれない（頁が変わった） | mode を出る |
| 頁の scroll（欄の外か handle 以外の所の drag） | 今のまま頁が scroll する。drag の間 bar は隠し、指が離れたら出し直す。bar と handle は毎 frame 欄の今の位置から描くので scroll に付いて動く |
| mouse の click・double click | **変えない**（double click は欄では全文、Text Editor では語）。bar は指の選択だけに出す |

- **Text Editor だけの違い**（今のまま保つ）: 1 本指の drag はどこからでも選択を始める（text view は頁の scroll の中に無い、2 本指で scroll）。
  指を離した時に選択が空でなければ bar を出す。long press は今の context menu（compositor の縦の menu）を開き、bar を隠す。
- 欄と text area で double tap を「全文」から「語」に変えるのは指だけ（`KL_HIT_TOUCHED` の時）。mouse の double click の全文は変えない。
- 秘密の欄（`secret`）の「語」は全文（点に語の境が見えないので）。

### 1.2 bar（「編集の bar」）

- 横一列の button の帯（丸い角の pill、影付き）。button は左から **切り取り・コピー・貼り付け・すべて選択**（決定の 4 つ）。
  出さない条件（Q1 の推しを採る）:

| button | 出す条件 |
| --- | --- |
| 切り取り（Cut） | 選択が空でない、秘密の欄でない、読み取り専用でない、clipboard の窓が結ばれている |
| コピー（Copy） | 選択が空でない、秘密の欄でない、clipboard の窓が結ばれている |
| 貼り付け（Paste） | `kl_window_can_paste` が真、読み取り専用でない（秘密の欄でも出す: password manager の貼り付けを妨げない） |
| すべて選択（Select All） | text が空でなく、選択がまだ全文でない |

  - 読み取り専用: 今の欄と Text Editor には無い（事実 5）。bar の口は `KL_TEXT_BAR_READ_ONLY` の印を取り（§2.2）、将来の読み取り専用の欄はそれを渡す。今回は読み取り専用の状態を足さない。
  - 窓が結ばれていない kl_ui（`kl_ui_window_text` を呼ばない app、事実 4）は clipboard が無いので Cut・Copy・Paste を出さない。
  - 出す button が 1 つも無い時は bar を出さない（例: 空の欄で clipboard も空）。
- 文字: "Cut"・"Copy"・"Paste"・"Select All" を `kl_tr` で引く（共有の domain `keiland`）。`userland/desktop/locale/ja/keiland.tr` を足して「切り取り・コピー・貼り付け・すべて選択」を入れる（`tools/i18n/tr.py update`）。catalog を開かない program では英語のまま（事実 12）。
- 見た目: 地は `theme->panel`、縁は `theme->control_edge`、影は `kl_canvas_shadow`、文字は `theme->text`（14 px）、button の間に縦の細い線（`control_edge`）、押されている button の地は `theme->selection`。高さ 36 px、button の左右の余白 12 px。光の appearance と暗い appearance は theme がそのまま切り替える。

### 1.3 bar の button を押した後

| button | 行い | その後 |
| --- | --- | --- |
| Copy | 選択を clipboard へ | 選択と handle は残る、bar は消える（mode は handle のまま続く。もう一度 bar を出すには handle を動かすか double tap） |
| Cut | 選択を clipboard へ写して消す（undo の履歴に 1 つ） | caret だけ、mode を出る |
| Paste | clipboard の text を選択の代わりに入れる（履歴に 1 つ、欄の limit まで） | caret は入れた text の後、mode を出る |
| Select All | 全文を選ぶ | 両端に handle、bar を新しい位置に出し直す |

### 1.4 bar の位置（純粋な計算、host 試験で確かめる）

入力: 選択の矩形 `selection`（窓の座標。選択の最初の行の上から最後の行の下まで。1 行なら左右は両端の x、複数行なら text の箱の左右）、
置ける範囲 `bounds`、bar の大きさ（幅 = button の幅の和、高さ 36）。定数: 間 `GAP` 8 px、knob の高さ `KL_TEXT_HANDLE` 12 px。

1. **縦**: 上に置く `y = selection.y - GAP - 36`。`y < bounds.y` なら下に置く `y = selection.y + selection.height + KL_TEXT_HANDLE + GAP`。
   下も `y + 36 > bounds.y + bounds.height` なら（選択が範囲を縦に覆う）、範囲の中の選択の上の端の内側 `y = max(bounds.y + GAP, selection.y + GAP)` に重ねて置き、
   範囲の下を越えない（`y = min(y, bounds.y + bounds.height - 36 - GAP)`）。
2. **横**: 選択の中央に合わせる `x = selection.x + selection.width / 2 - width / 2`。`[bounds.x + GAP, bounds.x + bounds.width - GAP - width]` に収める。
   幅が範囲より広い時は `x = bounds.x`（右が切れる。button を省かない）。
3. **選択が見えない**: 選択の矩形と、その view の見えている矩形（欄の clip、§2.3）の共通部分が空なら bar を出さない。共通部分があればその共通部分を `selection` に使う（scroll で半分隠れた選択は見えている所に合わせる）。
4. **範囲 `bounds`**: 描く canvas の全体（窓）から、画面 keyboard の inset が覆う下と右を除いた矩形。inset は kl_ui の `ui_inset`（窓の大きさが canvas と同じ時だけ使う）、Text Editor は `kl_window_keyboard_inset`。
   こうして keyboard に重ならない。keyboard が選択そのものを覆う時は 1 の 3 段目で範囲の下に収める（ws102 の inset の中心合わせが選択を keyboard の上へ動かすのが普通）。

## 2. libkeiland（p002）

### 2.1 kl_text_touch の追加（public、KL_VERSION 74 の仮）

- `struct kl_text_touch` の**末尾**に `int bar;`（bar を出すか）を足す（KL_VERSION 64 の `kl_field.limit` と同じ末尾の追加。使うのは libkeiland と Text Editor で、どちらも同じ tree で build される）。
- 規則（text-touch.c）:
  - `kl_text_touch_tap(…, twice=0)`: `bar = 0`。`twice=1`: `bar = 1`（語が無くても caret の位置に出す。出す button が無ければ描く側が出さない）。
  - `kl_text_touch_drag_begin`: `bar = 0`（drag の間は隠す）。
  - `kl_text_touch_drag_end`: 選択が空でなければ `bar = 1`、空なら `bar = 0`。
  - `kl_text_touch_set_selection`（key・pointer の選択）: `bar = 0`（今の handle と同じ）。
  - `kl_text_touch_long_press`: `bar = 0`（context menu と重ねない）。
  - 新しい `void kl_text_touch_select(struct kl_text_touch *touch, size_t anchor, size_t caret);`: 指の選択として置く（空でなければ handle、`bar = 1`、`KL_TEXT_TOUCH_SELECTION`）。Select All の後に使う。
  - 新しい `void kl_text_touch_hide_bar(struct kl_text_touch *touch);`: Copy の後（選択と handle は残す）。
- 変わった時に描き直しが要るので、`bar` の変化も `KL_TEXT_TOUCH_SELECTION` と同じく `changes` に立てる（新しい bit `KL_TEXT_TOUCH_BAR` 4U）。

### 2.2 bar の部品（public、新しい file `ui/text-bar.c`）

```c
#define KL_TEXT_BAR_CUT		1U
#define KL_TEXT_BAR_COPY	2U
#define KL_TEXT_BAR_PASTE	4U
#define KL_TEXT_BAR_SELECT_ALL	8U
#define KL_TEXT_BAR_BUTTONS	4U

/* What a text decides the bar from (bits for kl_text_bar_buttons). */
#define KL_TEXT_BAR_SELECTED	1U	/* the selection is not empty */
#define KL_TEXT_BAR_WHOLE	2U	/* the selection is the whole text */
#define KL_TEXT_BAR_EMPTY	4U	/* the text is empty */
#define KL_TEXT_BAR_SECRET	8U
#define KL_TEXT_BAR_READ_ONLY	16U
#define KL_TEXT_BAR_CLIPBOARD	32U	/* a window's clipboard is there to copy to */
#define KL_TEXT_BAR_CAN_PASTE	64U	/* the clipboard has text */

struct kl_text_bar {
	unsigned buttons;			/* KL_TEXT_BAR_CUT ... shown */
	struct kl_rect rect;			/* the bar, window coordinates */
	size_t count;
	unsigned kinds[KL_TEXT_BAR_BUTTONS];	/* each cell's button, left to right */
	struct kl_rect cells[KL_TEXT_BAR_BUTTONS];
};

unsigned kl_text_bar_buttons(unsigned facts);
int kl_text_bar_layout(struct kl_text_bar *bar, struct kl_text *text, unsigned buttons, const struct kl_rect *selection, const struct kl_rect *bounds);
unsigned kl_text_bar_hit(struct kl_ui *ui, uint32_t id, const struct kl_text_bar *bar, unsigned *held);
void kl_text_bar_draw(const struct kl_text_bar *bar, const struct kl_style *style, unsigned held);
```

- `kl_text_bar_buttons`: §1.2 の表を facts から（純粋）。
- `kl_text_bar_layout`: §1.4 の計算と cell の幅（文字の幅 + 余白、`kl_text_width`）。button が 0 なら 0 を返し `bar->count = 0`。出す時 1。
- `kl_text_bar_hit`: 各 cell を `kl_ui_hit(ui, id, cell の番号, …)` と同じ記録で、**focus を奪わない**印（新しい内部の flag `KEIUI_KEEP_FOCUS`: `ui_focus_press` が「何も無い所の press は focus を外す」と扱わない）で記録する。前の frame からの click の button（1 つの `KL_TEXT_BAR_*`、無ければ 0）を返し、`*held` に押されている button を返す。
- `kl_text_bar_draw`: §1.2 の見た目。`held` の button の地を変える。
- id: app は自分の widget の id と重ならない id を選ぶ（Text Editor は `MAIN_TEXT_BAR`）。library の欄は内部の予約の id `KEIUI_TEXT_BAR_ID`（0xfffffffdU。`KEIUI_ANY` 0xfffffffeU の隣、header に「app は使わない」と書く）。

### 2.3 欄と text area の選択（kl_ui の中、内部）

**状態の置き場所**: `struct kl_field`・`struct kl_text_area` は変えない。指の選択の mode に入れるのは focus を持つ 1 つの欄だけなので、kl_ui が 1 つ持つ（`struct ui_select`、ui.c の内部、新しい file `ui/text-select.c` に関数）:

| field | 意味・寿命 |
| --- | --- |
| `active` | mode の中か。§1.1 の「mode を出る」で 0 |
| `owner` | 欄の id と index（`struct ui_key`） |
| `kind` | 欄か text area か |
| `touch` | `struct kl_text_touch`（§2.1 の規則をそのまま使う） |
| `view` | kind ごとの `kl_text_view`（static const、field.c・text-area.c が持つ） |
| `scroll` | kl_ui が持つ `struct kl_scroll`。content を欄は横（`KL_SCROLL_X`）、area は縦（`KL_SCROLL_Y`）に動かす。handle の drag の自動 scroll（`kl_text_touch_edge`）と `ui_content` の座標の変換に使う |
| `box` | text の箱（窓の座標。欄は `rect.x + FIELD_SIDE`、area は `rect.x + AREA_SIDE, rect.y + AREA_TOP`）。content の座標の原点 = `box` の角 − scroll |
| `clip` | 欄が描かれた時の canvas の clip（`style->canvas->clip`）。§1.4 の 3 の「見えている矩形」 |
| `style` | 欄が描いた `struct kl_style` の**値の写し**（canvas・text・theme は app の物で、frame をまたいで生きる。style の struct 自身は app の stack の場合がある） |
| `data` | view の答えに要る物: 欄は `struct kl_field *`（app の物。`kl_ui_text_region` の touch と同じく次の frame まで生きる約束）、area は前の frame の行の割り付け（`struct area_layout`、kl_ui が最初の area の選択で 1 回 malloc し `kl_ui_destroy` で free。key は次の frame まで待つので、frame の間に text は変わらない） |
| `drawn` | この frame に owner が登録したか（`kl_ui_end` が見る） |
| `bar` | 今の frame の `struct kl_text_bar` |

**欄の 1 frame**（field.c、text-area.c も同じ形）:

1. `KL_HIT_CLICKED` かつ `KL_HIT_TOUCHED`:
   - `KL_HIT_DOUBLE`: `keiui_select_begin(ui, id, kind, view, data, box, …)` で mode に入り `kl_text_touch_tap(…, twice=1)`（語、handle、`bar=1`）。欄の `caret`・`anchor` を touch の物にする。
   - 1 回: mode の中なら `kl_text_touch_tap(…, 0)` → mode を出る（caret は今の field の click の処理のまま）。
   - mouse の click（`TOUCHED` 無し）: 今のまま、mode の中なら出る。
2. key・文字・commit を取った（`changes` か取った input がある）: mode を出る（`keiui_select_end`）。ただし bar の button が送った命令（下）は例外で、§1.3 の「その後」に従う。
3. mode の中なら毎 frame: `keiui_select_frame(ui, id, box, clip, style, content の幅か高さ, 今の scroll)` で登録（`drawn = 1`）。
   - drag の間（`touch.selecting`）は kl_ui の `scroll` の位置を欄が受け取る（`field->scroll = (int)select.scroll.x`、area は y）。その後の「caret を見える所に」の今の規則はそのまま走る。
   - drag の間でなければ欄の scroll を kl_ui の `scroll` に写す（`kl_scroll_move_to(…, 0)` と content・viewport の大きさ、`kl_scroll_set_bounds`）。
   - touch の `changes` の `KL_TEXT_TOUCH_SELECTION` を欄の `caret`・`anchor` に移す（`kl_text_touch_take`）。
4. 欄の選択の色は今のまま（focus の時に塗る）。

**kl_ui の中の流れ**:

- **handle の記録**: `kl_ui_end` の始め（frame の入れ替えの前）に、mode の中で `drawn` なら、handle の各端の knob の中心の周り `KL_TEXT_HANDLE_REACH` 四方を新しい記録の種類 `UI_KIND_HANDLE` で記録する（最後に記録するので一番上）。記録は `box`・`scroll`・`touch` を持つ。
- **指の down**: 一番上の記録が `UI_KIND_HANDLE` なら `touch_hit` は無し、`touch_region` はその記録（`ui_content` が使う矩形は記録の `box`。記録に「view の矩形」を足し、TEXT の記録では今の rect と同じ）。
- **handle の drag**: `UI_DRAG_SELECT` の今の道（`kl_text_touch_drag_begin` が knob の近くを見て端を持つ、`ui_drag_step` が指と端の自動 scroll）。
- **handle の上の tap**: 何もしない（選択も bar も保つ）。`ui_gesture` の tap で region が `UI_KIND_HANDLE` の時は飛ばす。
- **欄の外の tap で mode を出る**: `ui_gesture` の tap で、mode の中、tap の HIT が owner でも bar でもなく、region が handle でない時、`keiui_select_end`（選択は欄に残る）。
- **mode を出る他の時**: focus が owner から移った時（`ui->focus` の変化）、`kl_ui_end` で `drawn == 0`（その frame に欄が描かれない）の時。
- **bar の描画と記録**: `kl_ui_end` の始めに、mode の中で `drawn`、`touch.bar`、`touch.selecting == 0`、`ui->drag` が `UI_DRAG_SCROLL` でない時:
  1. facts を作る（選択の有無、全文か、空か、`field->secret`、`ui->window != NULL` で `KL_TEXT_BAR_CLIPBOARD`、`kl_window_can_paste(ui->window)` で `CAN_PASTE`）→ `kl_text_bar_buttons`。
  2. `selection`（view の `caret_rect` の両端から、窓の座標）と `bounds`（`style.canvas` の全体 − `ui_inset`）→ `kl_text_bar_layout`。
  3. `kl_text_bar_hit(ui, KEIUI_TEXT_BAR_ID, …)` で記録し、押された button を見る。
  4. 写しの `style` の canvas に、clip を canvas の全体にして（`kl_canvas_clip_push`、終わりに pop）`kl_text_bar_draw`。
  5. handle も同じ所で、`clip` ∩（`box` を左右に `KL_TEXT_HANDLE`、下に `KL_TEXT_HANDLE` 広げた矩形）の clip で `kl_text_touch_draw_handles` で描く（欄の上に後から描く widget に隠されない）。
- **押された button**: kl_ui は owner への key の入力を作って `ui->keys` に積む（`target = owner`）: Cut = Ctrl+X、Copy = Ctrl+C、Paste = Ctrl+V、Select All = Ctrl+A。欄は次の frame に今の `field_edit`・`area_edit`・Ctrl+A の処理で行う（clipboard・undo・limit の規則がそのまま効く）。`kl_ui_end` は 1 を返す（次の frame が要る）。
  bar が起こした命令の印（`struct ui_press` の新しい field `from_bar`）を欄が見て、§1.3 の「その後」にする: Copy → `kl_text_touch_hide_bar`（mode は続く）、Cut・Paste → mode を出る、Select All → `kl_text_touch_select(touch, 0, length)`。
- 描画の約束（header の kl_ui の説明に足す）: **`kl_ui_end` は、指の選択の mode の欄があれば、その欄を描いた canvas に handle と bar を描く**。だから app は `kl_ui_end` を、widget を描いた canvas がまだ有効な間に呼ぶ（今の全部の app がそう、事実 9）。`kl_ui_end` の後に描く物（Mailer の質問の dialog）は bar の上に来る。

### 2.4 view の 3 つの答え（欄・area）

- 欄（content の x は text の始めから。y は箱の上から）:
  - `position_at`: 今の `field_at` と同じ（表示の text、秘密は点の幅）。
  - `caret_rect`: x = その位置までの表示の幅（`kl_text_width`）、y = 8 − 0（箱の上からの caret の上、今の caret の線と同じ 8〜高さ−8）、幅 2、高さ = 欄の高さ − 16。
  - `word_at`: 秘密は 0〜length。他は `keiui_edit_word` の後ろ向きと前向き（Ctrl+Left・Right と同じ語の境）。位置が空白の上なら start = end。
- text area: 前の frame の割り付けで、`position_at` は行（y / AREA_LINE）と `area_at_x`、`caret_rect` は `area_x_of` と行の上、`word_at` は欄と同じ。

### 2.5 ABI・version・境界

- KL_VERSION 72 → **74**（仮。P2 の WS191 が 73 を取っている。merge で Q1 が揃える）。header の version の行に「74: the touch selection's bar of the fields, the text area and a text view, kl_text_bar_*, kl_text_touch_select, kl_text_touch_hide_bar and kl_text_touch's bar」。
- exports.map: `kl_text_bar_buttons`・`kl_text_bar_layout`・`kl_text_bar_hit`・`kl_text_bar_draw`・`kl_text_touch_select`・`kl_text_touch_hide_bar`。
- `struct kl_text_touch` の末尾の追加だけで、他の public の struct は変えない。
- libkeiland は OS の header・ioctl を足さない（`plan/tools/keiland-os-boundary/check.sh` が PASS）。新しい file は Linux・FreeBSD の Makefile にも足す。

## 3. Text Editor（p003）

- `te_app` に `struct kl_text_bar bar` と `unsigned bar_held` を足す。
- `main_fingers`（入力の frame）: `kl_ui_text_region` の後、dialog が無く choosing でなく、`main_app.touch.bar` かつ `!main_app.touch.selecting` かつ `main_widgets_text` なら:
  1. facts: 選択の有無（`anchor != cursor`）、全文か、空か、`KL_TEXT_BAR_CLIPBOARD`（窓は常に有る）、`kl_window_can_paste(main_window.kui)`。
  2. `selection`: `app_text_view.caret_rect` の両端を `te_app_text_rect` と scroll で窓の座標に（1 行なら両端の x、複数行なら text の矩形の左右）。見えている矩形 = text の矩形。
  3. `bounds`: 窓（`main_width`×`main_height`）から `kl_window_keyboard_inset` の右と下を除く。
  4. `kl_text_bar_layout(&main_app.bar, &main_widgets, …)` → `kl_text_bar_hit(main_input, MAIN_TEXT_BAR, &main_app.bar, &main_app.bar_held)`。
  5. 押された button: Copy → `te_app_action(TE_ACTION_COPY)` と `kl_text_touch_hide_bar`、Cut → `TE_ACTION_CUT`、Paste → `TE_ACTION_PASTE`（どちらも後で `te_edit_select` が `kl_text_touch_set_selection` を呼ぶので bar が消える）、Select All → `TE_ACTION_SELECT_ALL` の後 `kl_text_touch_select(&touch, 0, length)`。`dirty = 1`。
  6. bar を出さない時は `main_app.bar.count = 0`。
- `main_frame`: handle の後に、`main_app.bar.count != 0` なら `kl_text_bar_draw(&main_app.bar, &style, main_app.bar_held)`（`main_handles` の canvas、clip は窓の全体、style は `main_overlay` と同じ作り）。
- `te_app_touch`: `KL_TEXT_TOUCH_BAR` の変化で `dirty = 1`。long press の context menu は今のまま（kl_text_touch が bar を隠す）。
- log（試験のため、今の `te_log` の形）: `TOUCH bar shown buttons=… rect=X,Y,W,H`、`TOUCH bar hidden`、`TOUCH bar press button=copy|cut|paste|select-all`。

## 4. 試験

### 4.1 host（p002・p003、短いもの）

`plan/ws190/tests/run-host.sh`（host の cc、ASan・UBSan、libkeiland の file を直に compile、今の ws177 の host 試験の形に合わせる）:

1. `kl_text_bar_buttons`: §1.2 の表の全ての組（選択無し → Cut・Copy 無し、clipboard 空 → Paste 無し、秘密 → Cut・Copy 無し・Paste 有り、読み取り専用 → Cut・Paste 無し、全文 → Select All 無し、空の text → Select All 無し、窓無し → Select All だけ、全部無し → 0）。
2. `kl_text_bar_layout`: 上に置く、上が足りず下、上下とも足りず重ねる、左の端・右の端で収める、範囲より広い、keyboard の inset で下が狭い、選択が見えない（0 を返す）。
3. kl_text_touch: double tap → bar=1、drag_begin → 0、drag_end（空でない）→ 1、（空）→ 0、set_selection → 0、long press → 0、`kl_text_touch_select` → 1 と handle、`hide_bar` → 0 で handle は残る。
4. 欄と kl_ui（偽の canvas と font、今の kuidemo・ws177 の host の道具に合わせる）: 指の double tap → 語の選択と mode、handle の drag（touch down を handle の上、motion、up）で端が動く、bar の Copy の tap → clipboard の偽の copy に選択の bytes、Cut → text から消え undo で戻る、Paste → 入る、Select All → 全文、欄の外の tap → mode を出る、key → mode を出る、秘密の欄 → Copy・Cut が出ない、mouse の double click は全文のまま。
5. text area: double tap → 語、下の handle を次の行へ drag、Select All。

### 4.2 build

- amd64（zedBSD）の `$(BUILD)/dynamic/libkeiland.so`・`textedit`・`kuidemo` と、Linux の keiland（`make -f … keiland-linux` の target の libkeiland と textedit）で warning 0。target を名指す。
- `plan/tools/keiland-os-boundary/check.sh` PASS。`tools/i18n/tr.py check userland/desktop/locale/ja/keiland.tr` PASS。`plan/tools/style-check.py` で新しい・変えた C の file に指摘 0。

### 4.3 AAT（T1 に依頼）

- **aat-input に touch を足す**（Q1 の指示の「aat-input の touch」）: server が 4 つ目の device として touch screen（`INPUT_INJECT_KIND_TOUCH`、W×H の画素）を宣言し、命令 `tap X Y`・`double-tap X Y`・`touch-drag X1 Y1 X2 Y2 [STEPS]`（1 本指、down → STEPS の move → up）・`touch-down ID X Y`・`touch-move ID X Y`・`touch-up ID`。`plan/tools/aat/aat` に同じ名前の命令と、host の自己試験（`fake-aat-input.py`・`run-host.sh`）の行。
- **kuidemo に text area を足す**（fields の頁、名前・Password の下に Notes の 3 行の area）と、選択の log の行 `KUIDEMO SELECT id=name anchor=… caret=…`（欄の caret・anchor が変わった時）と `KUIDEMO TEXT id=… text=…`（今の field の changed の行が無ければ）。
- 試験の image: `plan/ws190/tests/config-amd64-aat.mk`（`plan/tools/aat/config-amd64-aat.mk` を include して `ZEDBSD_USER_PROGRAMS += kuidemo`）。過去の build を入力にしない。
- シナリオ（`tests/scenarios/desktop/widgets/touch-select.md`、suite の full に入れる）:
  1. kuidemo の fields の頁。名前の欄に `hello world` と打つ。`world` の上を double-tap → 撮影で両端の handle と bar（Cut・Copy・Paste は clipboard 次第・Select All）、log `KUIDEMO SELECT id=name anchor=6 caret=11`。
  2. 左の handle を `hello` の始めへ touch-drag → 指の間 bar が無い（drag の途中の撮影は touch-down・touch-move の後、touch-up の前）→ up の後に bar、`anchor=0 caret=11`（または入れ替えの形）。
  3. Copy を tap → bar が消え handle は残る。Password の欄を tap して `x` と打ち、double-tap → bar に Copy・Cut が無く Paste がある（撮影）。Paste を tap → `KUIDEMO` の Password の長さが増える。
  4. area に 2 行を打ち、1 行目の語を double-tap、下の handle を 2 行目へ drag → 撮影、Select All → 全文、Cut → area が空（log）、Ctrl+Z で戻る。
  5. 名前の欄の外（頁の何も無い所）を tap → bar と handle が消える（撮影）。
  6. 画面 keyboard（ある image なら）: 名前の欄で double tap → bar が keyboard に重ならない（撮影）。無ければ省き、注記に書く。
- Text Editor のシナリオ（`tests/scenarios/apps/textedit/touch-select.md`）: 3 行を打ち、語の double-tap → bar（log `TOUCH bar shown`）、Copy → `TOUCH bar press button=copy`・`TOUCH bar hidden`、1 本指の drag で 2 行を選ぶ → up で bar、Cut → 2 行が消え、Ctrl+Z で戻る、long press → context menu が開き bar が消える。
- 合否: 各手順の log の正解と、撮影の bar と handle の位置（人が見る項目は「人が見る」と書く）。

## 5. 受け入れの条件

- p002: §2 の口と振る舞い、§4.1 の 1〜5 の host 試験 PASS、§4.2 の build と check、AAT のシナリオと aat-input・aat・kuidemo の追加、T1 への依頼文を Q1 へ。
- p003: §3、§4.1 の 3 の Text Editor の部分（host で `te_app_touch` と bar の facts の組み立て、今の textedit の host 試験の形があれば）、build、Text Editor の AAT のシナリオ、T1 への依頼文。
- WS の最後に T1 の AAT の PASS（Q1 の判定）。

## 6. 範囲の外・制限

- **Notes の文字の box**（`notes/box.c`）: box は overlay の canvas に box の矩形（と影）の分だけを合成するので、box の外に置かれる bar は見えない。box の中に bar の範囲を絞る口（`bounds` を app が与える）は今回作らない。Future Work の候補として WS の結果に書く。
- 読み取り専用の欄の状態の追加はしない（印 `KL_TEXT_BAR_READ_ONLY` だけを用意）。
- mouse の選択に bar は出さない。右 click の context menu を欄に足すこともしない。
- compositor の context menu（縦の menu）は Text Editor の long press のまま。bar を compositor の popup にしない理由: grab のある popup は、出ている間の最初の touch を閉じるのに使うので、handle を続けて動かす操作（要望の「開始端・終了端をドラッグで決め、指を離すとポップアップ」）と合わない。また欄の library は窓の中に描く物で完結し、compositor の無い host 試験で確かめられる。

## 判断の記録

| # | 判断 | 理由 |
| --- | --- | --- |
| D1 | bar は窓の中に library が描く横の帯。compositor の popup にしない | §6 の最後 |
| D2 | 欄の指の選択の状態は kl_ui が 1 つ持つ。`kl_field`・`kl_text_area` の struct を変えない | mode は focus の 1 つの欄だけ。public の struct の大きさを保つ |
| D3 | 欄の handle と bar は `kl_ui_end` が描く | 全ての app が widget の後に `kl_ui_end` を呼ぶ（事実 9）ので、app を変えずに一番上に描ける |
| D4 | bar の命令は owner への Ctrl+X・C・V・A の key として欄に渡す | clipboard・undo・limit・秘密の規則（事実 4）を二重に書かない |
| D5 | 指の double tap は語、mouse の double click は今のまま全文 | 要望は指。mouse の振る舞いを黙って変えない |
| D6 | 秘密の欄でも Paste は出す | password manager の貼り付けを妨げない。Copy・Cut は出さない（今の Ctrl+C・X の規則と同じ） |
| D7 | Copy の後は選択と handle を残し bar だけ隠す | 続けて別の所に貼る前に選択を確かめられる。他の OS の普通の振る舞い |
| D8 | 文字は `kl_tr` の共有の domain `keiland` と `ja/keiland.tr` | library の初めての翻訳。catalog の install は wildcard で既にある |
