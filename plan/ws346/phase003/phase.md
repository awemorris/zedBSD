<!-- awesome-plan project=zedbsd record=ws346-p003 -->

# ws346-p003: 照合（verify）の道具と bench

Status: planned
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: 未割り当て（Q1 が Sonnet = phase-runner-low に渡す時に決める）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

`BROWSER_LAYOUT_CACHE=verify` の layout の照合の骨組み（§5.5.2 の layout の部分）と、速さの bench（§8）を作る。
まだ差分の計算は無いので、`verify` は「同じ build を 2 回して比べる」形になり、違いは 0 のはず（道具の自己検査）。

## 書く物

- `page/page.c`（`page_layout_work` の後に、mode が `verify` の時だけ scratch の木へ `layout_build` して `layout_dump` を比べ、違いを報告・計数）。
  比べる時、両方の木は同じ入力（style の engine・viewport・scroll・transition）で作る。違いの報告の形は §5.5.2 の通り。
- `plan/ws346/tests/bench/restyle.html`・`plan/ws346/tests/bench/restyle.sh`（§8 の 4 項目。`plan/ws074/tests/bench/bug411.html`・`bug411.sh` の形）。
  各項目は `bench <名前> <ms> <check>` を console に出す。check は結果の値（例 offsetWidth の合計）で、mode で変わらないこと。
- `plan/ws346/tests/run-verify.sh`: 渡した browser で §6.2 の試験の組を `BROWSER_LAYOUT_CACHE=verify` と `off` で流し、PASS の数と
  `mismatches` の合計を並べて出す（live の頁は含めない）。

## 確かめ

- `verify` で §6.2 の試験の組の `mismatches=0`、PASS の数が `off` と同じ。わざと壊す: 照合の比較で 1 行目を書き換える一時の改変で違いが報告されること。
- `sh plan/ws346/tests/bench/restyle.sh build/ws074-host/plain/browser` が 4 項目を出す。`off` の基準の数を「結果」に書く。

## 完了の条件

上の全部。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に §5.5.2・§8）。
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
