# Wayland public interface provenance

These headers and `userland/desktop/libwayland/` implement an independent selected
Wayland client ABI. No upstream client/server C implementation, scanner output,
libffi, or Linux dma-buf implementation is incorporated.

Wire opcodes, signatures, object versions and interface layouts were checked
against the following pinned primary interface descriptions on 2026-09-13:

| Description | Revision | SHA-256 |
| --- | --- | --- |
| [Wayland core protocol](https://gitlab.freedesktop.org/wayland/wayland/-/raw/1.23.1/protocol/wayland.xml) | Wayland 1.23.1 | `0c371e9c31f8178008a7ddecf431cbe12ec4b29ef7714803ecd3e20af925e7ed` |
| [xdg-shell protocol](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/1.36/stable/xdg-shell/xdg-shell.xml) | wayland-protocols 1.36 | `454c96a942bfd7b21acdceb74d189cee85858afb7e7d2274964c94f13616f69f` |
| primary-selection-unstable-v1.xml (`unstable/primary-selection/`, the Debian package wayland-protocols 1.44-1 on the build host, 2026-09-28) | wayland-protocols 1.44 | `d568482ba84df6e531698b1f531810860995ca24d69495427fe43aee6017f52c` |
| tablet-unstable-v2.xml (`unstable/tablet/`, the same Debian package, 2026-09-28) | wayland-protocols 1.44 | `db291b574adb2d42d27f3d01a77723bb3350a7a32e6d33de16513521b42294a9` |
| text-input-unstable-v3.xml (`unstable/text-input/`, the same Debian package, 2026-09-29; interface version 1) | wayland-protocols 1.44 | `49048087a67011a8840bca889cd2b0ba374382be1ed54ec98adf7837fdca1982` |
| xdg-activation-v1.xml (`staging/xdg-activation/`, the Debian package wayland-protocols 1.44-1 on the build host, 2026-10-05; interface version 1; the client side is libwayland's private `xdg-activation-v1-client-protocol.h`, reached through libkeiland's kl_activation_* and kl_instance_*, and the compositor's activation.c) | wayland-protocols 1.44 | `d8418be2d5738d50aff788bef1c7574f33f26659aa045447ff2ef9b78c58fe01` |
| content-type-v1.xml (`staging/content-type/`, the Debian package wayland-protocols 1.44-1 on the build host, 2026-10-06; interface version 1; the client side is libwayland's private `content-type-v1-client-protocol.h`, reached through libkeiland's kl_window_set_content_type, and the compositor's content-type.c; ws122-p005b) | wayland-protocols 1.44 | `203d17a26baa2ab4a8c2c9cb737c953fa3263c73a0a8aa6e552a41b3eac2196d` |
| [input-method-unstable-v2.xml](https://gitlab.freedesktop.org/wlroots/wlroots/-/raw/a047c2a33ff7724a476892cc4fe5dcb803607ef5/protocol/input-method-unstable-v2.xml) (MIT) | wlroots 0.19.2 (`a047c2a33ff7724a476892cc4fe5dcb803607ef5`) | `99414dbad9458e71aa1fa01bc45f94ca6685787bfcb4d98948f72c1b45b60703` |
| [virtual-keyboard-unstable-v1.xml](https://gitlab.freedesktop.org/wlroots/wlroots/-/raw/a047c2a33ff7724a476892cc4fe5dcb803607ef5/protocol/virtual-keyboard-unstable-v1.xml) (MIT) | wlroots 0.19.2 (`a047c2a33ff7724a476892cc4fe5dcb803607ef5`) | `7ad7870003ecd592cae47dc19d277a609b7f18fd7b7be012623cf3225a7294f5` |

The [client ABI](https://gitlab.freedesktop.org/wayland/wayland/-/blob/1.23.1/src/wayland-client-core.h),
[client API contract](https://wayland.freedesktop.org/docs/html/apb.html), and
[wire format](https://wayland.freedesktop.org/docs/book/Protocol.html) supplied
behavioral/interface facts. The maintained finite C descriptions and wrappers
are independently expressed source, with no production generator.

Selected wire descriptions: wl_display/registry/callback/region/buffer v1,
wl_compositor/surface/output v4, wl_seat/wl_pointer/wl_keyboard v5,
xdg_wm_base/positioner/surface/toplevel/popup v3 (v1 until WS035 p076),
wl_subcompositor/wl_subsurface v1 (WS035 p077: requests destroy and
get_subsurface `noo`; destroy, set_position `ii`, place_above `o`, place_below
`o`, set_sync, set_desync; no events; checked against the pinned Wayland 1.23.1
description), wl_data_device_manager/wl_data_source/wl_data_device/wl_data_offer
v3 (WS035 p079; the requests, events, `since` versions and dnd_action values of
the same pinned description; their events reach listeners through the generic
dispatch, and wl_data_device.data_offer creates the server's wl_data_offer).
This library does not claim a complete Wayland SDK. It does not supply
wl_shm, EGL, a public server library, or general C callback FFI.

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
repetition are not described. wl_touch v5 (WS079 p013, touch-protocol.c) follows
the same pinned description: request release (0, since 3) and events down
(`uuoiff`), up (`uui`), motion (`uiff`), frame and cancel; shape and orientation
(since 6) are not described, and get_touch has the upstream wrapper `wl_seat_get_touch`. The public names, signatures, listener member order (including the
never-invoked v8/v9 pointer members) and enum values follow the upstream client
header; no upstream code or scanner output was copied. The xdg-shell seat
arguments now use `struct wl_seat *` as upstream declares them. Custom event interfaces use `wl_proxy_add_dispatcher`; selected interface
listeners have typed independent dispatch code. Extension arrays, strings,
objects, scalar/fixed values and fd arguments use the standard wire format.
Server-created new_id event objects are outside these selected interfaces and
are rejected rather than silently synthesized. The fd count per outgoing
message is limited by zedBSD's existing SCM_RIGHTS maximum of eight.

The zedBSD-original `kl_gpu_buffer_v1` factory is version 1. Opcode 0 destroys
the factory. Opcode 1 has signature `nha`: a new wl_buffer, one SCM_RIGHTS fd,
and a metadata array. The fd has no in-band placeholder word. Metadata is an
opaque versioned GPU descriptor verified by the server against the immutable
kernel resource record; the client transport neither interprets nor trusts it.
The application uses ordinary Wayland and Vulkan interfaces; only the WSI and
compositor use this factory. No linux-dmabuf-v1 interface is advertised. Its client header is not public: it lives with libwayland
(`userland/desktop/libwayland/keiland-gpu-buffer-v1-client-protocol.h`), and only
libwayland and libvulkan's WSI include it. Other clients of the desktop's compositor that need
the compositor's own extension use libkeiland (`<keiland.h>`).

xdg-shell version 3 (added for WS035 p076) was checked against the same pinned
wayland-protocols 1.36 description: xdg_positioner requests set_reactive (7,
``), set_parent_size (8, `ii`) and set_parent_configure (9, `u`), and
xdg_popup request reposition (2, `ou`) and event repositioned (2, `u`), all
since 3. Version 2 adds only the tiled toplevel states (enum values). The
wrappers, the `repositioned` listener member and the `_SINCE_VERSION` names
follow the upstream client header; no scanner output was copied.

The primary selection (added for WS035 p100) was checked against the pinned
primary-selection-unstable-v1 description: zwp_primary_selection_device_manager_v1
v1 requests create_source (`n`), get_device (`no`), destroy; zwp_primary_selection_device_v1
requests set_selection (`?ou`), destroy and events data_offer (`n`), selection (`?o`);
zwp_primary_selection_offer_v1 requests receive (`sh`), destroy and event offer (`s`);
zwp_primary_selection_source_v1 requests offer (`s`), destroy and events send (`sh`),
cancelled.  The header `wayland/primary-selection-unstable-v1-client-protocol.h`
follows the upstream client header's names; its events reach listeners through the
generic dispatch.

The tablet protocol (added for WS079 p003) was checked against the pinned
tablet-unstable-v2 description, version 1: zwp_tablet_manager_v2 requests
get_tablet_seat (`no`), destroy; zwp_tablet_seat_v2 request destroy and events
tablet_added (`n`), tool_added (`n`), pad_added (`n`); zwp_tablet_tool_v2 requests
set_cursor (`u?oii`), destroy and events type (`u`), hardware_serial (`uu`),
hardware_id_wacom (`uu`), capability (`u`), done, removed, proximity_in (`uoo`),
proximity_out, down (`u`), up, motion (`ff`), pressure (`u`), distance (`u`), tilt
(`ff`), rotation (`f`), slider (`i`), wheel (`fi`), button (`uuu`), frame (`u`);
zwp_tablet_v2 request destroy and events name (`s`), id (`uu`), path (`s`), done,
removed.  zwp_tablet_pad_v2 is described (requests set_feedback `usu`, destroy;
events group `n`, path `s`, buttons `u`, done, button `uuu`, enter `uoo`, leave `uo`,
removed) only so that pad_added names an interface: the pad group, ring and strip
are not described and the compositor never announces a pad.  The header
`wayland/tablet-unstable-v2-client-protocol.h` follows the upstream client header's
names; its events reach listeners through the generic dispatch.

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

primary-selection-unstable-v1:

```
Copyright © 2015, 2016 Red Hat

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

tablet-unstable-v2:

```
Copyright 2014 © Stephen "Lyude" Chandler Paul
Copyright 2015-2016 © Red Hat, Inc.

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
