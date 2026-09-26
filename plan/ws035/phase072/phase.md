<!-- awesome-plan project=zedbsd record=ws035p072 -->

# ws035-p072: 窓の最小化と、窓をデスクトップ間で移す

Phase ID: `ws035-p072`
Parent: [WS035](../ws.md)
Status: in-progress（q483-i01）
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
