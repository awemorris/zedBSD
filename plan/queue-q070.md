# Queue: RTL8822BU physical connection closure

Last updated: 2026-09-04

QID: `q070`

Queue status: finished

Queue finished: **Yes**

Authorization: the user requested the existing WS004 physical RTL8822BU work
to be implemented to real-hardware operation, then published the working fix
to `origin/main`, authorized discarding the superseded local changes, and
accepted the physical result as closure.

Parent: [master plan](master.md)

Previous Queue: [q069](queue-q069.md)

## Purpose

Complete [`ws004-p043`](ws004-hardware/phase043-rtl8822bu-physical-connect-ux/phase.md):
make direct connect automatically scan, retain one 30-second command deadline,
provide explicit primitive up/down and quiet operation, localize the physical
pre-authentication timeout, and reach controlled-port authorization on the
exact RTL8822BU hardware.

## Execution registry

| Priority | WS / Phase | Status | Result |
| --- | --- | --- | --- |
| 1 | `ws004-p043` | completed | The user-published `origin/main` correction works on the physical RTL8822BU target; the merged focused driver, WLAN/WPA2, wifi-command, and HAL-format regressions pass |

## Accepted evidence

- `main` was fast-forwarded from `3202de2` to the user-published and tagged
  `d6591cb` (`Fix RTL8822BU`) with no merge conflict.
- The user explicitly reported successful physical RTL8822BU operation and
  directed that the workstream be closed.  No credential, key, or unredacted
  network identity is copied into this Queue.
- The merged RTL8822BU driver/security fixture, WLAN common-core and WPA2
  engine/L2 fixtures, primitive `wifi` command fixture, and amd64/i386 HAL
  formatting fixture pass.
- The earlier `%llu` diagnostic defect is covered by the merged HAL formatting
  regression and does not remain a p043 closure item.

## Closure boundary

Q070 and p043 are complete. WS004 is closed as the accepted current hardware
baseline. `BUG-009`, non-DFS 5-GHz expansion, AX211 direct-boot acceptance,
cross-driver refactoring, CDC ECM accounting adoption, same-endpoint multi-URB
work, and WS005's final five-run campaign remain explicit deferred or
separately owned work. They require a deliberate future reopening and do not
invalidate this closure.
