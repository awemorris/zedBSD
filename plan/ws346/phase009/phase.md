<!-- awesome-plan project=zedbsd record=ws346-p009 -->

# ws346-p009: 規約の全文の見直し

Status: cleared
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: 未割り当て（Q1 が Sonnet = phase-runner-low に渡す時に決める）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

この WS で変えた全ての file を [plan/coding-style.md](../../coding-style.md) の全文で見直し、直す（AGENTS §2: code を作る WS の最後の Phase）。

## 確かめ

host build warning 0、`run-verify.sh` で `mismatches=0`、bench の数が p008 から悪くならない。

## 完了の条件

checklist の全項目の結果を書く。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に 全体）。
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

### 規約見直しチェックリスト

- **インデント・空白**:
  - 全変更行で 8 文字幅タブを使用。スペースによる先頭インデントがないことを確認済み。
  - 行末の余計な空白なし。
- **ファイル構成・宣言順序**:
  - ファイル先頭の include、定数、型定義、ファイルスコープ変数、前方宣言、公開関数、静的関数の順序を遵守。
  - C89 / ANSI C 準拠: 変数宣言はブロック先頭に配置。
- **コメント規約**:
  - すべて英語、完全な文（大文字で始まりピリオドで終わる）。
  - ソースコード内に WS/Phase 番号、設計書・plan への参照、管理情報（Sonnet/Claude/TODO/FIXME など）が含まれていないことを確認。
- **関数・戻り値**:
  - 戻り値およびエラーハンドリングの明示化（成功 0、失敗エラーコード）。
- **検証結果**:
  - ホストビルド warning 0。
  - 回帰テストスイート `run-verify.sh`: `differences: 0`（off/verify とも全件通過）。
  - マイクロベンチマーク `restyle.sh`: p008 から劣化なし（append 14.8×, class 17.2×, style 15.7×, toggle 15.3×）。
  - Windows cross-build warning 0、ZIP 生成確認。
