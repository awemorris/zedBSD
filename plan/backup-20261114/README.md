# 2026-11-14 の退避（host の SSD の不調に備え、merge していない branch と未 commit の変更を diff で残す）

各 file は `git diff main...<branch>`（branch の分岐点からの差）。戻す時は分岐点（下の base）に `git apply`。commit の列は残らない。

| file | branch | base（分岐点） | commit 数 |
| --- | --- | --- | --- |
| agent_p6-ws296c.diff | `agent/p6-ws296c` | 61630bb0b1 | 54 |
| agent_p6-ws296b.diff | `agent/p6-ws296b` | 61630bb0b1 | 40 |
| codex_fix-bug202-boot-worker.diff | `codex/fix-bug202-boot-worker` | e42ef86004 | 30 |
| agent_p6-ws296.diff | `agent/p6-ws296` | 61630bb0b1 | 18 |
| agent_p6-ws052.diff | `agent/p6-ws052` | 08cd32bb8c | 13 |
| agent_p6-ws300.diff | `agent/p6-ws300` | 08cd32bb8c | 9 |
| agent_p4-ws283.diff | `agent/p4-ws283` | e4edf286c7 | 9 |
| agent_q1-gpudiag-steps.diff | `agent/q1-gpudiag-steps` | 92c62d2bf0 | 7 |
| agent_p6-ws299.diff | `agent/p6-ws299` | 0b60a32892 | 7 |
| agent_p2-q1553.diff | `agent/p2-q1553` | be55ac4db7 | 6 |
| agent_p2-bug333.diff | `agent/p2-bug333` | 6288e6ba9c | 5 |
| agent_p2-ws217.diff | `agent/p2-ws217` | d23ab88387 | 4 |
| agent_p2-q1539.diff | `agent/p2-q1539` | 4b7d221917 | 4 |
| agent_p2-q1258.diff | `agent/p2-q1258` | 91885aec73 | 4 |
| codex_rpi4-usb-session.diff | `codex/rpi4-usb-session` | a094b953c3 | 3 |
| agent_p2-ws291i.diff | `agent/p2-ws291i` | b5f66af597 | 3 |
| agent_p1-q1541.diff | `agent/p1-q1541` | 8ccc9435e2 | 3 |
| codex_menuconfig-firmware.diff | `codex/menuconfig-firmware` | c2b97e953f | 2 |
| codex_fix-menuconfig-arm64.diff | `codex/fix-menuconfig-arm64` | 93914124cb | 2 |
| codex_fix-ci-nasm.diff | `codex/fix-ci-nasm` | a5e1a182f5 | 2 |
| agent_p3-q1540.diff | `agent/p3-q1540` | fcba026e71 | 2 |
| agent_p1-q1262.diff | `agent/p1-q1262` | 91885aec73 | 2 |
| agent_p1.diff | `agent/p1` | bdc49fcb4c | 2 |
| p3-test-p5.diff | `p3-test-p5` | f362d7cab4 | 1 |
| p1-save-e8e9827.diff | `p1-save-e8e9827` | 14f77ea36a | 1 |
| keep_q1-base-7a86ffc11.diff | `keep/q1-base-7a86ffc11` | af7aa8c46f | 1 |
| codex_rpi4-pcie-boot.diff | `codex/rpi4-pcie-boot` | fbcb2b5437 | 1 |
| codex_rpi4-config-build.diff | `codex/rpi4-config-build` | df1d2be263 | 1 |
| codex_menuconfig-userland.diff | `codex/menuconfig-userland` | fe4300313f | 1 |
| codex_menuconfig-drivers.diff | `codex/menuconfig-drivers` | a1c65db6aa | 1 |
| codex_ci-rpi-firmware.diff | `codex/ci-rpi-firmware` | 9a0ddc6cb2 | 1 |
| agent_p6-ws243b.diff | `agent/p6-ws243b` | 61630bb0b1 | 1 |
| agent_p4-bug330.diff | `agent/p4-bug330` | d45e5eef9e | 1 |
| agent_p2-ws288-p006.diff | `agent/p2-ws288-p006` | 2f4777fcb9 | 1 |
| agent_p1-ws353m.diff | `agent/p1-ws353m` | ac5b3993f7 | 1 |
| worktree_ws197-media_uncommitted.diff | `codex/ws197-mms-media` の worktree の未 commit の変更（17 file） | 9dfebc99b9 | — |
| amaterrace_p1-ws347rtl_uncommitted.diff | amaterrace-gpu の worktree `/home/awe/amaterrace-worktrees/p1-ws347rtl`（branch ws347-rtl）の未 commit の変更（3 file、2026-11-11 に止めた物） | 6949b2482e | — |
