<!-- awesome-plan project=zedbsd record=ws195 -->

# WS195: zedBSD でも userland/desktop を /opt/keiland/ に入れる（account-admin を base から Keiland へ）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG006
Related Milestones: MG007
Objectives: O2
Parent: [Master](../master.md)
Queue: 未割当
Target: **ベータ3**（RC の後。2026-10-09 ユーザー、クリック「ベータ3（RC の後）」）
Resume point: p001 の D1〜D7（ユーザーの判断、[phase.md](phase001/phase.md) §2.2）。決まったら design-reviewer → p002 から実装（10/17 の後）。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー）

「userland/desktop/のインストール先を、zedBSDでもLinux/FreeBSDに合わせて、/opt/keiland/にします。account-adminはKeilandの必須バイナリとして、/opt/keiland以下に移します。baseから移動してください。」

## 目標

- zedBSD の image で userland/desktop の program・library・data を /opt/keiland/ の下に入れる（Linux・FreeBSD の Keiland と同じ配置）。
- account-admin を userland/base から Keiland（userland/desktop の側）へ移し、Keiland の必須の binary として /opt/keiland の下に置く（set-user-ID root、PATH に載せない方針は保つ）。docs/architecture/security.md を追従。
- 起動の経路（rc・sessiond・greeter・compositor が起動する program）、menuconfig、license の一覧、試験（/bin/wayland などを参照する約 520 file、2026-10-09 の数）を追従させる。試験は「試験の整理の基準」（AGENTS.md）を当て、使わない物は直さずに消す。
- 移行の間の互換（古い path の symlink を一時的に置くか）は p001 で決める。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 今の配置と参照の調べ、/opt/keiland の配置（bin・lib・libexec・share）と移行の設計、account-admin の移動 | planning（2026-10-09 夜 P1: 調べと設計の案。D1〜D7 のユーザーの判断と design-reviewer 待ち。p002〜p006 の案は phase.md §2.4） | ベータ2 の公開（10/17） |
