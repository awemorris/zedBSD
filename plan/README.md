# zedBSD planning entry

Awesome Plan is active in **GitHub mode**. Start with [configuration](config.md)
and [Guardrail](guardrail.md), then fetch Master/Queue/Past Log and relevant
WS/Phase records using [the sync procedure](tools/README.md).

[Master](https://github.com/awemorris/zedBSD/issues/1) ·
[Project](https://github.com/users/awemorris/projects/2)

- [Master cache](master.md): Objectives, Milestones, Focus and Priority.
- [Queue](queue.md): q303 finished/stopped; **no active execution Queue**.
- [Future Work](future-work.md), [known bugs](known-bugs.md), [Past Log](history/index.md).
- wsXXX/phaseYYY: plans and durable evidence; only load the relevant branches.
- old/: superseded proposals and pre-Awesome policy; tmp/: import/deployment evidence.
  Neither is an instruction source or a script to replay automatically.
- [Migration status](migration-status.md): inherited limitations and current setup evidence.

2026-09-11: Priorityリスト削除。WS025残件4件をユーザー判断でcleared、
WS025/WS019/WS006/WS022/WS002をcompletedとして閉鎖。後続指示による現行Focusはfg004（WS003の4機種インストーラ実機bring-up）。

Next session: inspect local changes and sync ownership → fetch current records
and decision comments → reconcile pending/conflicted edits → report current focus
and remaining work → follow the user's concrete instruction. Do not resume q303
or cancel/complete other work just because the management method changed.

[今回のbring-up計画](history/2026-09-11-installer-bringup-plan.md)。新規Issue/既存本文・Projectの公開は承認待ちでoutboxに保持。実行Queueなし。

現行Focusにfg005（WS005のネットワーク改善1〜3）を追加。[仕様と計画](ws005/network-improvements-2026-09-11.md)。fg004と両立し、順位・Queue未指定。両計画のGitHub公開は保留。

現行Focusにfg006（PC-9821V13起動改善）を追加。ユーザー指定の実行PhaseはWS003 p022→p023→p024。[具体化した計画](ws003/v13-boot-focus.md)。fg004/fg005は保持、実行Queueなし。
