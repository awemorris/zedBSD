<!-- awesome-plan project=zedbsd record=ws346-p005 -->

# ws346-p005: style の無効化の統合

Status: planned
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: 未割り当て（Q1 が Sonnet = phase-runner-low に渡す時に決める）
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

（実装の担当が書く: 自分で決めた点・数・未実施）
