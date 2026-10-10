# Codex Queue: runtime-manifest-20261011

Status: finished
Owner: Codex / codex/rpi4-sshd
Approval: userのRPi4 sshd写真と修正継続、2026-10-11「アーキテクチャ非依存な定義に変えてほしいです。」
Finite scope: [p010](phase010/phase.md)の共通配置/runtime修正と限定build/host照合・最終規則review。実機/全package移植/toolchain変更/pushは範囲外。

| Attempt | Phase | Status | Dependency |
| --- | --- | --- | --- |
| runtime-manifest-20261011-i01 | [ws193-p010](phase010/phase.md) | cleared | mainのp008出力/現在config |

Graph: p008 main source → p010。
Outlook: 成果commitの統合後、ユーザーによるRPi4でのsshd起動確認。

## Outcome

Common manifest / AArch64 runtime layout and incremental restaging checks PASS; [evidence](tests/runtime-manifest-20261011.md). Additional port build failures and legacy bootstrap limitations recorded. Main commit approval / physical RPi4 / shared projections pending; no next Queue, no push.

## Main integration follow-up / 2026-10-11

User explicitly approved main integration of `1b3d6cd03` (answer「mainへ統合する」). Clean main at 3ba73d2c8 fast-forwarded to the exact verified source commit; read-back confirms all 16 integrated files match the tested worktree. Source integration complete; preceding approval-pending text is historical. Documentation follow-up records this integration within the same approved scope. RPi4 physical host-key/SSH execution and whole-WS acceptance remain pending. No push; shared Master/Queue/history/cache/GitHub reconciliation stays Q1 pending.
