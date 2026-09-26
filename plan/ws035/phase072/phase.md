<!-- awesome-plan project=zedbsd record=ws035p072 -->

# ws035-p072: 窓の最小化と、窓をデスクトップ間で移す

Phase ID: `ws035-p072`
Parent: [WS035](../ws.md)
Status: cleared（q483-i01、2026-09-26）
Phase disposition: normal
Queue: q483-i01
承認: 2026-09-26 ユーザーの自律実行の指示（デスクトップ関連を優先）
設計: [wiseman-design.md](../wiseman-design.md)（Wiseview: 上部の仮想デスクトップ列へタイルを drag で移動）。最小化の button は
p059 から「まだ動作なし」。p065 の残り。

## 範囲

1. 最小化: 浮いた題名の bar と docked の題名（バー）の最小化の button で窓を隠す（描かない、focus・入力の対象外）。Wiseview には
   薄いタイルで残り、選ぶと戻って前面に。
2. デスクトップ間の移動: Wiseview のタイルを drag して上部バーのデスクトップの絵で離すと、その窓がそのデスクトップへ（Wiseview は開いた
   まま、タイルは消える）。Ctrl+Alt+Shift+←/→ で前面の窓を隣のデスクトップへ移し、表示もそこへ。
3. Wiseview のタイルの選択は press でなく release（動かさなかったとき）。

## 受け入れ

1. Venus で最小化の button で窓が消え、Wiseview で薄いタイルとして選べ、戻る（画面と log）。
2. Wiseview のタイルをデスクトップ 2 の絵へ drag すると窓が移り、デスクトップ 2 で見える。キーでも移る。
3. p059・p062・p063・p064・p065 の試験が通る。build warning 0、style-check 0。

## 結果（2026-09-26、q483-i01）

cleared。受け入れ 1〜3 を満たした。

### 実装（`userland/base/zwl/shell.c`、display.c、zwl.h）

- 最小化: 窓に `minimized`。浮いた題名の bar とバーの docked の題名の最小化の button で `window_minimize`（描かない、`zwl_top_window`・
  `window_at` の対象外、次の窓に focus、`ZWL GLASS minimize`）。Wiseview には白い wash で薄くしたタイルで残り、click で戻して前面に。
- Wiseview のタイル: 選択は release（8 px 動けば drag）。drag 中のタイルは指に付いて他の上に描かれ、上部バーのデスクトップの絵で離すと
  `window_to_desktop`（`ZWL GLASS move-desktop`、Wiseview は開いたまま）。Wiseview の中でバーの絵の click はデスクトップの切り替え。
- Ctrl+Alt+Shift+←/→: 前面の窓を隣のデスクトップへ移し、表示もそこへ。

### 検証（QEMU・Venus。i915 実機は未実施）

- `plan/ws035/tests/zdesktop-p072.sh` PASS（build/ws035-p072.log）: b の最小化で重なりが a の赤に（minimized.png）、Wiseview で b が薄い
  タイル（wiseview.png）、click で戻る（restored.png）。a のタイルをデスクトップ 2 の絵へ drag（dragging.png）して移し、2 で a（desk2.png）、
  Ctrl+Alt+Shift+← で a を 1 へ（desk1.png、`desktop settled desktop=1 windows=2`）。
- 回帰: zdesktop-p059・p062・p063・p064・p065 PASS（新しい image で）。build warning 0、shell.c・display.c の style-check 0。
- boot test PASS（build/ws035-p072-boot/login.png）。
