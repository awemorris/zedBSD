<!-- awesome-plan project=zedbsd record=ws035p065 -->

# ws035-p065: 仮想デスクトップの実体と左右の端のスワイプ

Phase ID: `ws035-p065`
Parent: [WS035](../ws.md)
Status: cleared（q481-i01、2026-09-26）
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

## 結果（2026-09-26、q481-i01）

cleared。受け入れ 1〜3 を満たした。

### 実装（`userland/base/zwl`）

- 窓（`zwl_object`）に `desktop`、map したときの `server->desktop`（display.c）。`zwl_top_window`・`window_at`・Wiseview の一覧は
  今のデスクトップの窓だけ（focus・入力・docked の題名もそれに従う）。
- 切り替え（shell.c `desktop_turn`）: 220 ms の横の移動（cubic ease-out）、終わると `ZWL GLASS desktop settled`。先の一番上の窓に
  focus（`front_surface` と `zwl_seat_focus`）。バーの絵の click、Ctrl+Alt+←/→（`zwl_glass_key`、seat.c から Home の後）、
  左右の端 16 px からの swipe（12 px 動いてから、窓が 1:1 で横へ、隣が無い側は 1/4 の抵抗、画面幅の 25 % で切り替え、届かなければ戻る）。
  壁紙は動かず、窓は layer の変換で横へ（Home が開いている間は今のデスクトップだけ、Home の layer のまま）。
- バー: 今のデスクトップの絵が濃く青い輪、窓のあるデスクトップの絵の下に青い点。起動時に絵の位置を log（試験用）。

### 検証（QEMU・Venus。i915 実機は未実施）

- `plan/ws035/tests/zdesktop-p065.sh` PASS（build/ws035-p065.log）: デスクトップ 1 の赤い窓（desk1.png）、バーの絵で 2 へ、そこで map した
  青い窓だけ（desk2.png）、左端からの swipe の途中で 2 つのデスクトップの窓が横に並ぶ（swipe.png）、1 へ戻る（back1.png）、Ctrl+Alt+→ で
  2（key2.png）、短い swipe は 2 のまま、Ctrl+Alt+← で 1（窓 1 つ）。
- 回帰: zdesktop-p059・p062・p063・p069・p071 PASS。build warning 0。style-check: shell.c・display.c 0、seat.c 3（HEAD と同じ）。
- boot test PASS（build/ws035-p065-boot/login.png）。

### 残り

窓を別のデスクトップへ移す（Wiseview のタイルをバーの絵へ drag）、Wiseview の中での左右のデスクトップ移動、touchpad の 3 本指、
速度での判定。
