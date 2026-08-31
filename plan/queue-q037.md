# Queue: PC-9821V13 IPL read-contract repair

Last updated: 2026-08-31

QID: `q037`

Queue status: finished

Queue finished: **Yes**

Authorization: on 2026-08-31 the user reported that the ordinary PC-98 image
beeps and stops on a PC-9821V13, confirmed that this model ignores `55 aa`,
and explicitly requested that the suspected early-IPL defect be fixed.

Timebox: none. Automatic work ended with one frozen image for a user-operated
PC-9821V13 observation.

Parent: [master plan](master.md)

Previous Queue: [q036](queue-q036.md)

Next Queue: [q038](queue.md)

## Final execution registry

| Priority | WS / Phase | Authoritative document | Final state | Result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p022` | [Phase](ws003-bringup/phase022-pc9821-v13-ipl-read-contract/phase.md) | uncleared | All automatic source/binary/layout, build, and qemu-pc98 login gates pass; exact frozen image awaits the already-requested PC-9821V13 observation |

## Result and resume condition

The private-stack/SENSE/read fix, full-cell failure diagnostics, BR-T54
source/binary/layout fixture, ordinary image checker, `make -j16`, and positive
qemu-pc98 login gate pass. The frozen image is
`/home/awe/zedBSD/build/pc98/hdd-image.img`, SHA-256
`d2bfc9c45077434670f4dd0578b26d295653eef98413ab31b667ca8d3368ed4d`
at the q037 automatic checkpoint. Generated build paths are mutable; the
already-requested observation refers to that exact hash.

The Queue finishes with the item uncleared because its only remaining action
is the external PC-9821V13 observation. This is not a software-test failure.
Resume p022 from the exact first observed marker (`1S`, `1R`, `2T`, `2N`, or
`2P`) or close it if the frozen image boots successfully. Any further code
change must enter a later Queue; do not rerun the completed automatic gates
without a new reason.
