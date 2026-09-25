<!-- awesome-plan project=zedbsd record=queue-history q433 -->

# Queue q433: page fault の固定費用と exit の page table の解体（ws061-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q433
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「expatのconfigureとコンパイルがLinuxと同等水準になることを直近の目標」「新規実装よりもバグ修正とパフォーマンス改善」。範囲は [ws061-p002](../ws061/phase002/phase.md)。途中でユーザーが HAL の規則を変えた（「HALの実装は勝手に修正してください。APIの変更のみ許可が必要です」）ので、承認待ちだった HAL の table count の差分を適用した。「USB ECMよりもconfigureパフォーマンス改善を優先」「必要ならECM/SSHの修正も」。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q433-i01 | [ws061-p002](../ws061/phase002/phase.md) | uncleared（anon fault 8.4 → 4.2 µs は目標達成。file の fault 13.6 → 8.4 µs、`true` 8 → 4〜5 ms は目標（数 µs・3 ms）に届かず、残りは fault の数（p003）と複写（ws046-p014）。VM object の registry の hash・slab の O(1)・kcrt の word 複写・HAL の table count と direct map の変換・itimer の tick で configure 87 → 59〜64 秒（overlay）、59 → 35〜38 秒（tmpfs）。回帰 boot・make 91/91・sh 1388/1425・SMP・COW・itimer） |

依存: ws061-p001（cleared）。

判明したこと: BUG-049（SSH が届かない）は計測用 image を extra-files 無しで build したためで不具合ではない（resolved）。BUG-050（strerror の穴）を記録。overlay の同期書きを p004 として立てた。

Upcoming Work Outlook: ws061-p003（fault-around）、ws061-p004（overlay の同期書き）、ws046-p014（直接 map）、ws056-p001 の判断（BUG-046）。
