<!-- awesome-plan project=zedbsd record=ws177-p033 -->

# ws177-p033: 上端の帯の長押し（apps bar の preview）と、Home が開ききった後の desktop の層を描かない

Parent: [WS177](../ws.md)
Status: cleared（2026-10-10 Q1 判定: T1-489 (a) で u-guest status 0（swap-cancel・status-gap ok）、回帰 ws181・p009 PASS。ユーザーの UAT「WS177 はOK」）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q908（承認済み、Q1 の dispatch 2026-10-08「WS177 案 U」）/ q908-i01
Origin: [backlog-p2](../backlog-p2.md) の WS181 ws181-p003 の行（帯の中の長押し、Home が開ききった後も desktop の層を描く）。[案 U](../phasing-20261008.md)。

## 範囲

1. 上端の帯（touch だけ）に置いて動かさない指が `KWL_EDGE_BAND_HOLD_MS`（500 ms）続いたら長押し: 押した点で下の物へ press だけを流し直す（離すのは後の本当の release）。bar の app の icon は長押しで preview をすぐ出し（`via=hold`、click と同じく離しても残る）、離しても click にしない。他の widget には普通の press と release になる。
2. App Home が開いて desktop の層の opacity が 0 になった後（progress が `KWL_EDGE_HOME_FADE_END` 以上）は、層の影・壁紙・desktop の icon・窓を描かない。層が見える間は今どおり。

## 実装（2026-10-08 P2）

- `edge.c/h`: `KWL_EDGE_BAND_HOLD_MS`、`kwl_edge_band_held(held_ms)`。
- `kwl.h`: `band_since_ms`・`band_held`（長押しの流し直しの間だけ 1）、apps bar の `press_held`、`KWL_APPS_VIA_HOLD`。
- `shell.c`: `band_button` が押した時刻を覚える、`band_tick`（`kwl_glass_tick` から。長押しで log `KWL EDGE band hold ms=N fullscreen=0` と `band_replay(server, 0)`）。`kwl_glass_draw` の層の部分を `draw_desktop_layer` に分け、`hidden`（Home の上で層の opacity が 0）の時は影・壁紙・`draw_desktop_layer` を省く。変わる時だけ log `KWL HOME layer hidden=1|0`。
- `apps-bar.c`: `band_held` の press が icon の上なら `kwl_apps_bar_show(…, KWL_APPS_VIA_HOLD)`（log `KWL APPS preview app=… via=hold`）、release は何もしない。hold で出した preview の上の icon の click は click の物と同じく隠す。

## 確認

| 確認 | 結果 |
| --- | --- |
| host `plan/ws181/tests/run-host-edge.sh` | PASS（checks=73 failures=0、長押しの 4 件を足した） |
| zedBSD の compositor（`make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws181 build/ws181/bin/wayland`） | 成功、warning 0 |
| Linux の Keiland（`make keiland-linux KEILAND_LINUX_BUILD=build/ws181-linux`） | 成功、warning 0 |
| `plan/tools/style-check.py`（shell.c・apps-bar.c・edge.c） | 新しい指摘 0（shell.c の既存 5 件は変わらず） |
| QEMU（T1、`plan/ws177/tests/u-guest.sh` の U1・U2） | 未実施 |
| 実機の指（5330） | 未実施（UAT: 長押しの時間の感じ、preview が指で選べるか） |

## 残り

- QEMU（T1）と 5330 の UAT。
