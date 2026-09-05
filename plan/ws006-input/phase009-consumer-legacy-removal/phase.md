# WS006 Phase 009: consumer closure and legacy console-input removal

Last updated: 2026-08-30

WSID: `ws006`

Phase ID: `p009`

Combined ID: `ws006-p009`

Status: Queue-ready; q063 satisfied the latest WS008 dependency and the user
confirmed p008 physical USB HID operation on 2026-09-05

Parent: [WS006](../ws.md)

Tests: [WS006 test index](../tests/README.md)

## Objective

Prove that every in-tree graphical, runtime, and kernel consumer has a supported
input path, then delete the obsolete zedBSD `/dev/console` continuous-event,
input-mode, drain-event, and key-state UAPI without leaving compatibility
stubs.  Ordinary tty character input and console graphics/cursor operations
remain supported.

This is the WS006 closure Phase. Q063 integrated the requested latest official
Noct tree at its new userland location and reverified its selected zedBSD BeUI
backend against evdev; the user has now supplied p008's bounded physical
observation.

## Dependencies

- `ws006-p006` through `ws006-p008`: truthful multi-source core, USB HID
  parser, live USB HID producers, console subscription, and hotplug.
- `ws018-p007`: Xzed is already evdev-only and has no `/dev/mouse` or console
  continuous-event fallback.
- [`ws008-p009`](../../ws008-noct/phase009-base-noct-relocation-target-resume/phase.md):
  both host-toolchain and target userland revisions are pinned, target Noct is
  acquired below `userland/base/noct/noct/` and integrated from
  `userland/base/noct/`, and the selected BeUI zedBSD backend uses
  capability-discovered `/dev/input/eventN` only.  This dependency is not
  satisfied by the rejected historical downstream runtime under
  `userland/packages/lang/noct`.

## Removal boundary

Remove from the live public console header and implementation:

- `ZEDBSD_CONSOLE_POLL_EVENT` and `ZEDBSD_CONSOLE_READ_EVENT`;
- `ZEDBSD_CONSOLE_KEY_STATE`;
- event-mode `ZEDBSD_CONSOLE_GET_INPUT_MODE` and
  `ZEDBSD_CONSOLE_SET_INPUT_MODE` behavior and its public event-mode records;
- `ZEDBSD_CONSOLE_DRAIN_INPUT` when its only remaining purpose is the removed
  event consumer; and
- the corresponding event queue, single event owner, key-state bridge,
  structures, constants, and dead helper declarations.

Do not renumber the surviving console ioctls merely to fill holes.  ABI numbers
are stable historical assignments even when operations are removed.  Removed
requests return the ordinary unsupported ioctl result because no live
dispatcher recognizes them; no compatibility emulation or hidden event path is
retained.

The following remain in scope and operational:

- ordinary tty reads, canonical/noncanonical termios behavior, job control,
  login, shell editing, and console character echo;
- the kernel-internal input-core subscriber used by the tty/console broker;
- `/dev/console` screen, cursor, font, and terminal-capability operations that
  are unrelated to continuous input events; and
- evdev capability, state, read, poll, grab, overflow, and detach behavior.

## Detailed procedure

1. Inventory all live C headers/sources, build descriptions, generated image
   contents, selected package sources, and public documentation for the legacy
   console input structures, constants, ioctls, paths, and helpers.  Archived
   planning/evidence may mention them historically and is not edited to hide
   the record.
2. Verify Xzed and the latest selected Noct/BeUI backend with capability-based
   evdev discovery, keyboard, relative/absolute pointer, `SYN_DROPPED`, and
   hotplug behavior.  No fixed `eventN`, name-based role, `/dev/mouse`, or
   console-event fallback is accepted.
3. Migrate the in-kernel diagnostic shell away from
   `console_input_poll_event()`/`console_input_read_event()` to the supported
   internal input/tty boundary.  Do not make kernel code open an evdev node.
4. Delete the legacy public declarations and console-driver dispatch/state,
   then remove dead internal helpers and event queues.  Preserve surviving
   ioctl numbers and unrelated console behavior.
5. Update the evdev and console reference documentation to state the sole
   continuous input interface and exact retained console boundary.
6. Add an IN-T50 source/build audit that rejects live legacy identifiers and
   paths in kernel/userland code while permitting explicitly marked historical
   plan/evidence files.
7. Run Xzed and latest Noct/BeUI focused tests, evdev IN-T00/T10/T11/T12/T20,
   USB HID IN-T40/T41, login/shell/tty regressions, supported platform builds,
   and normal amd64 QEMU console plus X/BeUI sessions.  Do not use `make check`
   or `.internal/` material.

## Verification contract

The implementation Phase must retain a source audit equivalent to:

```sh
rg -n 'ZEDBSD_CONSOLE_(POLL_EVENT|READ_EVENT|KEY_STATE|GET_INPUT_MODE|SET_INPUT_MODE)|console_input_(poll|read)_event|console_input_event|/dev/mouse' \
  include src userland
```

The command must return no live use or definition of the removed interfaces.
If an unrelated historical compatibility header must remain for source-build
diagnostics, it requires explicit human approval; an unreferenced stub does not
satisfy this Phase.

## Completion conditions

- Every selected in-tree consumer uses evdev or ordinary tty input through a
  documented supported boundary.
- Xzed and the latest integrated Noct/BeUI target pass without any legacy
  console-event or `/dev/mouse` fallback.
- The public console input-event/key-state declarations, implementation,
  queues, and kernel helpers are removed while surviving ioctl numbers and
  unrelated console features remain stable.
- Login, shell editing, kernel diagnostic input, Xzed, BeUI, PS/2/PC-98 input,
  and USB HID keyboard/mouse regressions pass.
- IN-T50 and the full source audit find no live in-tree consumer or dead
  compatibility implementation.
- WS006 completion conditions can be marked complete with exact QEMU and
  physical USB HID evidence rather than a software-only claim.

## Reconsideration boundary

Keep this Phase blocked if the latest WS008 revision is unavailable, disabled,
or still uses the legacy console API.  Stop and return to the owning WS if a
consumer requires a new evdev capability, or if removing the old API exposes a
tty/job-control redesign.  Do not restore a compatibility ioctl simply to
avoid completing a consumer migration.
