<!-- awesome-plan project=zedbsd record=ws035p065 -->

# ws035-p065: 仮想デスクトップの実体と左右の端のスワイプ

Phase ID: `ws035-p065`
Parent: [WS035](../ws.md)
Status: in-progress（q481-i01）
Phase disposition: normal
Queue: q481-i01
承認: 2026-09-26 ユーザーの自律実行の指示（デスクトップ関連を優先）。ws.md の planned の Phase。
設計: [wiseman-design.md](../wiseman-design.md)（画面の左端・右端から内側へ = 前・次の仮想デスクトップ、画面が追従、端から 16 px、
距離 25 % と速度、Ctrl+←/→）

## 範囲

1. 仮想デスクトップ 4 つ。窓は map したときのデスクトップに属する。見える窓・入力・focus・Wiseview・docked の題名は今のデスクトップの窓
   だけ（`zwl_top_window` と `window_at` と Wiseview の一覧が絞る）。
2. 切り替え: 上部バーのデスクトップの絵の click、Ctrl+Alt+←/→、画面の左端・右端（16 px 以内）から内側への drag（窓が指に 1:1 で横へ
   動き隣のデスクトップの窓が覗く、画面幅の 25 % で切り替え、届かなければ戻る、隣が無い側は抵抗）。220 ms の横の移動。壁紙は動かない。
3. バーの絵: 今のデスクトップが濃く、窓のあるデスクトップに小さな点。
4. 切り替えた先の一番上の窓に focus。
5. Home を開いている間・Wiseview の間は端の swipe を取らない。

## 受け入れ

1. Venus で 2 つの窓を別のデスクトップに置き、絵の click・キー・端の drag で切り替えて、それぞれのデスクトップで自分の窓だけが見える
   （画面）。drag の途中で 2 つのデスクトップの窓が並ぶ。
2. p059・p062・p063・p069・p071 の試験が通る。build warning 0、style-check は悪くしない。
3. i915 実機は未実施でよい。
