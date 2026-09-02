# WS010 shared test cases

Parent: [WS010](../ws.md)

| Case ID | Owner | Required observation |
| --- | --- | --- |
| SCT-T001 | `ws010-p001` | Noct checkout/build is reproducible and File/Process/System smoke operations work |
| SCT-T010 | `ws010-p002` | Each x86 build tool accepts a known-good fixture and rejects a malformed fixture |
| SCT-T020 | `ws010-p003` | Python invocation inventory is empty on all three required paths and `.internal/` is unreachable |
| SCT-T030 | `ws010-p004` | amd64 image reaches the init-ready/login marker in bounded QEMU |
| SCT-T031 | `ws010-p004` | PC/AT i386 image reaches the init-ready/login marker in bounded QEMU |
| SCT-T032 | `ws010-p004` | PC-98 i386 image reaches the init-ready/login marker in bounded PC-98 QEMU |
| SCT-T040 | `ws010-p005` | Every maintained userland Makefile exposes download/patch/build/install; downloaded upstream trees are not recursively registered |
| SCT-T041 | `ws010-p005` | Top-level download validates Noct 2.0.1 and both firmware caches, then succeeds again from local bytes without compiling an image |
| SCT-T042 | `ws010-p005` | Config-free preparation, corrupt-cache rejection, host-artifact recovery, and standalone Noct build/install preserve the declared lifecycle |
| SCT-T043 | `ws010-p005` | Seven network-free Noct acquisition fixtures reject wrong size/digest, unsafe path/type, interrupted transfer, and fuzz-only patching without publishing partial archive/source state |

Test programs and fixtures required by these cases live in this directory.
`.internal/` is excluded and must not be read, copied, or invoked.

`qemu-boot-capture.noct` is the bounded, shared VGA/GDC capture harness. The
accepted observations and exact emulator profiles are recorded in
[`boot-results.md`](boot-results.md).

Run the p005 lifecycle inventory with:

```sh
plan/ws010-scripting/tests/run-userland-lifecycle-audit.sh
```

The test includes tracked and newly added non-ignored Makefiles in the current
worktree, ignores downloaded source through `.gitignore`, and audits all four
targets without performing network I/O. Q063's acquisition and representative
execution results are retained in the
[Noct 2.0.1 evidence record](../../ws008-noct/tests/q063-noct-2.0.1-evidence.md).

Run the real acquisition/extraction recipes against bounded local fixtures
with:

```sh
plan/ws010-scripting/tests/run-noct-acquisition-fixtures.sh
```

The helper accepts only the fixture URL and never opens a network connection.
