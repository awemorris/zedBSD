<!-- awesome-plan project=zedbsd record=ws346-p002 -->

# ws346-p002: DOM の変更の記録・mode と計数の土台

Status: planned
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: 未割り当て（Q1 が Sonnet = phase-runner-low に渡す時に決める）
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

（実装の担当が書く: 自分で決めた点・数・未実施）
