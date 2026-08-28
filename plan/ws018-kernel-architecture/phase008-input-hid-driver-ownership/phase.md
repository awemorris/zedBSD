# WS018 Phase 008: independent input/HID driver ownership

Last updated: 2026-08-29

WSID: `ws018`

Phase ID: `p008`

Combined ID: `ws018-p008`

Status: completed (`q026`)

Parent: [WS018](../ws.md)

Shared tests: [WS018 test index](../tests/README.md)

## Objective

Give the generic evdev core, console frontend, and each physical mouse backend
an explicit final source owner.  PC/AT PS/2 and PC-98 bus-mouse drivers must
register and drive their own `/dev/input/eventX` device, including their own
activation and button state, so the generic `mouse-device.c`, `/dev/mouse`, and
the legacy mouse UAPI can be deleted completely.

`src/drivers/hid/` means human-interface devices in the broad zedBSD taxonomy;
it is not restricted to USB HID protocol implementations.

## Dependencies

- `ws018-p001` has moved the root driver tree to `src/drivers/` and repaired
  the global source/build path.
- `ws018-p007` has removed every in-tree Xzed dependency on `/dev/mouse` and
  console continuous events.
- WS006's evdev queue, capability, state, and producer contracts remain the
  behavioral interface; this Phase changes ownership, not that UAPI.

Do not queue this Phase before p007.  The legacy node is removed in this Phase,
not left as an unowned compatibility stub.

## Target ownership

```text
src/drivers/input-device.c
src/drivers/input-queue.c
src/drivers/input-capability.c
src/drivers/input-keymap.c
src/drivers/fs/console.c
src/drivers/hid/ps2-mouse.c
src/drivers/hid/pc98-busmouse.c
include/drivers/hid/ps2-mouse.h
include/drivers/hid/pc98-busmouse.h
```

The four `input-*.c` files deliberately remain directly under `src/drivers/`.
Their existing kernel interface headers may remain under `include/kern/`; this
Phase changes implementation ownership and does not casually rename a stable
kernel API.  The console file remains the `/dev/console` and tty/evdev bridge
under `src/drivers/fs`, while its established public kernel header remains the
call boundary.

Architecture console producers inside `src/hal/` remain HAL code in this
Phase.  Moving those low-level ports/scancode readers would change the HAL
boundary and therefore requires a separate decision rather than an opportunistic
path cleanup.

## Fixed mouse design

- There is no common mouse frontend, backend registry, `/dev/mouse` ring, or
  mouse-specific event structure after this Phase.
- Each of `ps2-mouse.c` and `pc98-busmouse.c` privately owns:
  its `struct input_device *`; evdev capabilities and identity; last button
  state; reader/activation reference state; hardware start/stop callbacks; and
  direct `EV_REL`, `EV_KEY`, and `EV_SYN` publication.
- Required lifecycle code is intentionally duplicated.  First evdev open
  starts the hardware, additional opens do not reinitialize it, and the final
  close stops or masks it.  A failed first start restores an inactive,
  retryable state.  Concurrent opens/closes cannot observe a half-started
  backend or underflow a reference count.
- PS/2 packet parsing and IRQ ownership stay private to the PC/AT driver.
  PC-98 PPI sampling and its IRQ-service worker stay private to the PC-98
  driver.  The drivers do not call one another or a newly renamed mouse-common
  helper.
- Each complete sample emits changed `REL_X`/`REL_Y`, changed left/right/middle
  button values, then one `SYN_REPORT`.  Button state is updated by the evdev
  capability core; zero-motion button edges remain observable.  Existing
  positive-down Y behavior is preserved and documented in evdev terms.
- Device numbering remains dynamic.  Product/name/physical-path metadata may
  describe each backend accurately but cannot become a consumer selection
  contract.

## Detailed procedure

1. Move `input-device.c`, `input-queue.c`, `input-capability.c`, and
   `input-keymap.c` from `src/kern/` directly to `src/drivers/`.  Update every
   architecture manifest, focused test command, and source audit in one
   buildable change; do not leave compatibility copies at the old paths.
2. Move `console-device.c` to `src/drivers/fs/console.c`, preserving its cdev,
   tty, HAL-key broker, and keyboard-evdev behavior exactly.  This is an
   ownership move, not the console legacy-UAPI removal Phase.
