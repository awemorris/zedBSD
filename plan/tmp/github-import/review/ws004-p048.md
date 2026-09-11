# WS004 p048: Observable, independently retried WLAN stop

Date: 2026-09-06
Combined ID: `ws004-p048`
Status: completed (`q085`)

Implementation, focused lifecycle gates, all 30 ordinary/sanitized command
stories, maintained regressions and three serialized builds pass. See
[implementation and evidence](results.md). Physical RF and QEMU boot were not
performed; this is deterministic production-code acceptance.

The user accepted P047's review-3 response and explicitly authorized defining,
queuing and implementing it, followed by the complete P012 scenario campaign.
This instruction releases the previous stop before acceptance. Review progress
every 90 active minutes; continue bounded useful work without another approval.

## Contract

Administrative down closes forward admission immediately. An incomplete driver
stop retains resources, has an observable pending/error state, and is retried
without a subsequent up or an open net-device poll. Only checked hardware stop
and common ownership reconciliation publish completion. Never reset or release
under active callers. Persistent hardware failure remains visible and retryable.

Implement a WLAN retirement execution path separate from the network worker,
with retained station/device lifetime, duplicate suppression, bounded retries,
and open/detach/shutdown exclusion. Preserve driver lifecycle serialization.
Expose read-only stop state even while normal station admission is closed;
reserve an explicit status field from the existing UAPI reserved area while
preserving request size and ioctl number. Document input/output compatibility.
Update wifi and networkd to verify stop completion and retain known radio
identity across transient status failures. Keep the six public net wifi forms.

## Sequence and acceptance

1. Implement kernel lifetime/retirement and both driver adapters; implement the
   status and managed stop completion contract together.
2. Add focused production-core fixtures for delayed callers, retry without up,
   retained resources, status reads, persistent errors, open/detach/shutdown and
   device reuse. Run normal/sanitizer gates; repair source defects.
3. Complete P047 lifecycle gates and P012's actual command/daemon/child acceptance
   harness. Run all 30 saved stories, maintain individual results, repair and
   rerun affected stories plus the final complete normal/sanitized set.
4. Run applicable maintained regressions, serialized amd64/PCAT/PC98
   `make -j16` builds, and practical actual command/daemon integration smoke.
   Distinguish deterministic radio boundaries from actual RF evidence.
5. Record exact results and remaining coverage in P/Q/W/M. Completion requires
   evidence, not merely successful builds or a count of isolated assertions.

Scope: P047 common WLAN/AX211/RTL8822BU lifecycle, necessary kernel worker
integration, WLAN status UAPI consumers, and P012 managed retirement/acceptance.
No commits, aggregate make check, unrelated workstreams, firmware/RF changes,
private `.internal` reads or credentials in fixtures/logs. Use synthetic profiles.

Initial design evidence: [review-3 response](../phase047/review3-response.md).
