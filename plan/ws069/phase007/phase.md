<!-- awesome-plan project=zedbsd record=ws069p007 -->

# ws069-p007: i915 実機での GLX の間欠の止まり（BUG-057）の段の特定

Phase ID: `ws069-p007`
Parent: [WS069](../ws.md)
Status: uncleared（q485-i01、2026-09-27）
Phase disposition: canceled（2026-09-27 ユーザーの判断: Wayland 用の X server を単体の zdesktop-x11server として作り直し、Xzed は元に戻す。BUG-057 は新しい server の上で [ws069-p010](../ws.md) として調べ直す）
Queue: q485-i01
承認: 2026-09-26 ユーザーの自律実行の指示（デスクトップ関連、Wayland コンポジタの X11 と GLX を優先）
Bug: [BUG-057](../../bugs/BUG-057.md)

## 範囲

1. zgears に watchdog（SIGALRM。frame が 3 秒進まなければ、今の段（事象の読み・大きさ・描画・swap）と frame を async-signal-safe に stderr へ）。
2. Xzed に client の接続の状態の log（2 秒ごと、要求の読みか返答の書きが滞った client だけ: 入力 buffer の byte・未送の出力の byte・
   最後の要求からの時間・EAGAIN の数）。
3. 実機の run（`CAPTURE=zdesktop-x11 ZDESKTOP_APP=home`）を最大 4 回。止まった run で、zgears の段と Xzed の接続の状態から止まった側を決める。
4. 原因が X11 の範囲（Xzed・libX11・libGL の glx.c・zgears）で直せるなら直し、実機と Venus で確かめる。範囲外なら BUG-057 に記録。

## 受け入れ

1. 止まりが再現した run で、止まった段（zgears の側か Xzed の側か、どの呼び出しか）が log で分かる。4 回で再現しなければそう記録する。
2. 直したときは、実機の run で gears_turns を含む 6 検査が通り、Venus の x11-p005・x11-p004 が通る。
3. 実機の LCD の目視は未実施でよい。

## 依存

ws068-p006（cleared）、ws069-p005（cleared）。

## 結果（2026-09-27、q485-i01。canceled）

- zgears に watchdog（SIGALRM、`ZGEARS STALL frame= step= swap_step=`）、libGL の `glXSwapBuffers` に段の印（`zglx_swap_step`、
  libGL の非公開の export）。実機の run（`build/ws069-p007-hw-run1〜2/`）で止まった段は **swap の 5（`glx_pbuffer` の中の
  `XGetGeometry`、libGL の中の Xlib の写し）**: X server の返事を永久に待つ。Xzed の書き込みの失敗（`XZED WRITE_DROP`）は 0。
- libX11 の `XPending` の不具合を直した（非 blocking の recv で 32 byte 未満の event の断片を読んで捨て、以後の返事をずらしていた。
  今は MSG_PEEK で丸ごと揃ったときだけ取る）。Venus の x11-p004・x11-p005 は PASS。ただし実機の止まりはこれでは消えなかった（run2）。
- 次の手掛かり（p010 へ）: server 側で、止まった client の受信の状態（保持した byte、最初の要求が要る長さ、最後の要求からの時間）を出す。
  kernel の unix socket の取りこぼし（部分の配送や起床の取りこぼし）も疑う。
- canceled の理由: 2026-09-27 ユーザーの判断で X server を単体の zdesktop-x11server に作り直す（[design.md](../design.md) §0）。
  Xzed の上での調べは続けない。
