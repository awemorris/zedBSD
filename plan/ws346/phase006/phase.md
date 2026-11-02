<!-- awesome-plan project=zedbsd record=ws346-p006 -->

# ws346-p006: layout の再利用 A: 幾何の保存・根の候補・intrinsic の持ち越し

Status: planned
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: 未割り当て（Q1 が Sonnet = phase-runner-low に渡す時に決める）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

接ぎ木の前の準備。(1) 後の段が書き換える前の相対の幾何を box に残す（§5.3.3）、(2) 再利用の根の条件（§5.3.2）を判定して計数 `roots` に数える
（まだ接ぎ木はしない）、(3) intrinsic の大きさを前の木から持ち越す（§5.3.5、これは実際に使う）。

## 書く物

- `layout/layout.h`（`struct layout_box` に足す field と、後の段が書く field の一覧の comment）、`layout/box.c`・`layout/block.c`（入口で入力の組を記録）。
- `layout_build_reusing` の宣言と、`previous` を受けて持ち越しと根の判定をする所まで（§5.3.1）。`page/page.c` から mode が `on`・`verify` の時に呼ぶ。
- 試験の頁 `plan/ws346/tests/pages/reuse-*.html`（根になる・ならない場合を §5.3.2 の項目ごとに 1 つ以上。overflow・flow-root・flex item・float・absolute の
  containing block の外・anchor・sticky・container query・縦書き）。`BROWSER_LAYOUT_CACHE_STATS=1` の `roots` で確かめる。

## 確かめ

- `run-verify.sh` で `mismatches=0`（持ち越しが結果を変えないこと）。
- ASan の build で pages と DOM の試験。
- 計数 `roots`・`intrinsic` が頁ごとに期待の通り（期待は手で決めて結果に書く）。

## 完了の条件

上の全部。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に §5.3.2・§5.3.3・§5.3.5）。
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
