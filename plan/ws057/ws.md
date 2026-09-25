<!-- awesome-plan project=zedbsd record=ws057 -->

# WS057: 仮想メモリの reserve と commit の分離と、commit の swap の裏打ち（over commit 禁止）

<!-- awesome-plan-current:start -->
Status: completed（2026-09-25）
Primary Milestone: MG004
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし
Resume point: —
<!-- awesome-plan-current:end -->

## 結果

- reserve（`PROT_NONE` の mapping）は commit を課金せず、commit（anonymous private・書ける file private・stack・brk・shared object・fork）は上限で ENOMEM（over commit しない）。上限は起動時の空き物理 + swap で、ユーザーの決定（2026-09-25）により**物理 + swap のまま**（swap file が無くても起動できるため）。design policy 10 に記録。
- BUG-048（`SYSCALL_PAGE_MASK` が 32 bit で mmap などの長さが 4 GiB で切り捨て）を直し、`MAP_NORESERVE` を受け付けて無視。128 GiB の reserve、5 GiB の mapping、9 GiB の ENOMEM を probe で確認。
- 回帰: boot PASS、sh 1388/1425、make 91/91、SMP 0。実機は未実施。

## 制限・移管

- 上限の物理の分は起動時の空き量で固定（総量ではない）。必要になれば別の判断。
- `mprotect` で `PROT_NONE` に戻しても commit は unmap まで返らない（方針に反しない）。fork した子の commit は後始末の非同期で数秒後に返る。

## Phase の一覧

| Phase | 内容 | 結果 |
| --- | --- | --- |
| ws057-p001 | commit の課金と上限の調査、方針との差の設計 | cleared（q425、[history](../history/queue-q425.md)） |
| ws057-p002 | BUG-048 の修正と `MAP_NORESERVE` | cleared（q426、[history](../history/queue-q426.md)） |
| ws057-p003（案） | 裏打ちの定義の反映 | 不要（ユーザーの決定: 物理 + swap のまま） |
