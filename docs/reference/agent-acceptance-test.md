# Agent Acceptance Test interfaces

Status: reference.

An agent drives a zedBSD machine over SSH: it moves the pointer, clicks,
types and takes screen captures, as a person at the machine would. These
interfaces exist only in test images. A release image has neither the
injector nor the capture.

## Building a test image with them

| Setting | Effect |
| --- | --- |
| `CONFIG_INPUT_TEST_INJECT := y` | The kernel has `/dev/input-inject` ([`<uapi/input-inject.h>`](../../include/uapi/input-inject.h)): a test pen, touch screen, touch pad, mouse or keyboard. Root only; up to four opens at once, each its own device. |
| `ZEDBSD_TEST_SCREEN_CAPTURE := y` | The compositor is linked with its screen capture. Without it the compositor has no capture code at all. |
| `ZEDBSD_USER_PROGRAMS += aat-input keiland-shot` | The two programs below. |

## aat-input

`aat-input` holds a relative mouse, an absolute pointer and a keyboard on
`/dev/input-inject`. The compositor sees them as it sees USB devices. Only
root may run it.

```text
aat-input start [--width W --height H]   the devices, and a server on /run/aat-input.sock (0600)
aat-input stop
aat-input COMMAND [ARGUMENT...]          one command; prints "ok" (status 0) or "error WHY" (status 1)
```

`start` prints `AAT-INPUT ready width=W height=H pid=N` once the devices
exist and the compositor has had time to take them. The absolute pointer
covers W x H, which should be the output's size in pixels (default 1920 x
1200), so coordinates are pixels.

| Command | Meaning |
| --- | --- |
| `move-to X Y` | the absolute pointer to pixel (X, Y) |
| `move DX DY` | the relative mouse (the compositor's acceleration applies) |
| `click [left\|right\|middle] [X Y]` | a click, at (X, Y) when given; left by default |
| `double-click [BUTTON] [X Y]` | two clicks |
| `down BUTTON`, `up BUTTON` | a button held or let go |
| `drag X1 Y1 X2 Y2 [STEPS]` | the left button held from one point to the other, in STEPS moves (20) |
| `wheel N`, `hwheel N` | the wheel turned N notches; N > 0 is up or right |
| `key NAME[+NAME...]` | keys pressed in order and released in reverse, e.g. `key leftmeta+l` |
| `key-down NAME`, `key-up NAME` | a key held or let go |
| `type TEXT` | ASCII text on the US layout, with Shift where a character needs it |
| `sleep MS` | a pause |

A key name is the evdev key's name in lower case: `a` to `z`, `0` to `9`,
`f1` to `f24`, `enter`, `esc`, `tab`, `space`, `backspace`, `delete`,
`insert`, `home`, `end`, `pageup`, `pagedown`, `up`, `down`, `left`,
`right`, `minus`, `equal`, `leftbrace`, `rightbrace`, `semicolon`,
`apostrophe`, `grave`, `backslash`, `comma`, `dot`, `slash`, `capslock`,
`leftshift`, `rightshift`, `leftctrl`, `rightctrl`, `leftalt`, `rightalt`,
`leftmeta`, `rightmeta`, `compose`, `sysrq`, `mute`, `volumeup`,
`volumedown`, `brightnessup` and `brightnessdown`. `ctrl`, `alt`, `shift`,
`super` and `meta` name the left keys.

A click holds the button for 30 ms; keys of a combination are 20 ms apart,
typed characters 15 ms.

## keiland-shot

```text
keiland-shot [--socket PATH] [--timeout MS] OUT.png
```

`keiland-shot` writes what the display shows, as the compositor composed it,
to an 8-bit RGB PNG, and prints `KEILAND-SHOT OUT.png WxH` (status 0) or
`KEILAND-SHOT error WHY` (status 1). The compositor copies the swapchain
image of its next frame (a Vulkan readback) and sends the pixels; the same
path works on the GPU of a real machine and in QEMU.

A compositor built with the capture listens on
`$XDG_RUNTIME_DIR/keiland-shot.sock`, or on `/tmp/keiland-shot.<uid>.sock`
when it has no runtime directory (the login screen). The socket belongs to
the compositor's user with mode 0600, so that user and root can capture.
Without `--socket`, `keiland-shot` asks every such socket and uses the
compositor that shows the display now, the session's or the login screen's.

The socket takes one line per connection: `PING` is answered `ACTIVE` or
`INACTIVE`; `SHOT` is answered `OK width height format` (a Vulkan format
number) followed by width x height x 4 bytes of pixels, or `ERROR why`.

A display whose swapchain images cannot be read back (the compositor logs
`ZWL SHOT unsupported`) still shows the desktop; captures then fail with
`ERROR unreadable`.
