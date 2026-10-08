<!-- awesome-plan project=zedbsd record=ws102-p010 -->

# ws102-p010: L3 の計測の道具と基準値（遅れ・開く動き）

Parent: [WS102](../ws.md)
Status: in-progress（2026-10-08 夜 P1 q913: 道具を実装・build、基準値の計測を T1 へ）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q913（P1、Q1 の投入「WS102 p010・p011（画面 keyboard の L3 の計測と直し）。phase.md を立てて計測（host か QEMU の T1）と直し」）
設計: [design.md](../design.md) §3 の L3: (a) key の離しから compositor の送出まで p95 ≤ 5 ms、app の frame まで p95 ≤ 50 ms（QEMU の Venus）、
(b) 開く動きの frame の間隔が全て ≤ 20 ms。log の時刻の中央値と p95（3 回）。

## 範囲と設計

- compositor（`userland/desktop/wayland/keyboard.c`）が測って log に出す:
  - `KWL OSK latency send_us=N`: panel の press の離し（`keyboard_panel_release`、pointer と指の両方の入口）を受けた時から、その離しで key を
    送った時（`keyboard_send_key` の press・release の送出の後、`keyboard_send_commit` の commit の後）まで、CLOCK_MONOTONIC の µs。
    離しの後で遅れて送る物（手書きの候補・予測の候補）は測らない（離しの処理の終わりで印を消す）。
  - `KWL OSK latency frame_us=N`: 同じ離しから、key を送った先の client（その時の focus の surface の client）が次に buffer 付きの
    `wl_surface.commit` をするまで（`kwl_keyboard_surface_commit`、`protocol.c` の `surface_commit` の最初）。
  - `KWL OSK slide end leaving=L frames=F first_ms=A max_gap_ms=G`: panel の滑り出し（L=0）・戻り（L=1）ごとに 1 行: 描いた frame の数、
    滑りの始めから最初の frame まで、frame の間の最大の間隔（compositor の描画の時刻、ms）。panel が入れ替わる時は先に描く方だけを数える。
  - 測りの時刻は compositor の中（入力の event の時刻ではない）: kernel の event から compositor の受け取りまでは含まない。
- `plan/ws102/tests/osk-latency.py LOG [--check]`: 各値の数・中央値・p95（nearest rank）・最大と、(a)(b) の目標の met/missed。
- `plan/ws102/tests/osk-guest.sh` の `latency` の段: 3 回（compositor を起こし直し、Text Editor に QWERTY で 20 文字、flick に入れ替えて閉じる）
  の log を `OUTDIR/latency.log` に集め、`latency.txt` に要約。判定は「測れたこと」（send 60・frame 54 以上・slide 9 以上）だけで、目標の
  合否は報告（QEMU の値は参考、WS094 と同じ扱い）。

## 確認（2026-10-08 夜 P1）

| コマンド | 結果 |
| --- | --- |
| `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat build/p1-uat/bin/wayland` | rc 0、warning 0 |
| `python3 plan/tools/style-check.py userland/desktop/wayland/keyboard.c` | 指摘の数は変更の前と同じ（2、既存） |
| `osk-latency.py` に手で書いた log（目標の met・missed の両方） | 期待どおりの要約と `--check` の rc 1 |

未実施（T1）: 基準値（`latency` の段）と回帰（`qwerty`・`roll`、離しの入口を `keyboard_panel_release` にまとめたため）。

## 基準値

（T1 の結果を待つ）
