<!-- awesome-plan project=zedbsd record=ws333-p006 -->

# ws333-p006: cpuperf の枠組み（kernel）

Status: in-progress
Parent: [WS333](../ws.md)
Queue: q1754-i01（P7）

## 目的

design.md §9 の p006: [p003](../phase003/phase.md) の header（`include/kern/cpuperf.h`）の口を `src/kern/cpuperf.c` に実装する（topology の表・domain・要求の箱と notify の道・provider の口・相対の性能と基準・受け手「cpu」・strategy の写しと MANUAL の式・直の設定と CUSTOM・上限・legacy の governor・idle の統計・WS302 の口）。受け入れは host 試験（模擬の provider）と全 platform の vmunix の build（warning 0）。UAPI（p004）は作らない。

## 状態

- 着手（2026-10-31）。結果はこの節の下に書く。
