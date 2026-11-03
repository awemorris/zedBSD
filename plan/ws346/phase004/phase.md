<!-- awesome-plan project=zedbsd record=ws346-p004 -->

# ws346-p004: selector の特徴の表と変更の影響の範囲

Status: cleared（2026-11-03 Q1。host build warning 0、host-restyle-scope 52/52、host-change 88/88、既存の試験は基点と同じ、わざと壊して 10 項目が FAIL）
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: [q2003](../../queue.md)（Q1 が直に実装、2026-11-03 ユーザーの指示）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

engine が sheet を加える時に selector の特徴を集め（§5.2.1）、`css_engine_change_scope`（§5.2.2）と `css_engine_invalidate`（§5.2.3）を実装する。
page からはまだ呼ばない（p005 で統合）。

## 書く物

- `css/cascade.c`・`css/css.h`（必要なら `css/internal.h`・`css/index.c`）。
- 試験: `plan/ws346/tests/host-restyle-scope.c`。規則の組（sheet の text）と変更（種類・属性・element）の表で、`css_engine_change_scope` の期待の値を手で決めて確かめる。
  少なくとも: 名指されない属性 → NONE、class → SUBTREE、`.a + .b` の時の class → SUBTREE|SIBLINGS、`:has()` → ALL、`style` → SUBTREE、
  状態の擬似 class の有無、`[data-x]` が名指された時、`attr()`、`:lang()` と `lang` 属性。
  `css_engine_invalidate` は、entry が外れた element だけが計算し直されることを、計算の回数で確かめる（計数の口が要れば `css/internal.h` に試験用の static でない関数を足してよい）。

## 確かめ

host build warning 0、`host-restyle-scope` PASS、既存の css の試験（`plan/ws074/tests/host-css-*`・`host-cssom*` ほか、`run-wpt-reftests.py` 既定の 100 件）が基点と同じ。

## 完了の条件

上の全部。p003 と並べて動いてよい（file が重ならない）。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に §5.2.1〜§5.2.3）。
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

2026-11-03 Q1（main の checkout で実装、p003 の main `5031ad9c61` の上。code の大半はユーザーの snapshot commit `4992585ac8` に入った）。

### したこと

- `css/cascade.c`: 
  - `cascade_features`（engine の sheet の全 rule の selector と `@scope` の root・limit を再帰で見て、`:has()`・兄弟の結合子・`:nth-*(of S)` の有無を、最初に聞かれた時に 1 回求め、sheet を足すと求め直す）。
  - `css_engine_change_scope(engine, element, kind, attribute)`（design.md §5.2.2 版 3）。
  - `css_engine_invalidate(engine, element, scope)`（NONE は何もしない、SUBTREE は element と子孫と shadow tree の中、SIBLINGS は element と後の兄弟とその部分木、ALL は全部。あまり深い木は全部を外す）。
  - style の表から 1 つ外す `cascade_style_drop`（open addressing の表の **backward shift** で、同じ run の後の entry を失わない）。`css_engine_counts` に外した数を足した（`stat_dropped`）。
- `css/parser.c`・`css/internal.h`: sheet が名指す属性の token の表を `data-*` から `data-*`・`aria-*`・`role`・`tabindex` に広げた（名指されない時だけ NONE にするため）。
- `css/css.h`: `CSS_RESTYLE_*`（`DOM_RESTYLE_*` と同じ値）と 2 つの関数の宣言。
- `dom/dom.h`・`dom/change.c`: `DOM_RESTYLE_*` と document の `change_scope`・`change_scope_context`（hook）。ATTRIBUTE・STATE の style の bit は hook の答えで決まる（hook が無ければ SUBTREE と SIBLINGS の両方）。
- `page/page.c`: `page_cache_fold` が外した数も足す。
- 試験: `plan/ws346/tests/host-restyle-scope.c`（52 項目: scope の表 37、無効化 9、衝突 3、sheet を後から足す 3）、`host-change.c` に hook の 9 項目（88 項目になった）。

### 自分で決めた点（design.md が黙っていた所・版 3 に反映）

1. **change_scope の hook を dom に置く**: bit に kind と属性の名前が無く、take_style の時に css へ問えないため、dom_note_change の時に engine が決める（design.md §5.2.4 の 3'）。
2. **STATE は常に SUBTREE**: user agent の sheet が `:link`・`:popover-open` を見るので、「状態を見る selector が無い」engine は無い。
3. **NONE にする属性は aria-*・data-*・role・tabindex だけ**（名指されない時）。他の属性（width・height・align・bgcolor など presentational hint、href、lang、dir、class、id、style）は常に SUBTREE（`cascade.c` が読む属性の全部を表にして保てないため）。
4. **兄弟の結合子**: engine にそれが 1 つでもあれば SIBLINGS を足す（結合子の左の特徴までは見ない、安全側）。
5. **`CSS_RESTYLE_SIBLINGS` だけの scope でも element 自身を外す**。
6. **危険（p005 で扱う）**: element の custom property を engine の arena に写す style は、再計算のたびに arena が増える（engine を作り直す時にだけ解放）。p005 で測って上限を決める（design.md §10 に足した）。
7. `data-*` の未指名は DOM 側では quiet（記録しない）なので hook には来ないが、css 側も NONE にして揃えた（最初の版は NONE にせず試験が 1 件 FAIL したので直した）。

### 確かめ

- host build（clang）warning 0。
- `host-restyle-scope` 52/52、`host-change` 88/88。
- 基点と同じ: run-dom-tests 39/42、run-js-tests 21/21、run-fetch-tests 38/38、run-http-tests 15/15・`--async` 19/20（async-sheets は基点でも FAIL）。
- わざと壊した（3 つを同時）: backward shift を止める、`:has` を見逃す、shadow tree の中を探さない → 52 項目のうち **10 項目が FAIL**（`:has` 4 項目と、scope の再計算、siblings、host の subtree、衝突の 3 項目ほか）。元に戻して `cmp` で一致を確かめ、build し直して 52/52。
  1 回目の変異は未使用の変数が `-Werror` で止めたので、変数を使う形にして入れ直した。

### 未実施

- page からの呼び出し（hook の登録・take_style の visitor・engine を保つ判定）は p005。
