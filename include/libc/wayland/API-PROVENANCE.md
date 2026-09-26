# Wayland public interface provenance

These headers and `userland/base/libwayland/` implement an independent selected
Wayland client ABI. No upstream client/server C implementation, scanner output,
libffi, or Linux dma-buf implementation is incorporated.

Wire opcodes, signatures, object versions and interface layouts were checked
against the following pinned primary interface descriptions on 2026-09-13:

| Description | Revision | SHA-256 |
| --- | --- | --- |
| [Wayland core protocol](https://gitlab.freedesktop.org/wayland/wayland/-/raw/1.23.1/protocol/wayland.xml) | Wayland 1.23.1 | `0c371e9c31f8178008a7ddecf431cbe12ec4b29ef7714803ecd3e20af925e7ed` |
| [xdg-shell protocol](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/1.36/stable/xdg-shell/xdg-shell.xml) | wayland-protocols 1.36 | `454c96a942bfd7b21acdceb74d189cee85858afb7e7d2274964c94f13616f69f` |

The [client ABI](https://gitlab.freedesktop.org/wayland/wayland/-/blob/1.23.1/src/wayland-client-core.h),
[client API contract](https://wayland.freedesktop.org/docs/html/apb.html), and
[wire format](https://wayland.freedesktop.org/docs/book/Protocol.html) supplied
behavioral/interface facts. The maintained finite C descriptions and wrappers
are independently expressed source, with no production generator.

Selected wire descriptions: wl_display/registry/callback/region/buffer v1,
wl_compositor/surface/output v4, wl_seat/wl_pointer/wl_keyboard v5,
xdg_wm_base/positioner/surface/toplevel/popup v1.
This library does not claim a complete Wayland SDK. It does not supply wl_touch,
wl_shm, wl_subcompositor, EGL, a public server library, or general C callback
FFI.

Input interfaces (added for WS031 p013) were checked against the same pinned
Wayland 1.23.1 description and client ABI: wl_seat requests get_pointer (0),
get_keyboard (1), get_touch (2), release (3, since 5) and events capabilities
(0), name (1, since 2); wl_pointer requests set_cursor (0, `u?oii`), release
(1, since 3) and events enter (`uoff`), leave (`uo`), motion (`uff`), button
(`uuuu`), axis (`uuf`), frame, axis_source (`u`), axis_stop (`uu`) and
axis_discrete (`ui`), the last four since 5; wl_keyboard request release (0,
since 3) and events keymap (`uhu`), enter (`uoa`), leave (`uo`), key (`uuuu`),
modifiers (`uuuuu`) and repeat_info (`ii`, since 4). The described versions stop
at 5: pointer axis_value120/axis_relative_direction (v8/v9) and keyboard v10 key
repetition are not described, and wl_touch is not supplied, so get_touch has no
wrapper. The public names, signatures, listener member order (including the
never-invoked v8/v9 pointer members) and enum values follow the upstream client
header; no upstream code or scanner output was copied. The xdg-shell seat
arguments now use `struct wl_seat *` as upstream declares them. Custom event interfaces use `wl_proxy_add_dispatcher`; selected interface
listeners have typed independent dispatch code. Extension arrays, strings,
objects, scalar/fixed values and fd arguments use the standard wire format.
Server-created new_id event objects are outside these selected interfaces and
are rejected rather than silently synthesized. The fd count per outgoing
message is limited by zedBSD's existing SCM_RIGHTS maximum of eight.

The zedBSD-original `zed_gpu_buffer_v1` factory is version 1. Opcode 0 destroys
the factory. Opcode 1 has signature `nha`: a new wl_buffer, one SCM_RIGHTS fd,
and a metadata array. The fd has no in-band placeholder word. Metadata is an
opaque versioned GPU descriptor verified by the server against the immutable
kernel resource record; the client transport neither interprets nor trusts it.
The application uses ordinary Wayland and Vulkan interfaces; only the WSI and
compositor use this factory. No linux-dmabuf-v1 interface is advertised. Its client header is not public: it lives with libwayland
(`userland/base/libwayland/zed-gpu-buffer-v1-client-protocol.h`), and only
libwayland and libvulkan's WSI include it. Other zdesktop clients that need a
non-standard zdesktop extension use libzdesktop (`<zdesktop.h>`).

## Protocol description license notices

For the source interface descriptions (not imported implementation), the
applicable MIT-style notices are retained here. The independent implementation
and original zedBSD protocol use the Zlib license stated in each source file.

Wayland core protocol:

```
Copyright © 2008-2011 Kristian Høgsberg
Copyright © 2010-2011 Intel Corporation
Copyright © 2012-2013 Collabora, Ltd.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

xdg-shell:

```
Copyright © 2008-2013 Kristian Høgsberg
Copyright © 2013 Rafael Antognolli
Copyright © 2013 Jasper St. Pierre
Copyright © 2010-2013 Intel Corporation
Copyright © 2015-2017 Samsung Electronics Co., Ltd
Copyright © 2015-2017 Red Hat Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
