# gpu: the GPU command

Status: reference for `/sbin/gpu`

`/sbin/gpu` is root's command for the machine's GPUs. It lists the GPU nodes
(`/dev/gpuN`), shows what each node can do, reads its displays as the kernel
reports them, and asks the desktop session for a display's mode. It also shows
a render node's Vulkan, OpenGL ES, desktop OpenGL and Vulkan Video, resets a
device and reads its driver's reports. The source is
[userland/base/gpu/](../../userland/base/gpu/). The requests it uses are
described in [the GPU device interface](gpu.md).

gpu links the C library alone. The desktop's libraries (libvulkan, libEGL,
libGLESv2, libGL, libwayland-client and libkeiland) are opened when a command
needs them. A machine without the desktop can still list its GPUs, show their
capabilities, read their displays, reset them and read their reports.

## Synopsis

```
gpu [--porcelain]
gpu [--porcelain] GPU [show]
gpu [--porcelain] GPU caps [--capset=ID]
gpu [--porcelain] GPU display [list]
gpu [--porcelain] GPU display modes DISPLAY
gpu [--porcelain] GPU display edid DISPLAY [--raw]
gpu [--porcelain] GPU display set DISPLAY WIDTHxHEIGHT[@HZ] [--keep] [--timeout=S] [--session=SOCKET]
gpu [--porcelain] GPU display keep|revert|reset DISPLAY [--keep] [--timeout=S] [--session=SOCKET]
gpu [--porcelain] GPU vulkan [properties|features|limits|formats|extensions|queues]
gpu [--porcelain] GPU gl [es|desktop]
gpu [--porcelain] GPU video
gpu [--porcelain] GPU reset [--wait=MS] [--yes]
gpu [--porcelain] GPU diagnose [list | KIND [--argument=N] [--output=FILE]]
gpu help
```

- `GPU` is `gpuN` or `/dev/gpuN`. A number alone is not a node, so that it is
  not mistaken for a display's number. `N` is written without a leading 0, as
  the kernel names its nodes.
- `DISPLAY` is one of these: the display's number in `gpu GPU display list`,
  counted from 1; `id=N`, the kernel's display id; the display's whole name; or
  the start of its name. A name or a start that matches more than one display
  is refused, and the number or `id=N` is used instead.
- An option may stand anywhere on the line. `--porcelain` goes with every
  command. The other options belong to the commands shown with them, and an
  option given to another command is a wrong use.
- Numbers (`--capset`, `--argument`) are decimal, or hexadecimal after `0x`.
  `--timeout` is 1 to 3600 seconds. `--wait` is at most 600000 ms.
- A mode is `WIDTHxHEIGHT`, with an optional `@HZ` that may have up to three
  decimals (`1920x1080@59.94`). Without `@HZ` the compositor chooses the
  refresh.

## Who may use it

Only root may use gpu. Every command except `gpu help` first checks the
effective user ID. Anyone else gets `gpu: only root may use gpu` and the exit
status 1, before the line is read any further. gpu is not set-user-ID.

A person who changes their own desktop's resolution uses Settings. gpu is the
administrator's tool for a remote fix and for diagnosis.

## Commands

### The list

`gpu` lists every GPU node in `/dev`, in the order of the node numbers. Each
line shows the node, the driver's name, the node's roles (`render`,
`display`), and for a node with displays how many it has and how many have a
monitor. A node whose open waited more than 10 seconds is shown as
`busy (reset?)`. A node that does not open shows the error. The last line shows
the GPUs that a driver has attached but not yet published (`hw.gpu.attaching`)
and the GPUs that are held back for a start (`hw.gpu.start`); see
[the GPU sysctl variables](gpu-sysctl.md). A value that cannot be read is `?`.

```
# gpu
gpu0  powervr-display   display         displays=1 connected=1
gpu1  powervr           render
attaching=0 held=0
```

The list does not open libvulkan. A machine with no GPU node prints
`no GPU node` and exits with 0.

gpu opens each node it reports on, and closes it after its requests. The open
of a node whose device has failed may start the driver's recovery reset of
that device, as any open of the node does.

### show

`gpu GPU` (or `gpu GPU show`) shows one node: the driver, the roles, the
kernel's device id, the companion device (`none` when the driver names none),
the capabilities by name, the limits of one open, and one line for each
display. The Vulkan, GL and video lines are the render node's.

### caps

`gpu GPU caps` shows the node's capability bits as `GPU_GET_INFO` returns
them, one line for each bit set, with its name and value, then the driver,
the roles, the device id, the companion and the limits. The names are those of
the `GPU_CAP_` macros in lower case with dashes (`display-control` for
`GPU_CAP_DISPLAY_CONTROL`); a bit without a name is shown by its value. The
bits are described in [GPU capabilities](gpu-capabilities.md).

`--capset=ID` prints instead the capability set `ID`, version 0, that
`GPU_GET_CAPSET` returns, as hexadecimal bytes. gpu does not decode it.

### display

`gpu GPU display` (or `display list`) lists the node's displays as the kernel
reports them (`GPU_DISPLAY_QUERY`), in the kernel's order. Each line shows the
number, the name, whether a monitor is connected and whether the display is
lit, the current mode, the preferred size, the physical size and the kernel's
display id. A node without displays is refused with the nodes that have one:

```
# gpu gpu1 display
gpu: gpu1 has no display (roles render); display nodes: gpu0
```

`display modes DISPLAY` lists the modes the driver offers
(`GPU_DISPLAY_MODE`), in the driver's order with its default first. `*` marks
the current mode and `+` the monitor's preferred size.