3. Move/rename the two physical mouse sources and headers to the HID subtree.
   Update platform includes and manifests without introducing wrapper sources.
4. In the PS/2 module, add backend-local evdev registration, capability table,
   open/close lifecycle, button transition state, and event publication.
   Remove all `mouse_device_set_backend()` and `mouse_input_report()` calls.
5. Implement the same complete, private frontend lifecycle in the PC-98
   bus-mouse module.  Preserve the one-time worker creation and safe IRQ masking
   across zero-to-one and one-to-zero reader transitions.
6. Remove the generic `mouse_device_register()` call from VFS initialization.
   `kern_platform_input_init()` now registers the selected physical pointer
   device after `input_core_init()` and console keyboard registration.  No code
   may depend on the resulting event number.
7. Delete `src/kern/mouse-device.c`, `include/kern/mouse-device.h`, the
   `/dev/mouse` cdev registration, and `include/uapi/zedbsd/mouse.h`.  Remove
   obsolete build entries and mouse-specific constants only after a repository
   consumer audit is empty.
8. Update comments, plans/tests referenced by active build commands, and driver
   diagnostics to describe evdev rather than `/dev/mouse`; do not rewrite
   historical evidence documents merely to hide the old milestone.
9. Add focused lifecycle/publication fixtures during execution.  Exercise two
   simultaneous readers, first-open failure/retry, last-close stop, motion,
   zero-motion button edges, unchanged buttons, and complete `SYN_REPORT`
   framing for both private implementations.
10. Run all input-core fixtures with their new paths, build supported targets,
    boot amd64 under QEMU, classify the production keyboard/pointer without
    names or numbers, and operate Xzed through the p007 evdev consumer.  Build
    and, where the maintained runner exists, boot PC-98 to validate its own
    independently registered pointer.

## Verification contract

At minimum, the implementation Queue must record equivalents of:

```sh
test ! -e src/kern/mouse-device.c
test ! -e include/kern/mouse-device.h
test ! -e include/uapi/zedbsd/mouse.h
test -e src/drivers/input-device.c
test -e src/drivers/fs/console.c
test -e src/drivers/hid/ps2-mouse.c
test -e src/drivers/hid/pc98-busmouse.c
rg -n '/dev/mouse|mouse_device_(register|set_backend)|mouse_input_report|\
zedbsd/mouse' src include userland platform Makefile
make -j16
git diff --check
```

The final `rg` must find no live implementation or consumer; historical plan
records are outside this deletion audit.  Re-run WS006 queue, capability, and
keymap fixtures using `src/drivers/input-*.c`.  KA-T070 must observe correct
motion and button press/release framing from both PC/AT PS/2 and PC-98
bus-mouse implementations.  KA-T071 must prove the final source/node/symbol
layout and absence of `/dev/mouse` in a booted system.

Use `make -j16`, not `make check`.  Use `qemu-system-x86_64` for amd64 runtime
evidence and disposable image copies for guest mutation.  Missing PC-98 runtime
infrastructure may leave only the PC-98 runtime portion `uncleared`; it does not
permit claiming full Phase completion from an amd64-only test.

## Completion conditions

- All generic input source implementations occupy their specified final paths,
  and every supported manifest/test command references only those paths.
- The console frontend is at `src/drivers/fs/console.c` with unchanged tty and
  keyboard-evdev behavior.
- PC/AT and PC-98 pointer drivers independently register, activate, publish,
  synchronize, and stop their evdev devices without shared mouse production
  code.
- Xzed remains usable through evdev after `/dev/mouse` disappears.
- `mouse-device.c`, its kernel header, the mouse UAPI header, the cdev node, and
  all live in-tree references are absent.
- KA-T070, KA-T071, applicable WS006 tests, supported builds, representative
  runtime boots, and `git diff --check` pass with evidence.

## Reconsideration conditions

Stop and request human review if removing `/dev/mouse` exposes an unrecorded
in-tree consumer, if the input core cannot express the required reader/start
lifecycle without a public UAPI change, or if a platform's hardware service
cannot safely be owned by its backend.  Do not respond by recreating
`mouse-device.c`, adding a new generic mouse facade, or retaining a silent
legacy cdev.  A need to move architecture console producers out of HAL is a
new Phase and is not folded into this one.

