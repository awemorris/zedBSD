# Queue q101: unified UFS driver and image transition

Date: 2026-09-07
Status: finished
Authorization: autonomous WS025 completion includes required WS024 integration;
q100 froze the already-approved single-UFS product direction.
Timebox: review every 90 active minutes; record facts and continue autonomously.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws024-p002](ws024-unified-ufs/phase002-single-driver/phase.md) | completed | One 64-bit driver/registration, checked width boundaries, preserved features. Requires completed p001. |
| 2 | [ws024-p003](ws024-unified-ufs/phase003-formatters-and-image-consumers/phase.md) | completed | Switch target/host formatters and every image/root consumer to the same frozen profile. Requires p002 integration interface. |

Execute the [frozen contract](ws024-unified-ufs/format-contract.md). Carry WS025
p008/p010/p014 behavior into the current 64-bit codec under one owner. Check sparse
high-address and inode-size conversions on both ABIs. Preserve journal, snapshot,
quota, extended attributes, inode and namespace fault semantics.

Coordinate the kernel/producer transition: do not boot an intermediate new kernel
with old generated UFS1 roots/data. Back up generated images opaquely with hashes
before rebuild; no external media or retained-user-image conversion. One public
ufs registration/CLI, with explicit optional persistence profile. P004 separately
owns final retirement audit and full boot/feature acceptance after producer switch.
No commit; no aggregate make check; make -j16 and serialized tests/build/runtime;
never inspect `.internal/`.

Previous: [q100](queue-q100.md).

P002 core interface and focused gates pass; keep its integrated feature gates open
while p003 builds the coherent producer images they require. This is a dependency
on the implemented interface, not a claim that the full p002 phase has passed.

Checkpoint: unified producers agree, formatter faults and descriptor lifecycle
pass, all three configured builds pass, mounted feature/remount/reboot recovery
passes, and the aligned 202-sample baseline retains 10/20/20 ms quantiles. The
initial reboot failure was fixed by preserving the former UFS1 validated reopen
policy; its failed log remains recorded. Finish target CLI/native-to-overlay
acceptance, then close q101 and queue p004 retirement/full integration.

Result: both selected implementation phases completed. Full consolidated acceptance and legacy-path retirement continue in q102 / ws024-p004.
