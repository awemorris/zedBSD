<!-- awesome-plan project=zedbsd record=ws346-p004 -->

# ws346-p004: selector の特徴の表と変更の影響の範囲

Status: planned
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: 未割り当て（Q1 が Sonnet = phase-runner-low に渡す時に決める）
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

（実装の担当が書く: 自分で決めた点・数・未実施）
