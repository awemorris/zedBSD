<!-- awesome-plan project=zedbsd record=ws181-p011 -->
# ws181-p011: bar の仮想 desktop の island を bar の中央に（dock の時は今の位置）

Status: in-progress（2026-10-08 夜 P2、実装・build。QEMU は T1、撮影の PNG を Q1 からユーザーへ）
Disposition: normal
Parent: [WS181](../ws.md)
Queue: Q1 の投入（2026-10-08 夜、通常の優先度、承認済み）

## 由来（ユーザー、2026-10-08 夜の UAT）

「ドックバーの仮想デスクトップのislandなのですが、使ってみたところ、やっぱりdockバーの中央がいい気がしました。ただ、dockingしたときは、今の位置にするのがいいと思います。試作の繰り返しで申し訳なく思いますが、通常優先度でスケジューリングしておいて、あとで見せてください！」

## 実装（2026-10-08 P2、`userland/desktop/wayland/shell.c`）

- `bar_layout_on`: desktops の pill は窓が dock していない時 bar の中央（`output.x + (width − pill の幅) / 2`）、dock している時は p009 の位置（status の pill のすぐ左）。docked の layout が入る animation（`bar_dock`、head の bar は即座）に合わせて 2 つの位置の間を動く。中央が status の側より右になる狭い画面では status の側。title と menu の領域（`desktops_line`）は pill の位置から決まる（dock の時は p009 と同じ）。
- `KWL GLASS desktops x=` の log は pill が動くたびに出し直す（前は最初の 1 回だけ。試験は最後の行を読む）。
- 試験の追随: `plan/ws181/tests/p009-guest.sh` の 7（pill が中央、1280 幅で最初の slot が 591）、`ws181-guest.sh` の B9（b を dock した後、pill が 640 より右）。

## 確認

- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/wayland` exit 0・warning 0。style-check で新しい指摘なし（shell.c の既存の指摘は残る）。`sh -n` で 2 つの guest の script。
- QEMU: 未実施（T1。p009-guest.sh の 7 と ws181-guest.sh の B9、撮影）。
