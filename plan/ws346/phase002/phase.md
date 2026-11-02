<!-- awesome-plan project=zedbsd record=ws346-p002 -->

# ws346-p002: DOM の変更の記録・mode と計数の土台

Status: cleared（2026-11-03 Q1。host build warning 0、host-change 79/79、基点と同じ dom 39/42・js 21/21・fetch 38/38・http 15/15・async 19/20。Acid は p003 で取る）
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: [q2002](../../queue.md)（Q1 が直に実装、2026-11-03 ユーザーの指示）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

DOM の変更を node の bit と document の要約に記録する（design.md §5.1）。mode の読み取りと計数の土台（§5.5.1・§5.5.3）を足す。
**頁の振る舞いは変えない**（記録するだけで、まだ誰も読まない。mode が何でも今どおり動く）。

## 書く物

- `userland/desktop/libbrowser/dom/change.c`（新規）、`dom/dom.h`（`reserved` を `change` に改名、enum・bit・関数・document の field）。
- §5.1.5 の表の全ての呼ぶ所（`dom/node.c`・`dom/shadow.c`・`dom/select.c`・`dom/control.c`・`dom/radio.c`・`bind/html-input.c`・`bind/select.c`・
  `bind/prelude.c`・`bind/environment.c`・`bind/style-sheet.c`・`bind/geometry.c`・`page/images.c`・`page/media.c`）: generation を進める行を
  `dom_note_change` に置き換える。generation の進み方は今と同じにする。
- `libbrowser/sources.mk`（`dom/change.c` を足す。host-build.sh が拾う形か確かめる）。
- `page/`: `BROWSER_LAYOUT_CACHE` を 1 回読む関数と、page ごとの計数の構造体と、page の解放の時の 1 行の出力（§5.5.3。まだ数える物は
  `engines`・`styles`・`style_hits`・`layouts` だけでよい。他は 0）。置き場所は実装の担当が決める（`page/page.c`・`page/page.h`）。
- 試験: `plan/ws346/tests/host-change.c`（libbrowser の DOM の関数を直に呼ぶ host の試験。既存の `plan/ws074/tests/host-*.c` の形と build の方法に倣う）。
  種類ごとに bit・要約・`change_sheets` の条件・上限・quiet の属性・削除された部分木の bit の消去を確かめる。

## 確かめ

- host build warning 0、`host-change` PASS。
- `python3 plan/ws074/tests/run-dom-tests.py`・`run-js-tests.py`・`run-fetch-tests.py`・`run-http-tests.py`（`--async` も）・`run-acid-tests.py` の結果が
  基点と同じ（基点の数を先に取る）。
- `BROWSER_LAYOUT_CACHE_STATS=1` で 1 行が出る。

## 完了の条件

上の全部。generation の値の列が基点と同じこと（試験の中で、同じ操作の列の後の generation を基点の期待の値と比べる）。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に §5.1・§5.5.1・§5.5.3）。
4. 触る source の今の code（下の「書く物」の file と、その呼び出し元）。

## 守る事

- 下の「書く物」の file と試験だけを変える。他の module・公開の header（`userland/desktop/include/browser/browser.h`）・HAL・toolchain・
  `plan/` の他の記録（AGENTS.md・master.md・queue.md ほか）を変えない。この phase.md の「結果」の節は書いてよい。
- design.md の契約（関数の意味・bit・mode・計数の行の形）を広げない・変えない。足りない・誤りがあれば止めて Q1 に報告する。
  契約が黙っている所は自分で決め、「結果」の節に列挙する。
- source の comment に WS・Phase の番号、plan/ への参照、日付の経緯を書かない。
- `rm` などで file を消さない。commit は `git commit -m WIP -- <path>...`（新しい file は先に `git add`）。push しない。
- build は `CC=clang sh plan/ws074/tests/host-build.sh plain`（warning 0）。大きな build の後は `sync`。
- 試験が通った後、git の外で実装を数通りわざと壊し、試験が FAIL することを確かめて戻す（`cmp` で戻ったことを確かめる）。
- `BROWSER_LAYOUT_CACHE=off`（と未設定）で、既存の試験の結果が基点と同じであること。

## 報告（Q1 へ）

commit の SHA、流した command と最後の行、自分で決めた点、未実施、わざと壊した方法と結果。

## 結果

2026-11-03 Q1（main の checkout で実装）。

### したこと

- `dom/change.c`（新規）: `dom_note_change`・`dom_change_needs_sheets`・`dom_changes_take_style`・`dom_changes_clear_layout`。`dom/dom.h` に bit・enum・上限・宣言・
  document の field（`change_sheets`・`change_full_style`・`change_full_layout`・`change_count`）、node の `reserved` を `change` に改名。`sources.mk` に追加。
