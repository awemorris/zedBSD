<!-- awesome-plan project=zedbsd record=ws346-p005 -->

# ws346-p005: style の無効化の統合

Status: cleared（2026-11-03 Q1。pages 4/4 が off・on・verify で一致、verify の組で差異 0、ASan で pages 4/4、bench 4 が 701 → 44 ms、わざと壊して検出）
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: [q2003](../../queue.md)（Q1 が直に実装、2026-11-03 ユーザーの指示）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

mode が `on`・`verify` の時、DOM が変わっても sheet の集合が同じなら engine を保ち、変更の記録から無効化する（§5.2.4）。
`verify` の style の照合（§5.5.2）を足す。

## 書く物

- `page/page.c`（`page_update_styles`）、削除の hook（§5.2.4 の 4）、`css/cascade.c`（style の照合の口）、計数の `invalidated` と `mismatches`。
- 計算済みの style が指す string の寿命を調べ（design.md §10）、GC で回収され得る物があれば対策し、結果に書く。
- 試験の頁 `plan/ws346/tests/pages/restyle-*.html`（class・id・style 属性・兄弟の結合子・:nth-child・:empty・:checked・:has()・挿入・削除・text・
  `<style>` の text の変更・`<link>` の追加）。各頁は変更の後の `getComputedStyle` の値を console に出し、`run-verify.sh` の組に足す。

## 確かめ

- `sh plan/ws346/tests/run-verify.sh build/ws074-host/plain/browser`: `mismatches=0`、PASS の数が `off` と同じ。
- ASan の build（`host-build.sh asan`）でも同じ組（時間がかかれば DOM・JS・pages だけ）。
- bench（restyle.sh）の項目 4 が `off` の 10 倍以上速い。項目 1〜3 の数も書く。
- 計数: bench の頁で `engines` が 1〜2。

## 完了の条件

上の全部。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に §5.2.4・§5.5.2 の style の部分・§10 の GC の行）。
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

2026-11-03 Q1（main の checkout で実装、p004 の main `af0251ef21` の上）。

### したこと

- `page/page.c`:
  - `page_update_styles`: cache が on・verify で、engine があり、sheet が変わっていない（`change_sheets`・`change_full_style` が 0、`styled_visits`・`styled_sheets` が同じ）、engine が大きすぎない（`css_engine_arena_bytes` が 32 MiB 以下、`css_engine_kept_styles` が 100000 以下）時は engine を保ち、`dom_changes_take_style` で bit のある node を回って `css_engine_invalidate` する（`page_style_visit`。style の bit は何でも部分木の無効化にし（子が継承するため）、`STYLE_SIBLINGS` があれば後の兄弟も、document の node なら全部）。それ以外は今どおり `page_gather_styles`（engine の作り直し）。
  - `page_gather_styles`: 今の記録を `dom_changes_take_style(…, NULL, NULL)` で消し、cache が off でなければ document の hook（`change_scope`・`change_removed`）を登録する。`page_destroy` で hook を外す。
  - `page_change_scope`（engine が無い時は SUBTREE と SIBLINGS、あれば `css_engine_change_scope`）、`page_change_removed`（document から出た部分木の style を外す）。
  - `page_layout` の成功の後に `dom_changes_clear_layout`。
  - `page_layout_verify`（p003）を、**何も保たない新しい engine**（`page_engine_fresh`）で全体の木を作る形に直した。違えばその木と engine が page の物になる。
