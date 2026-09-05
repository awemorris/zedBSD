# WS009 shared test cases

Parent: [WS009](../ws.md)

| Case ID | Required observation |
| --- | --- |
| DOC-T00 | Every relative Markdown file link resolves and navigation reaches all active WS/Phase documents |
| DOC-T10 | Public UAPI statements cite headers, implementations, and executable tests |
| DOC-T20 | A clean supported environment follows the build guide through image creation |
| DOC-T30 | Documented QEMU boot commands reach the stated loader/root/login outcome |
| DOC-T31 | Kernel parameter reference matches parser names, defaults, and unknown-key behavior |
| DOC-T40 | init/service/network examples match fixed YAML v1, root-only typed ZSV1, argument and interactive command grammar, atomic locking, and documented failure boundaries, then pass against the installed commands |
| DOC-T50 | Console, graphics, system, input, and GPU references distinguish current, deprecated, and planned interfaces |
| DOC-T60 | POSIX/SUS and `_XOPEN_SOURCE` claims agree with WS001, public headers, and compile/runtime tests |
| DOC-T70 | WLAN examples match the six interface-free `net wifi` forms, credential boundary, one managed connection, 30-second retry, and link-loss recovery without publishing secrets |
| DOC-T71 | Input documentation distinguishes physically confirmed USB HID/evdev from the still-present deprecated console event/key-state UAPI pending WS006 p009 |
| DOC-T72 | Build/boot documentation reproduces all three amd64 Variants and separates host Noct/bootstrap ownership from LLVM/sysroot-built target artifacts |

Link validation is shared across all WSs; producer WSs own behavioral evidence
while WS009 owns document accuracy and navigation.

## q046 reference reconciliation

`ws009-p004` closes DOC-T53 at the q044 producer boundary: the public evdev
reference is anchored to the public headers, production input/console owners,
LP64/ILP32 layout checks, and the ordinary plus ASan/UBSan ownership runner.
It deliberately does not claim live USB HID or final legacy-console removal.

`ws009-p005` closes DOC-T31 against BR-T42, BR-T44, the configured-loader
parser fixture, and retained q015/q031/q032 evidence. It separates current
required `zedbsd.cfg`/`BOOTZBSD.CFG` loader behavior from the old-handoff
compatibility default and from runtime `swapon`/`swapoff`.

## WS012 service-console handoff

As of 2026-08-28, the public contract is
[`docs/reference/init-services.md`](../../../docs/reference/init-services.md).
WS012 owns executable evidence for the strict YAML model and canonical writer,
concurrent atomic policy updates, ZSV1 framing and lifecycle, argument-mode
dispatch, the interactive console, and QEMU integration. That evidence is
indexed in the [WS012 shared test cases](../../ws012-service-console/tests/README.md).

WS009 owns continuing public-document accuracy and navigation. DOC-T40 must
therefore compare examples against the WS012 evidence rather than infer
behavior from display text or the legacy `/etc/service.d/` assignment parser.
In particular, future edits must keep these boundaries explicit:

- `/etc/rc.conf` is the fixed YAML v1 policy; `/etc/service.d/NAME` remains the
  separate assignment-format definition.
- ZSV1 is a bounded, typed, versioned root-only protocol; human CLI output is
  not its wire format.
- Runtime start/stop/restart and persistent enable/disable are different
  operations, and reload never reconciles running instances.
- Persistence precedes synchronous reload. A post-persistence reload failure
  leaves the new file committed, reports potentially stale runtime policy, and
  does not roll back over a possible concurrent writer.
- Interactive mode shares the argument dispatcher and per-update lock; it has
  no candidate configuration or session-wide lock.

## DOC-T00 command

Run the native Noct validator from the repository root:

```sh
build/NoctLang/build-static/noct \
  plan/ws009-documentation/tests/check-relative-links.noct docs plan
```

It recursively checks every Markdown link with a relative file target. Web,
mail, absolute-path, and same-document fragment links are intentionally outside
this file-existence check. Ignored disposable `temp/` trees and prohibited
`.internal/` material are not product/planning documents and are not traversed.

## DOC-T70--T72 command

Run the completed-producer reconciliation check from the repository root:

```sh
build/NoctLang/build-static/noct \
  plan/ws009-documentation/tests/check-completed-producer-docs.noct .
```

It compares the managed-WLAN command/state/credential claims, USB HID and
consumer state, amd64 Variant labels, Noct release, LLVM cache, and sysroot
ownership against current production source. It also requires every product
document to use a `current`, `experimental`, `deprecated`, or `planned` status
banner and rejects retained internal paths in `docs/`.

Q072 uses this check together with DOC-T00 and `git diff --check` to complete
`ws009-p006`; it does not rerun the already accepted producer runtime matrices.
The final run passed 11 product-document status banners, while DOC-T00 passed
all 1,803 permanent `docs/` and `plan/` relative links.
