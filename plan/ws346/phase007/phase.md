<!-- awesome-plan project=zedbsd record=ws346-p007 -->

# ws346-p007: layout の再利用 B: 部分木の接ぎ木

Status: planned
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: 未割り当て（Q1 が Sonnet = phase-runner-low に渡す時に決める）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

根の条件と入力の組が合う時、前の木の部分木を新しい木へ写して layout を飛ばす（§5.3.4）。page の統合（§5.3.6）を終える。

## 書く物

- `layout/`（接ぎ木の深い写し、相対の幾何への戻し、後の段の field の初期化）、`page/page.c`（layout の bit の消去、container query の段の扱い）。
- 計数 `grafts`・`graft_boxes`。
- 試験の頁を p006 の reuse-*.html に足す（同じ頁で DOM を変えて幾何を読む繰り返し。根の外の変更・根の中の変更・根の大きさが変わる変更）。

## 確かめ

- `run-verify.sh` で `mismatches=0`、PASS の数が `off` と同じ。ASan の build で pages・DOM・JS。
- bench: 項目 1・2 が `off` の 10 倍以上、項目 3 が 3 倍以上（届かなければ理由と測った内訳を書く）。
- 前の木の arena を指す pointer が残らないこと（ASan で、前の木の解放の直後に新しい木を dump・描画する試験）。

## 完了の条件

上の全部。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に §5.3.1・§5.3.4・§5.3.6）。
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
