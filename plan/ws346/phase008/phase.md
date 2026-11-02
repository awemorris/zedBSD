<!-- awesome-plan project=zedbsd record=ws346-p008 -->

# ws346-p008: 統合の試験・速さの判定・既定の mode の案

Status: planned
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: 未割り当て（Q1 が Sonnet = phase-runner-low に渡す時に決める）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

design.md §6.2 の全部と §8 の目標を確かめ、既定の mode の案を Q1 に出す。live の頁は Q1 が流す（担当は道具と手順を用意する）。

## 書く物

- 結果の記録（この phase.md）と、足りない試験の追加。不具合が見つかったら直してよい（範囲は p002〜p007 の file）。

## 確かめ

- §6.2 の全部（`verify`、ASan を含む）。Windows の cross-build（`make browser-win64-zip LLVM_MINGW=$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64`）warning 0。
- §8 の bench の表（`off`・`on`、3 回の中央値）。

## 完了の条件

上の全部と、既定の mode の案（根拠の数字つき）。ユーザーの判断は Q1 が聞く。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に §6・§8・§12）。
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
