# Codex Queue: rpi4-genet-20261011

Status: finished
Owner: Codex / codex/rpi4-genet
Approval: current user 2026-10-11「とりあえず、RPi4のEthernetドライバを実装できますか？デバッグが楽になると思います。」
Finite scope: RPi4 onboard GENET v5 driver + existing network API integration + option + warning0 arm64 kernel build + focused host model + final full C standards review. No QEMU/image build/toolchain mutation/push; no USB/storage scope expansion. Physical acceptance is a user handoff, not claimed from build.

| Attempt | Phase | Scope | Status | Dependencies |
| --- | --- | --- | --- | --- |
| rpi4-genet-20261011-i01 | ws203-p001 | source/build wiring | cleared | current main APIs |
| rpi4-genet-20261011-i02 | ws203-p002 | final conformance/build/model | cleared | i01 source |

Graph: main a094b953c (context) → i01 → i02. p003 is outlook (physical handoff). Shared projections and F-029 promotion pending Q1. Investigate within GENET/PHY and existing API contracts; a required HAL API change/new product decision stops only that dependency.

## Terminal outcomes / handoff (2026-10-11)

Both scoped attempts cleared: i01 implemented the user-corrected PHY/MAC split; i02 reviewed final source against the full C standard and passed warning0 ON/OFF arm64 builds, focused host model and common-menu checks. Evidence: [results](tests/results.md), [source hashes](tests/source.sha256). No main merge or hardware-success claim. WS remains incomplete pending p003. Next queue is not started automatically.

Merge request ID: ws203-genet-20261011. Submission SHA: `e9ddb4540` (implementation + initial evidence); integration SHA: `d1def8aef` (fast-forward main); ACK: current user approval and main read-back, 2026-10-11. Shared Master/focus/priority, Queue/history, F-029 promotion and publication are pending Q1. Local records have not been published to GitHub (plan/config.md publication state).

## Main integration / 2026-10-11

User「mainに統合をお願いします。」を承認として `git merge --ff-only codex/rpi4-genet` をmainで実行、`a094b953c` → `d1def8aef`。競合なし。全13 source/config hashes一致、mainのmenuconfigでEthernet分類とGENET選択を確認、現在のconfig.mkはRPi4既定 `y`、make dry-runのarm64 source selectionにPHY/MAC両ファイルあり。pushなし、共有Master等の投影保留は継続。実機成功は未確認。
