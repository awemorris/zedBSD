---
id: desktop.windows.arrange
title: desktop の pill のメニューで窓を整列し、title の drag で入れ替え、dock で整列モードが終わる
status: draft
areas: [compositor, windows]
paths: [userland/desktop/wayland/arrange.c, userland/desktop/wayland/arrange-shell.c, userland/desktop/wayland/shell.c]
machine: either
human: look
since: WS181-p004
---

## 目的
2026-10-07 の UAT（WS181）の整列のメニューと整列モード: pill のどこを tap してもメニューが開き（メニューは今の desktop に対して、desktop の切り替えは swipe・key だけ、ws181-p005）、7 つの形で今の位置から大まかに窓を枠へ置き、整列モードの title の drag は入れ替え、dock（double click・bar への drag）で整列モードが終わり、戻すと普通の floating になること。QEMU の自動の版は plan/tools/desktop-guest/ws181-guest.sh の C10〜C12。

## 準備
窓の mode で Files・Terminal・Calculator を開く（3 つとも floating）。

## 操作と確認
1. 操作: bar の desktop の pill を click。
   確認事項: メニュー。正解: pill の所から小さく濃い白で現れ、約 0.2 秒で広がりながら透けていき（ws181-p007）、pill の下に窓の panel と同じ frosted の glass の popup（後ろの窓と壁紙がぼけて透ける。設定の frosted を切ると不透明の白）。整列の 7 つの絵だけが 2 列（1 行目 左右に並べる・上下に並べる、2 行目 左に 1 列＋右の縦分割・右に 1 列＋左の縦分割、3 行目 上に 1 行＋下の横分割・下に 1 行＋上の横分割、4 行目 格子、文字と desktop の絵は無い）。log `KWL ARRANGE menu open`、`KWL ARRANGE menu item=<形> x=… y=…` が 7 行、広がり終わると `KWL ARRANGE menu settled`。確認方法: log、撮影（人が見る）。
2. 操作: 「右に 1 列＋左の縦分割」の絵（2 行目の右）を click。
   確認事項: 枠。正解: 右半分に 1 つ、左半分に 2 つが縦に、各窓は元の位置に近い枠へ滑って入り、枠の大きさに描き直る。pill に整列の印は出ない。log `KWL ARRANGE apply layout=right-main desktop=2 windows=3 slots=…`、各窓の `KWL GLASS resized … width=W height=H` が最後の `KWL CONFIGURE` の大きさと同じ。確認方法: log、撮影。
3. 操作: 左上の窓の title を右の枠へ drag して離す。
   確認事項: 入れ替え。正解: 2 つの窓が枠を入れ替える。log `KWL ARRANGE swap a=… b=… slots=1,0`。確認方法: log、撮影。
4. 操作: どれかの窓の title を double click。
   確認事項: 整列の終わり。正解: その窓が最大化、log `KWL ARRANGE end desktop=2 reason=dock` と `KWL LAYOUT mode=docked`。確認方法: log。
5. 操作: bar の title を double click（最大化を戻す）。
   確認事項: 普通の floating。正解: 全部の窓が枠の場所のまま floating（`KWL LAYOUT windows … mode=windowed … docked=0`）、title の drag は普通の移動（`KWL GLASS moved`、`KWL ARRANGE swap-start` は出ない）。確認方法: log。
6. 操作: pill の右から 2 つ目の点を click。
   確認事項: desktop は切り替わらない。正解: メニューが開くだけ（`KWL ARRANGE menu open`、`KWL GLASS desktop=` の行は増えない）。Ctrl+Alt+Right で右の desktop（`KWL GLASS desktop=3 via=key`）。確認方法: log。

## 合格
各段の log の行と、撮影で枠が重ならず隙間が揃うこと（needs-person）。
