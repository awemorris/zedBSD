# WS014 architecture review cases

Parent: [WS014](../ws.md)

The architecture review is on manual hold (`MB-005` in the master plan). These
cases remain a future discussion checklist and are not active Queue inputs.

The only current Phase is architectural discussion. These are design review
cases, not executable conformance tests.

| Case | Required design result |
| --- | --- |
| `GPU-D001` | Public objects, handles, ownership, lifetime, and process-exit cleanup are complete |
| `GPU-D002` | Memory, images, mapping, sharing, cache transitions, queues, and fences are coherent |
| `GPU-D003` | Malformed shader/command/input and GPU hang/reset paths fail without escaping isolation |
| `GPU-D004` | Mandatory, reduced GLES2-class, optional, and unsupported capabilities are explicit |
| `GPU-D005` | Display takeover, fallback, console, panic, and permission ownership are deterministic |
| `GPU-D006` | WS004, WS007, WS008, WS009, i915, Vulkan, and GLES responsibilities have one owner each |
