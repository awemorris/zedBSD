# ws002-p019: integrated QEMU acceptance and repair

WSID: `ws002`

Phase ID: `p019`

Status: complete for the documented minimum system

Parent WS: [WS002](../ws.md)

## Objective and design

Run the installed system under bounded `qemu-system-x86_64` through boot/init,
logging, getty/login/shell, networking, scheduling, optional time setup,
service control/failure, persistence, and orderly shutdown. Repair integration
defects that prevent the documented minimum rather than handing them off.

## Acceptance and result

The minimum system reached complete integrated operation. The authoritative
executable entry is `tests/phase19-qemu-test.py` with its rc.conf, service, and
smoke fixtures, indexed in [WS002 tests](../tests/README.md). POSIX gaps found
outside the minimum remain in [WS001](../../ws001-posix/ws.md).

## Interruption record

Not interrupted. The subsequent network synchronization redesign is
`ws002-p020`; new hardware/network expansion belongs to WS003–WS005.

## Completion conditions

- The installed QEMU system passes boot, logging, login/shell, services,
  scheduling, networking, optional time, persistence, and shutdown scenarios.
- Defects blocking the documented minimum system are repaired and retested.
- Remaining out-of-scope POSIX incompatibilities are recorded in WS001 with evidence.