- `dom/dom.h`・`dom/change.c`: document の `change_removed` hook（connected な document から出た部分木を知らせる）。
- `css/cascade.c`・`css/css.h`: `css_engine_arena_bytes`（custom property の style が arena に取った byte）、`css_engine_kept_styles`、`class` の語が 1 つも無い engine で `items + first` が NULL に 0 を足す既存の未定義動作（2026-10-03 から）の直し。
- `plan/ws074/tests/host-build.sh`: asan の variant で clang の時 `-shared-libsan`（共有 library に静的な runtime を入れられず link が通らなかった）。
- 試験: `plan/ws346/tests/pages/restyle-{basic,has,shadow,sheets}.html`（変更の後の `getComputedStyle` と `offsetWidth`・`offsetHeight` を出す。class・id・style 属性・兄弟の結合子・`:nth-child`・`:first/last-child`・`:empty`・`:checked`・`:disabled`・属性 selector・`aria-*`・`role`・`tabindex`・`:lang`・継承・`attr()`・移動・削除と同じ id の新しい element・`:has()`・`:nth-child(of S)`・shadow tree と `:host`・`<style>`/`<link>`・CSSOM・adopted sheet など約 220 の確認）と `plan/ws346/tests/run-pages.py`（off・on・verify の出力が同じか、verify が差異を言わないか）。

### 自分で決めた点

1. **style の bit は何でも部分木の無効化**にした（design.md の `STYLE`＝element だけ、は使わない）: element の style が変わると子が継承した値が古くなるため。`:empty` のための text の変更も親の部分木になるが、親は普通は小さい。
2. **hook は document の node の 1 つ**（page ごとに登録、cache が off の時は付けない）。
3. **GC**: 計算済みの style が指す `vm_string` は全部 atom（`vm_atom_from_*`、heap と同じ寿命）で、custom property の token は engine の arena に写してあるので、engine を長く保っても回収された string を指さない（`css/values.c`・`cascade_keep_tokens` を読んで確かめた）。
4. **arena と保つ style の上限**: custom property の style は再計算のたびに engine の arena を増やす（engine を壊す時にだけ解放）ので、32 MiB を超える、または保つ style が 100000 を超えると engine を作り直す。実際の頁での増え方は p008 で amazon で測る（未実施）。
5. 削除された部分木は `change_removed` で外す（同じ address に新しい element ができても古い style を返さないため）。
6. ASan を走らせるには `LD_LIBRARY_PATH=/usr/lib/llvm-19/lib/clang/19/lib/linux` と `ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0` が要る。

### 確かめ

- host build（clang）warning 0。
- `run-pages.py`: 4/4（off・on・verify の出力が行ごとに一致: restyle-basic 607 行・has 100・shadow 60・sheets 51、verify の差異 0）。例（restyle-basic）: engine の作り直し off 39 → on 2、style の計算 876 → 365、外した style 342。
- `run-verify.sh`（dom・js・fetch・WPT reftest 100 件・Acid）: off と verify で同じ（dom 39/42、js 21/21、fetch 38/38、WPT 62/37/1、Acid3 100/100 完全一致）、**差異 0**。
- `host-change` 88/88、`host-restyle-scope` 52/52。
- ASan（clang 19、`-fsanitize=address,undefined`）: pages 4/4、host-change 88/88、host-restyle-scope 52/52、js 21/21。dom-tests は 38/42: `computed`・`exception` は plain と同じ FAIL、`markup` は `dom/serialize.c:519` の既存の未定義動作（null への 0 の加算、この変更とは無関係で未修正）で落ち、plain で FAIL の `performance-observer` が ASan では通る。
- bench（`restyle.sh`、3 回の中央値、`-O1`）: off と on の check は同じ。toggle-then-computed **701 → 44 ms（16 倍）**。class-then-offset 14080 → 11387、style-then-offset 14020 → 11336、append-then-rect 14229 → 12121 ms（layout が支配的で、p007 の対象）。
- わざと壊した（2 つ同時）: 後の兄弟を無効化しない、`css_engine_invalidate` の SUBTREE が element だけを外す → 4 頁のうち 3 頁が FAIL（継承した色・nth-child・兄弟の結合子・shadow の値が違う）、`verify` が差異を報告した。元に戻して `cmp` で一致を確かめ、build し直して 4/4。1 回目の変異は未使用の引数が `-Werror` で止めたので、引数を使う形にして入れ直した。

### 未実施

- live の頁（amazon ほか）の verify（p007 の後でまとめて、design.md §12 の回数の中で）。
- amazon での arena の増え方（p008）。
- scripted reftests（`run-interop2025-scripted-reftests.py`）の verify。
