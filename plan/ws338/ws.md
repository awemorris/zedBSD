<!-- awesome-plan project=zedbsd record=ws338 -->

# WS338: libmedia の H.264 の software の decoder の速さの続き

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG006。
- Status: planning（2026-11-01 09:05 新設。設計の Phase はまだ始めていない）
- 目標: [WS326](../ws326/ws.md) で正しさを確かめた H.264 の software の decoder を速くする。Pi 5 で 1080p30 の High を decoder 4 thread で 40 fps 以上（WS326 p024 の設計の目標）を受け入れの目安にし、5330 でも測る。
- 範囲（WS326 の「制限・移管」の案 a〜e）: a deblocking の filter と weight、b neighbour・transform・syntax の消し・CABAC の file の中の fast path、c thread（deblock の行の pipeline、parse と再構成の分離）、d SIMD（NEON・SSE2）、e Pi 5・5330 の実測と目標の判定。
- 出発点: WS326 p024 の設計と計測（host の 1 thread で 1080p 12〜19 fps、ffmpeg の C だけで 88 fps。重いのは補間 30〜35%・deblocking 22〜27%）。
- Resume point: p001 の設計（WS326 p024 の設計を土台に Phase を分ける）。担当の割当はユーザーの指示で。

## ユーザーの決定（要点）

- 2026-11-01 09:05 ユーザーから、WS326 の速さの続きは WS326 に Phase を足さず新しい WS にする、という趣旨の答えを受けた（Q1 の推しの案）。

- 2026-11-01 09:30 Q1: SIMD（範囲の d）は WS329 のユーザーの答えと同じ形で行う（gcc・clang の vectorizer の report を見て C を整える Phase → 表せない所は short vector 型の intrinsics）。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| p001 | 設計（WS326 p024 の案 a〜e を Phase に分ける、受け入れの数） | in-progress（2026-11-15 P1 q2121） | WS326 |
