<!-- awesome-plan project=zedbsd record=ws177-p034 -->

# ws177-p034: 全画面の app へ上端の帯の press を流し直す（touch の口）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 P2 q908 実装・build。QEMU は T1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q908 / q908-i01
Origin: [backlog-p2](../backlog-p2.md) の WS181 ws181-p003 の行（全画面の app への帯の press の流し直し）。[案 U](../phasing-20261008.md)。

## 範囲

全画面の窓の上（bar が無い）で上端の帯に触れた指は、Wiseview の swipe か分かるまで compositor が持つ。swipe でないと分かった時（横・上へ動いた、動かずに離した＝tap、動かずに長押し）、今は press が失われていた → 触れた点・時刻で全画面の client に down（wl_touch か pointer の左 press）を届け、その後の指は client の物にする。tap はその場で up まで。

## 実装（2026-10-08 P2）

- `touch.c` の新しい口 `kwl_touch_shell_handback(server, x, y, time, lifted)`（touch.h）: shell が持っている指（`ROUTE_SHELL`）を、触れた点で `deliver_first`（wl_touch の down、無い client には pointer の move と左 press）に渡し、まだ触れていれば今の点へ motion、離していれば up（pointer は release、press の権限も消す）。frame を送る。log `KWL TOUCH handback contact=N x=… y=… lifted=… route=…`。
- `contact_end`: shell の指は先に最後の点への motion を shell に渡し（その motion で手放された指は新しい route で終わる）、その後に route ごとの終わり。shell の指の終わり方は今までと同じ（motion → release）。
- `shell.c`: `band_button` が全画面の上かを `band_fullscreen` に、押した時刻を `band_time` に。`band_motion`（swipe でない）・`band_button`（release）・`band_tick`（長押し）が全画面の上では `band_handback` → `kwl_touch_shell_handback`（log `KWL EDGE band handback lifted=… given=…`）。

## 確認

| 確認 | 結果 |
| --- | --- |
| zedBSD の compositor・Linux の Keiland の build | 成功、warning 0（p033 と同じ command） |
| style-check（touch.c） | 0 |
| host 試験 | 無し（touch.c は server・evdev に結び付いている。判定の純関数は p033 の edge.c） |
| QEMU（T1、`plan/ws177/tests/u-guest.sh` の U3） | 未実施 |
| 実機の指（5330、全画面の app） | 未実施（UAT） |
