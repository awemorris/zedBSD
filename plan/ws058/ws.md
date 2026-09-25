<!-- awesome-plan project=zedbsd record=ws058 -->

# WS058: cache の大きさを現代の機械向けに見直す（page cache・buffer cache・VM object cache）

<!-- awesome-plan-current:start -->
Status: completed
Primary Milestone: MG004
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: q427
Resume point: 完了（8192 級の値は F-013 の後）
<!-- awesome-plan-current:end -->

## 目標

[design policy 10](../master-design-policy.md): 初期の i386 向けに極端に小さくした cache の大きさ（buffer cache の line 数、VM object cache の 32 object、page cache の上限、UFS の cache など）を、主記憶 4 GB・swap 16 GB を前提に見直す。
ws046-p012（VM object の寿命と cache の入れ替え）は設計で cap 32 → 256 を予定しており、この WS の方針に沿う。

受け入れ: cache ごとに今の上限・根拠・メモリの使用量と、新しい上限・根拠の表。guest（8 GiB）で clang の compile・link と expat の configure の時間が改善し、512 MiB の guest でも起動して sh・make の差分試験が通る（小さい機械でも壊れない）。

きっかけ: 2026-09-25 ユーザーの指示（design policy 10）。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws058-p001](phase001/phase.md) | cache の上限の定数と根拠の調査、新しい上限の設計 | cleared（q427-i01。buffer cache 物理/16、page cache 物理/4 は比例。固定で小さいのは VM object 32、file 表 192、inode 512、I/O pool 4 MiB） | — |
| [ws058-p002](phase002/phase.md) | cache の上限を現代の機械向けに変える | cleared（q428-i01。buffer 物理/8、page cache 物理/2、object cache 32 → 256、I/O pool 64 MiB、file 192 → 2048、inode 512 → 2048、overlay 256 → 4096。2 つの退行を bisect で解いた: overlay の表は inode cache 以上、object cache は file の表を消費する。8 GiB と 512 MiB で回帰、configure 96 → 89 秒） | p001 |