## Execution result

Completed on 2026-08-29 under `q026` without reaching a reconsideration
condition.

- The four generic input implementations now live directly below
  `src/drivers/`, and the unchanged console frontend now lives at
  `src/drivers/fs/console.c`.  All supported architecture manifests and the
  active WS006 input fixture commands name only those final paths; their
  established `include/kern/` interfaces remain unchanged.
- PC/AT PS/2 and PC-98 bus-mouse sources and headers now occupy their final
  `src/drivers/hid/` and `include/drivers/hid/` paths.  Each driver owns its
  input device, exact evdev capability set, button state, reader count,
  first-open hardware start, failed-start retry, and final-close stop.  The
  lifecycle mutex prevents half-started concurrent opens and reference
  underflow.  PC/AT keeps packet/IRQ work behind its controller lock; PC-98
  retains its one-time service worker and PPI interrupt gate.
- Both producers publish nonzero `REL_X`, then nonzero `REL_Y`, changed
  left/right/middle button values, and one `SYN_REPORT`.  Positive-down Y is
  preserved.  Final close serializes against an in-flight producer, publishes
  releases plus `SYN_REPORT` for held buttons, and clears evdev state before a
  later open; a pending inactive IRQ cannot publish a late frame.
- VFS initializes the input core, registers the console keyboard, then asks
  the platform to register its physical pointer.  The generic
  `mouse-device.c`, its kernel header, the legacy mouse UAPI, `/dev/mouse`
  registration, backend registry, and all live consumer/producer references
  are deleted.
- [`run-input-hid-host-test.sh`](../tests/run-input-hid-host-test.sh) passed
  ordinary `-Werror` and ASan/UBSan runs for both production driver sources.
  KA-T070 covers a failed first open and retry, two readers, last-close stop,
  signed motion, unchanged-button frames, zero-motion button edges, ordered
  `SYN_REPORT` framing, held-button close/reopen state, and suppression of late
  inactive IRQ publication.  KA-T071 verifies the final paths, registration
  sites, VFS ordering, and absence of every retired live symbol/path.
- The p007 Xzed production-linked fixture passed ordinary `-Werror` and
  ASan/UBSan runs after legacy removal.  WS006's LP64 and ILP32 evdev layout
  compiles, bounded queue fixture, capability/state fixture in ordinary and
  ASan/UBSan modes, and keymap fixture all passed from the relocated sources.
- A normal `make -j16` amd64 build passed.  Fresh supported `vmunix` builds for
  amd64, i386 PC/AT, i386 PC-98, rpi4, sun4u, and x68k all passed; the isolated
  cross-build outputs are below
  `plan/ws018-kernel-architecture/temp/q026-p008-*`.  The maintained WS006
  `qemu-evdev-capability.sh` acceptance also passed capability-only discovery.
- The amd64 `qemu-system-x86_64` run used a disposable image at
  `plan/ws018-kernel-architecture/temp/q026-p008-amd64-runtime.eh9KzR`.
  Boot registered `event0` as the console keyboard and `event1` as the PC/AT
  PS/2 mouse; Xzed independently classified and opened both roles.  Injected
  pointer motion was accepted and injected keyboard input produced `p008key`
  inside the Xzed session.  The guest-log SHA-256 is
  `dc76348e501f2f88056e36a4fb7e8f5efe878c38777e3031ecb6af8e94fd284c`.
- The maintained BR-T46 runner rebuilt and booted the PC-98 default cell under
  `plan/ws018-kernel-architecture/temp/q026-p008-pc98-runtime`; its result is
  `pass`, the guest reached `login:`, and every captured boot registered the
  independent `event1: NEC PC-98 bus mouse`.  The normalized guest-log SHA-256
  is `61bcd95f61193038ec6fdbd3ed014683478931e75592ba3622fe95788a2a9f8f`
  and the result-table SHA-256 is
  `de752e60791a70aa95eec7c0f04e0133294382a3f04274353c940dabb8267439`.
- An independent review found no P0--P2 issue in ownership, lifecycle,
  publication, state, manifests, or fixtures.  Its stale pre-change PC-98
  artifact observation was resolved by the maintained runner's fresh rebuild.
  The final live-source legacy audit and `git diff --check` passed; no Noct or
  repository `.internal/` source was inspected or changed.