`display edid DISPLAY` reads the display's EDID (`GPU_DISPLAY_EDID`) and
shows its base block: the manufacturer, the product code, the serial number,
the week and year of manufacture (or the model year), the EDID version, the
screen size in cm, the number of extension blocks, the product name and serial
text from the display descriptors, and whether the block's checksum is right.
`--raw` prints the bytes in hexadecimal instead. A display that has no EDID
is refused with `ENOENT`.

These reads claim nothing and show nothing.

`display set`, `keep`, `revert` and `reset` change a display's mode through
the compositor of the desktop session that holds the display. They use the
same requests as Settings, with the same trial. A new mode is shown at once,
and the mode shown before comes back unless the new one is kept within the
trial (15 seconds). On a terminal, gpu asks whether to keep it. Elsewhere, and
with `--timeout`, gpu waits that long for a keep from anyone. `--keep` keeps
the new mode at once. `--session=SOCKET` names the session's socket; without
it, gpu uses the session of the user who holds the seat. gpu never sends a
mode to the kernel itself. With no desktop session, and on the login screen,
the change is refused. While the screen is locked the compositor refuses a
change.

### vulkan, gl and video

`gpu GPU vulkan` shows the Vulkan physical device of a render node:
`properties` (the default), `features`, `limits`, `formats`, `extensions` or
`queues`. `gpu GPU gl` shows the OpenGL ES and desktop OpenGL versions,
extensions and limits (`es` or `desktop` alone). `gpu GPU video` shows the
Vulkan Video codecs, profiles and limits. Each is read in a child process of
its own. A node that is not a render node is refused with the render nodes.

### reset and diagnose

`gpu GPU reset` resets the node's device (`GPU_DEVICE_RESET`). Every program
that uses the device, the desktop included, loses it. On a terminal gpu asks
first; `--yes` does not ask, and without a terminal `--yes` is required.
`--wait=MS` is how long the reset may wait for the device.

`gpu GPU diagnose list` lists the report kinds the node's driver has.
`gpu GPU diagnose KIND` reads one report (`GPU_DIAGNOSE`) as raw bytes, in
hexadecimal or, with `--output=FILE`, into a file. `--argument=N` is the
number that some kinds take.

## Output

The output for people is in English, in columns separated by spaces.

Text that gpu did not write itself is escaped before it is printed. This
covers a driver's name, a display's name, the texts of an EDID, and the
strings that the Vulkan and GL drivers and the compositor return. Control
characters (below 0x20, and 0x7f), the C1 controls (U+0080 to U+009F) and
every byte that is not part of a correct UTF-8 character are written as
`\xHH`. Every correct UTF-8 character is printed as it is.

### The porcelain form

`--porcelain` prints one record a line, `kind key=value ...`, for scripts.
The first line is `porcelain version=1`. Besides the escapes above, a value
escapes the space, `=` and `\` as `\x20`, `\x3D` and `\x5C`, so that a value
never splits its record. Numbers are decimal, a set of bits is hexadecimal
with `0x`, and a truth is 0 or 1. A script ignores keys it does not know;
keys are added without changing the version, and the version changes when a
key changes its meaning or goes away.

| Kind | Printed by | Keys |
| --- | --- | --- |
| `gpu` | `gpu`, `gpu GPU` | `node`, `unit`, `state` (`ok`, `busy`, `failed`), and `error` when it is not `ok`; for `ok`: `driver`, `capabilities`, `roles`, `role` (the names, comma-separated), `device_id`, `companion_id`, `displays`, `connected` |
| `sysctl` | `gpu` | `attaching`, `held`; a variable that cannot be read is left out |
| `caps` | `gpu GPU caps` | `node`, `driver`, `capabilities`, `names` (comma-separated), `max_resources`, `max_resource_bytes`, `roles`, `role`, `device_id`, `companion_id`; with `--capset`: `node`, `capset`, `capset_version`, `bytes`, `data` (hexadecimal) |
| `display` | `gpu GPU`, `gpu GPU display` | `node`, `index` (from 1), `display_id`, `key` (the kernel's name), `connected`, `active`, `width`, `height`, `refresh_mhz`, `preferred_width`, `preferred_height`, `max_width`, `max_height`, `width_mm`, `height_mm`, `flags`, `generation`, `session` |
| `mode` | `display modes` | `node`, `index`, `display_id`, `key`, `ordinal`, `width`, `height`, `refresh_mhz`, `native`, `current`, `source` (`driver`) |
| `edid` | `display edid` | `node`, `index`, `display_id`, `key`, `bytes`, `total`, `valid`; for a valid block: `checksum`, `manufacturer`, `product`, `serial`, `week`, `year`, `model_year`, `version`, `revision`, `width_cm`, `height_cm`, `extensions`, `name`, `serial_text`; with `--raw`: `data` (hexadecimal) |
| `result` | a request that is refused or fails | `verb`, `error` (the errno's name, such as `ENODEV`, or its number) |

`session=none` on a `display` record means that the record is the kernel's
view and no desktop session was asked. When the session is asked, the record
also carries the compositor's keys: `label`, `anchor`, `selectable`, `why`,
`state` and `remaining_ms`. A change of mode prints `trial` records
(`key`, `outcome` = `kept` or `reverted`, `width`, `height`, `refresh_mhz`)
and a `result` record. Only an error is reported on the standard error; the
records go to the standard output.

## Exit status

| Status | Meaning |
| --- | --- |
| 0 | Done. |
| 1 | The request was refused or failed: not root, no such node, no display, a display not found, or a request the kernel or the session refused. |
| 2 | A wrong use: an unknown word or option, a missing argument, a malformed number or mode, or an option the command does not take. |

## Files

| File | Use |
| --- | --- |
| `/dev/gpuN` | The GPU nodes, opened with `O_RDWR` for each answer and closed again. |
| `hw.gpu.attaching`, `hw.gpu.start` | The last line of the list. |
