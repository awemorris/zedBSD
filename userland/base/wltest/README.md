# wltest

A finite fullscreen Wayland application using standard Vulkan 1.0,
`VK_KHR_surface`, `VK_KHR_wayland_surface`, and `VK_KHR_swapchain`.
It uses standard `wl_compositor` and `xdg-shell` client interfaces. No GPU ioctl,
Venus command, zedBSD buffer factory, or kernel handle operation is part of the
application. The selected Vulkan library owns its WSI transport.

```
/bin/zdesktop --socket=/tmp/wayland-0 --timeout=150 &
/bin/wltest --display=/tmp/wayland-0 --mode=fifo --frames=60
/bin/wltest --display=/tmp/wayland-0 --mode=mailbox --frames=60 --recreate-at=30
```

Without `--display`, the standard Wayland environment selects the connection.
The default invocation presents 60 frames, delaying 16 milliseconds between
frames. `--delay-ms` accepts 0 through 1000. The image contains three opaque
color regions and a moving white bar, all drawn by Vulkan GPU commands. It
requires an opaque RGBA8/BGRA8 UNORM surface, graphics/presentation queue, and
configured extent of at least 64 by 16 pixels.

`--verify-session` selects six frames and waits after each `WLTEST FRAME` line
for `next` followed by a newline. Each wait is bounded to 60 seconds. This
explicit capture mode gives the remote harness time to capture the native
scanout; it never reads image pixels back in the application. `--token` labels
one run. `--recreate-at=N` exercises standard old-swapchain replacement.

On another OS, compile these same application sources against that OS's
Vulkan and Wayland client libraries and the standard generated
`xdg-shell-client-protocol.h` plus its protocol metadata source. For example,
with those development dependencies already supplied:

```
cc -I/path/to/generated-xdg-shell main.c window.c renderer.c \
    /path/to/generated-xdg-shell/xdg-shell-protocol.c \
    -lwayland-client -lvulkan -o wltest
```

This is a build recipe, not a claim that a second OS runtime has been tested.
The zedBSD package obtains the selected standard protocol metadata from its
independent `libwayland-client.so` implementation.
