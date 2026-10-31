<!-- awesome-plan project=zedbsd record=ws291-p025 -->

# ws291-p025: Raspberry Pi 4（V3D 4.2）で mview を動かし、qs40 の model をスペックの比で決めた copies・1920x1080 で 50 fps に

Status: in-progress（2026-10-31 20:20 新設、P4 q1784）
Parent: [WS291](../ws.md)
Queue: q1784（P4）

## 目的

2026-10-31 ユーザーから、Pi 4 の GPU の driver を改善して mview を動かし、最適化する、Pi 4 は 1920x1080 で起動して進める、空いている担当に割り当てる、
この作業が終わってから Pi 5（[p021](../phase021/phase.md)）に移る、という趣旨の指示を受けた。

今（[BUG-380](../../bugs/BUG-380.md)）: main の V3D の Vulkan の executor（`src/drivers/gpu/bcm2711/vk/`、Pi 4 と Pi 5 で共有）は compositor の四角の形だけを受け、
mview の vkCreateGraphicsPipelines が FEATURE_NOT_PRESENT。足りない機能（index の draw・instance・mip・attribute の直の address）は p021 で P4 が host で
実装済み（agent/p4-ws291pi、6334ae147〜f2f87ac1b、main に未 merge）。

## 範囲

1. agent/p4-ws291pi を main と合わせ、Pi 4（V3D 4.2）で mview が動くまで driver を直す（Pi 5 の V3D 7.1 の build と host 試験を壊さない）。BUG-380 を閉じる。
2. 目標の copies を p021 と同じ規則で決める（RTX 2070 の scene-5000 の FP32 の比。Pi 4 の V3D 4.2 の QPU 数・clock は実機の dmesg で確かめる）。
3. 1920x1080・direct scanout（WS309 p015 の zero copy の game mode）で計測し、50 fps へ最適化（p021 の 4・5: CPU の詰め直し・texture の tile の写しを無くす等）。
4. desktop の後退なし（Pi 4 の desktop、compositor の四角の経路）。HAL は変えない、driver は hal_* を使わない。
5. 実機: Pi 4（10.0.30.6、SwitchBot 14C19FA80F86）。1920x1080 で起動するように起動の設定を変える。
