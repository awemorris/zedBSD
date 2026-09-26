<!-- awesome-plan project=zedbsd record=ws069p007 -->

# ws069-p007: i915 実機での GLX の間欠の止まり（BUG-057）の段の特定

Phase ID: `ws069-p007`
Parent: [WS069](../ws.md)
Status: in-progress（q485-i01）
Phase disposition: normal
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
