# 緊急のラップアップ（2026-10-08 02:27、ユーザーの指示「緊急でラップアップしてください。全サブエージェントです。」）

全サブエージェント（P1・P2・BUG-258 の bug-analyzer）はユーザーが停止した。T1 は先に終了済み（T1-375b・T1-376・T1-377 PASS、台帳 cef44ad67 は main に merge 済み）。

## 再開の情報

| 担当 | 状態 | 再開 |
| --- | --- | --- |
| P1（q862） | agent/p1 43d35865a（main に未 merge、途中）: ws090-p015 の libkeiland の kl_scroll の拡張（scroll.c・keiland.h・exports.map・phase.md）。範囲 1 の途中、Terminal・Notes の移行と試験は未着手。未 commit の差分は無し | **先に** ws113-p015 の 2 番目以降の display の dock bar（2026-10-08 ユーザーの決定、phase015/phase.md の末尾）→ その後 ws090-p015 を agent/p1 の続きから |
| P2（q860） | agent/p2 e5c38be2b に未 commit の WS143 p002 の作業（usb-bt.c・bt-hci.c・bt-hci-proto.c・bluetooth.h・host 試験・phase002/phase.md、Makefile 等の登録）。Q1 が [plan/ws143/wip-20261008/](../ws143/wip-20261008/) に保存（tracked.patch = `git diff --binary`、untracked.tar.gz）。worktree の差分はそのまま残っている | **先に** q863 ws183-p002（タップのクリックの遅れ）→ WS143 p002 の続き。T1-378（5330 の Linux で Bluetooth の情報）は保留 |
| bug-analyzer（BUG-258） | 解析の途中で停止、結果無し | BUG-258.md の「次」から解析をやり直す |

## 5330（zedBSD 単独起動中）

- 入れ替えは**未実施**（最後の操作をユーザーが止めた）。/tmp/vmunix.q1（main fc5cad676、BUG-256 の診断 594eb28ef 入り、cksum 4151178383）・/tmp/wayland.q1・/tmp/settings.q1・/tmp/libkeiland.q1 を 5330 の /tmp に置いただけ。ESP（/dev/sda1）は /tmp/esp に mount したまま。/esp/vmunix は 2026-10-07 15:07 の物のまま。
- ユーザーの許可（2026-10-08）:「5330はいつでも再起動OKです。アップデートもOKです。」

## ラップアップの直前のユーザーの UAT

- PDF viewer の窓のリサイズのドラッグが重い:「リサイズイベントごとになんらかの重い処理をしているように見えて、レスポンスが悪い。毎イベントに完全な再描画や内部リサイズ処理をしないで、200msごとに内部リサイズをするとかはどうだろう。あとは、重い処理が不要な処理であればそもそも省くとか。たぶん画面描画の矩形が変わるだけの実装で十分なのに、内部バッファを作り直してソフトウェアレンダリングをやり直している気がする。」→ backlog（未割当）。
- 5330 の USB メモリ（BUG-258）、タップの遅れ（ws183-p002）、2 番目の display の dock bar の決定（ws113-p015）。