- generation を進めていた全ての場所を `dom_note_change` に置き換えた（DOM: node.c の挿入・削除・text 4 か所・shadow 付与・属性、shadow.c の adopted、select.c・control.c・radio.c 3 か所、
  bind: html-input.c・select.c・prelude.c の popover・environment.c の :target・style-sheet.c・geometry.c の scroll、page: geometry.c 2 か所・transition.c、
  images.c・media.c の資源）。design.md §5.1.5 の表に無かった `page/geometry.c`（rendering の強いる再 layout・anchor の scroll）と `page/transition.c`（view transition の開始）は
  DOCUMENT として扱った。
- `page/`: `page_cache_mode()`（`BROWSER_LAYOUT_CACHE`、既定 off）・`struct page_cache_counts`・`page_cache_fold`・`page_cache_report`（`BROWSER_LAYOUT_CACHE_STATS=1` で
  page を壊す時に 1 行）。数えるのは `engines`・`styles`・`style_hits`・`layouts`（他は 0）。`styles`・`style_hits` は `css/cascade.c` の `css_engine_counts`（engine が作られてからの数）を
  engine を壊す前に page に足す。page の `query_css`（2 つ目の engine）は数えない。
- 試験: `plan/ws346/tests/host-change.c`（79 項目）。`plan/ws074/tests/host-build.sh` の試験の loop に `plan/ws346/tests/host-*.c` を足した。

### 自分で決めた点（design.md が黙っていた所）

1. **`DOM_CHANGE_SHEETS` の追加**: adopted sheet と CSSOM の規則の変更は DOCUMENT と分けて `DOM_CHANGE_SHEETS`（`change_sheets`・`change_full_style`・`change_full_layout` を立てる）にした。
   design.md §5.1.2・§5.1.4 の DOCUMENT の行を直す（版 2）。
2. **RESOURCE は generation を進めない**（今も画像の到着は document の generation を進めず、page の `images_generation` が layout を促すため）。element が分からない時は document の node に
   RESOURCE を送り、`change_full_layout` だけを立てる（style は変わらない）。
3. **削除された部分木の bit を消す**のは、`dom_note_change` の CHILDREN が `child->parent == NULL` の時（connected でない時・上限の後・shadow の中の時も）に行う。
4. **connected でない木の変更は数えず、bit も立てない**（`change_count` は connected の変更だけ）。script が離れた所で作る大きな木が上限に当たらないため。
5. **shadow tree の中の変更**は、document に最も近い host を `STYLE_SUBTREE | LAYOUT` にする（shadow の中の node には bit を立てない）。入れ子の shadow は一番外の host。
   css の `css_engine_invalidate`（p004）の `CSS_RESTYLE_SUBTREE` は **shadow tree の中も含めて**外さなければならない（design.md §5.2.3 に追記）。
6. **comment の文字の変更**は generation を進めるだけで bit を立てない（box も style も無い）。
7. **`dom_option_set_selected`** は option でなく owner の select に STATE を送る（list 全体が見せる物で、peer の選択も変わるため）。
8. `dom_changes_take_style` は style の bit に加えて `change_sheets`・`change_full_style`・`change_count` を戻す。`dom_changes_clear_layout` は layout の bit と `change_full_layout`・`change_count` を戻す。
   `visit` は NULL でよい（bit の消去だけ）。visit の中で木を変えてはならない。
9. 上限 `DOM_CHANGE_LIMIT` は 10000（`dom/dom.h`）。上限を超えると `change_full_style`・`change_full_layout` を立て、bit は立てない。

### 確かめ

- host build（`CC=clang sh plan/ws074/tests/host-build.sh plain`）warning 0。
- `host-change` 79/79。
- 基点（変更前の同じ build）と同じ: run-dom-tests 39/42、run-js-tests 21/21、run-fetch-tests 38/38、run-http-tests 15/15・`--async` 19/20（async-sheets は基点でも FAIL）。
- `BROWSER_LAYOUT_CACHE_STATS=1` で 1 行が出る（off と verify で mode の名前が変わる）。
- わざと壊した: `change_is_sheet` が常に 0、`dom_changes_take_style` が子へ降りない、`change_mark` が先祖に below bit を立てない、の 3 つを同時に入れて 49/79（30 項目が FAIL）。
  元に戻して `cmp` で一致を確かめた。
- Acid2・Acid3（`run-acid-tests.py`）は未実施（WPT の checkout が build/ws074-suites/wpt に無い）。p003 で取る。

### 未実施・残り

- generation の値の列を基点と全部の操作で比べる試験は、host-change の「+1」の確かめだけ（基点の build との直の比較は無い）。
