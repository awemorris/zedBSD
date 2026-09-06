# WS024 test index

Last updated: 2026-09-06

Parent: [WS024](../ws.md)

Planned acceptance; no fixture or test execution is claimed by this record.
P001 freezes concrete bounds and the selected implementation Queue supplies
its finite runtime budget.

| Area | Required result |
| --- | --- |
| Format and width | One documented format; checked 64-bit addressing on 32/64-bit ABIs; explicit limits and legacy-image rejection |
| Driver behavior | Ordinary I/O and metadata, endian handling, extended attributes, persistent quotas, journal/snapshot recovery and lifecycle remain passing |
| Generation | Target and host output matches the selected production format; retained reservation/refusal/flush/read-back cases pass |
| Boot and persistence | Native and overlay roots boot; generated upper data persists across unmount/remount and reboot |
| Retirement | One active driver/registration/formatter path; no permanent UFS1/UFS2 fork or stale build consumer |

Place reusable fixtures here and disposable diagnostics under the WS `temp/`
directory. Retain earlier WS018/WS019 results as historical baselines.
