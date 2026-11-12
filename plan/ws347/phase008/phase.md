<!-- awesome-plan project=zedbsd record=ws347-p008 -->

# ws347-p008: blitter の無い GPU で表示の写しを shader で行う

Status: in-progress（2026-11-13 Q1、q2030-i01 P4）
Parent: [WS347](../ws.md)

## 目的

2026-11-13 ユーザーの決定: blitter（CAP_BLT）が無い時は、表示の写しも shader で行う。

今の `src/drivers/gpu/ghci/display.c` は、copy の present（ordinary storage を driver の 2 つの frame の片方へ写す）で blitter が無いと CPU が写す（`copy_by_processor`、discrete の GPU では PCIe の local memory の窓を通す）。VF2＋KC705 では PCIe が遅く、1080p の frame の CPU の写しは表示を大きく遅らせる見込み。Vulkan の転送は既に blitter が無ければ driver の shader の program（`vk-execute.c` の転送の program、p003）で行う。

## 範囲

- blitter が無い時の copy の present を、driver の shader の program（compute の dispatch）で session 0 から写す。frame は session 0 に map する（今は blitter の時だけ map）。既存の Vulkan の転送の program と道を共有できるかを先に確かめ、共有できる形を本来の形とする。
- CPU の写しは、shader の program が使えない時（compiler が program を拒む等）の代わりの道として残すかを決めて書く。
- unified の GPU（UMA）でも shader が良いか（CPU の直の写しとの比較）を本来の形で決めて書く。
- HAL の API・Makefile・mk/ は変えない。

## 受け入れ

- host 試験（WS290 の試験の道具、blitter の無い profile の `minimal`・`flat`・`kc705-min`）で、copy の present が dispatch になり blitter の packet も CPU の写しも出ない。blitter の有る profile は今どおり blitter。全 PASS。
- vf2・x86_64 の kernel が warning 0。実機の速さは p004。
