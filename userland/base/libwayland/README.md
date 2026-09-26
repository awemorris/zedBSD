# Independent Wayland client library

`libwayland-client.so` supplies the selected standard client transport for
Vulkan Wayland WSI and ordinary applications. Public headers live below
`include/libc/wayland/`; thin `<wayland-client.h>` and
`<xdg-shell-client-protocol.h>` entry headers preserve standard include spelling.
The base package is `libwayland-client` in `base/libwayland`, installed at
`/lib/libwayland-client.so` with the same SONAME.

The implementation owns real AF_UNIX connections, native-endian Wayland framing,
dynamic proxies, registry version negotiation, request/event arguments,
SCM_RIGHTS, partial nonblocking I/O, listener dispatch and independent queues.
Proxy wrappers inherit the original identity while selecting a separate queue
for constructed objects. WSI can therefore receive buffer releases and frame
callbacks without invoking the application's queue. `prepare_read_queue`,
`read_events` and `cancel_read` coordinate socket readers using a mutex and
condition variable. Callbacks and poll run outside the connection mutex.

Each live proxy generation is retained independently by its caller, wire map,
wrappers and queued events. Local destruction suppresses callbacks; only the
server's `delete_id` acknowledgement makes its wire ID reusable. Descriptor
arguments are duplicated with CLOEXEC while marshalling. The original fd can be
closed immediately; the queued duplicate survives until the first successful
sendmsg transfers it. Partial byte sends never retransmit rights. Received rights
are CLOEXEC and belong to the callback only when delivered; discarded events,
failed connections and disconnect recover undelivered rights. Byte and fd queues
are independent: a complete message waits for rights arriving in a later recvmsg.

Normal public Wayland ownership rules apply: the caller synchronizes concurrent
use of the same event queue/object and stops all users before disconnecting.
Callers destroy their own proxies and queues before `wl_display_disconnect`.
The display keeps its first fatal protocol/transport error; EAGAIN output
backpressure and a canceled read remain ordinary event-loop conditions.

`protocol.c` contains the independently maintained finite description tables and
typed request wrappers. `event.c` calls selected listeners using their exact
public callback types; custom event bindings use `wl_proxy_add_dispatcher`.
There is no libffi or imported upstream implementation. Utility arrays/lists
remain caller-owned. The private zed_gpu_buffer_v1 factory only carries an fd and
an opaque metadata array; GPU identity, bounds, immutable metadata and import
permissions are checked by the compositor/GPU layers. Revision two adds
`set_acquire_fence(surface, fd, generation_hi, generation_lo)`: a kernel fence
and its payload generation that the surface's next commit waits for.

[API-PROVENANCE.md](../../../include/libc/wayland/API-PROVENANCE.md) records pinned
upstream interface facts, hashes, selected scope, limitations and notices.
Selected core descriptions are wl_display/registry/callback/region/buffer v1,
wl_compositor/surface/output v4 and wl_seat/pointer/keyboard v5. Selected
xdg-shell descriptions are v1; their seat arguments are typed `struct wl_seat *`
as upstream declares them. wl_shm, wl_subcompositor, wl_touch, server-created
new_id events, a public server library and general typed callback FFI are
outside this library's current scope.

Input (WS031 p013) uses the upstream client API names, signatures, listener
layouts and enum values: `wl_seat_get_pointer`/`get_keyboard`/`release`/
`destroy`, `wl_pointer_set_cursor`/`release`/`destroy`, `wl_keyboard_release`/
`destroy` and the three `*_add_listener` functions. `wl_pointer_listener` keeps
the upstream member order including `axis_value120` and
`axis_relative_direction`; those two belong to pointer v8/v9, which are not
described, so they are never called. `wl_seat.get_touch` is described only to
keep the request opcodes in order; no wrapper sends it. The wl_keyboard.keymap
descriptor is delivered to the listener, which owns and must close it; an
unobserved, suppressed or discarded keymap event closes it in the library.
`wl_fixed_to_int`, `wl_fixed_from_int`, `wl_fixed_to_double` and
`wl_fixed_from_double` are exported functions rather than inline helpers.
Custom event bindings use the public dispatcher contract. This is a minimal
client SDK, not full Wayland or desktop conformance.

Focused production-source test:

```sh
plan/ws014/phase006/tests/run-wayland-client.sh
```

The independent socket peer verifies default/private queue isolation, wrapper
inheritance, callback destruction before dispatch, delete_id lifetime,
fragmented messages, 4096 dynamic object identities, 60016-byte partial sends
with exactly one descriptor transfer, sender-close and callback-fd ownership,
CLOEXEC, delayed rights after complete message bytes, unsent-fd cleanup,
two-thread prepared-reader cancellation, malformed protocol rejection, compositor disconnect,
and the seat/pointer/keyboard request encoding, typed event dispatch, keymap fd
ownership and version gating of pointer v5 events. Ordinary and ASan/UBSan executions
pass. Public Wayland utility layouts and the Vulkan Wayland creation record
are checked on ILP32/LP64 in C and C++ by
`plan/ws014/phase006/tests/run-wayland-abi.sh`; that runner accepts the pinned
reference core directory and Vulkan Wayland header paths as arguments.
The full zedBSD target build and QEMU compositor acceptance are recorded
by the owning p006 result; host fixtures alone do not prove guest GPU display.

The p006 [implementation handoff](../../../plan/ws014/phase006/wayland-implementation.md)
records the private buffer factory, WSI image ownership, supported extents,
focused acceptance evidence and the remaining whole-Phase runtime verification.
