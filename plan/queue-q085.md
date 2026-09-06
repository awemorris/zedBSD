# Queue q085: Wi-Fi command stories and recovery

Date: 2026-09-06
Status: finished

The user explicitly authorizes a coherent structural rewrite based on the static
report, followed by acceptance using the saved
30 managed command scenarios and implementation corrections until all pass.
The frozen source is `a3f1ea3`; Q084 remains archived and complete.

Result 2026-09-06: accepted review-3 design implemented; all 30 actual host
command/daemon/child stories pass ordinary and ASan/UBSan execution. Focused
common WLAN, AX211 PCI/boot/runtime and RTL8822BU/RTL8822B lifecycle gates,
applicable userland regressions and serialized amd64/PCAT/PC98 builds pass.
No real RF or QEMU boot was performed. No commit was made.

Evidence: [P048 implementation/lifecycle results](ws004-hardware/phase048-wlan-deferred-stop/results.md)
and [P012 individual acceptance results](ws005-networking/phase012-wifi-command-scenarios/results.md).
Review dispositions and earlier checkpoints remain in their phase directories.

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 0 | [ws004-p048](ws004-hardware/phase048-wlan-deferred-stop/phase.md) | completed | Independent checked stop, visible completion and managed retirement implemented and verified |
| 1 | [ws004-p047](ws004-hardware/phase047-wlan-driver-lifecycle-review/phase.md) | completed | Driver lifecycle corrections verified with P048 focused runtime fixtures |
| 2 | [ws005-p012](ws005-networking/phase012-wifi-command-scenarios/phase.md) | completed | All 30 command/recovery stories, normal/sanitizer, maintained regressions and builds pass |

Current authorization: the user accepted the review-3 design and explicitly
requested implementation through scenario acceptance. The preceding stop before
acceptance is released. P048 defines the added finite implementation scope;
P012 retains the 30-story acceptance contract. Execute in the order above.

P012 defines the finite scope and gates. Review progress each 90 active minutes;
complete authorized useful work and retain real failures until corrected.
No commits, aggregate make check, private credential reuse or unrelated WS work.
