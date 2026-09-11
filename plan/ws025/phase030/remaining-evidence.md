# p030 remaining evidence after q275

> 67b28ce0：IMOD実装の残存を静的確認済み。以下は旧測定の台帳であり、現行commitの動作確認は未実施。[最新状態](../post-rollback-review.md)。

2026-09-10. This ledger distinguishes already accepted recovery behavior from
coverage under the IMOD experiment. It does not reopen completed p025/p029.

| Requirement | Existing evidence | Remaining p030 condition |
| --- | --- | --- |
| USB2/USB3, intervals 0/160/4000 | q255–258 actual register readback, storage/HID and raw interval counters | No unchanged matrix rerun needed |
| REC01/02 bounded reset and mode/capacity handling | p025 final acceptance table; q260 actual BOT fixture | Native correlation under comparison interval if selected; not unimplemented reset |
| REC03/05 media identity and namespace | q259 native equal-size idle/mounted exchange | q276/q277 now cover 0/160/4000 on the same QEMU topology; do not repeat |
| REC04/06 pending/control/late ownership | p025 final-async and worker tests; q261 core late completion, q262 HCD; p029 acceptance | Do not claim native delayed completion at each IMOD from host fixtures |
| Dirty media loss | p029 q230 high/super native dirty disposal and replacement | Already accepted there; not evidence of physical latency or each IMOD |
| Physical IRQ latency and WLAN concurrency | No current supported physical observation | Host routing/SSH, inspect devices/current workload, then construct same topology comparison |

Read-only readiness command: `ssh -o BatchMode=yes -o ConnectTimeout=10
 awe@10.0.10.25 'uname -a'`; exit 255, `No route to host`.
No remote state changed. Do not busy-retry without a meaningful new opportunity.

Completed q276/q277: explicit source/root image and expected IMOD readback
integration, plus native media recovery at 0/160/4000. Both before/after MMIO
captures match, same workload-root hash and all media oracles pass. This closes
the selected media-comparison gap. Other pending-I/O/late-completion combinations
remain only as their documented host evidence unless a native fault injection
path is explicitly selected; physical/WLAN conditions are unchanged.

Keep 4000 as default. Physical/WLAN requirements remain uncleared even if the
additional native media cells pass; no unrelated full-suite repeats needed.
