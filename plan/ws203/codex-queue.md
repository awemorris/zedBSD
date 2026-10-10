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

Merge request ID: ws203-genet-20261011. Submission SHA: provided by this branch's WIP commit; integration SHA / ACK pending Q1 or explicit user approval. Shared Master/focus/priority, Queue/history, F-029 promotion and publication are pending Q1. Local records have not been published to GitHub (plan/config.md publication state).
