# WS017: `/dev/graphics` linear-framebuffer fast path

Last updated: 2026-09-05

WSID: `ws017`

Status: Queue-ready; the initial-protection `mprotect` ceiling is selected

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: after the higher-priority WS009, WS011, WS019, and WS022 work,
Queue `ws017-p001` followed by p002--p004.

Shared tests: [WS017 test index](tests/README.md)

## Goals

- Preserve `/dev/graphics` as the portable drawing abstraction for planar,
  banked, indexed, accelerated, and other non-linear display devices.
- Add an optional `mmap` fast path when the active backend exposes a stable
  linear framebuffer (LFB).
- Describe fixed geometry, stride, 8/16/24/32-bit pixel layout, and direct-RGB
  masks without exposing the physical framebuffer address.
- Let Xzed render dirty regions directly into a mapped true-color LFB while
  retaining its current ioctl BLIT/FLUSH path as an automatic fallback.
- Complete the initial WS when Xzed visibly starts through the mapped LFB path
  under amd64 UEFI.

## Objective

The existing `/dev/graphics` design remains authoritative and valuable on
low-memory or retro systems that do not have an LFB. This WS adds one optional
capability; it does not replace device-independent graphics operations or make
linear memory a platform requirement.

The current amd64 boot-framebuffer backend already receives a fixed GOP/VBE
aperture and renders through a kernel mapping, but every Xzed presentation
converts a dirty rectangle into RGB24, copies it through ioctl, and converts it
again in the driver. Mapping that same aperture into the exclusive graphics
owner removes this copy/conversion boundary. No new mode-changing interface is
added: the mapping describes only the mode selected by the existing
`ZEDBSD_GRAPHICS_ENTER` operation.

## Fixed public contract

### Capability and query

- Add `ZEDBSD_GRAPHICS_CAP_LFB_MMAP` and a versioned
  `ZEDBSD_GRAPHICS_GET_LFB_INFO` ioctl after the existing request numbers.
- The query is valid only for the exclusive `/dev/graphics` owner after a
  successful `ENTER`. It succeeds with an unavailable flag for a non-linear
  backend; lack of an LFB is not an error that breaks fallback.
- The returned structure contains version/size, flags, pixel format, width,
  height, bits per pixel, byte stride, mapping length, offset from mapping base
  to the first visible pixel, and RGB shift/mask pairs. Reserved fields are
  zero.
- No physical/bus address is exposed.

### Pixel representation

- Mappable modes may be 8, 16, 24, or 32 bpp. Width, height, and mode remain
  fixed for the complete graphics-owner lifetime.
- Eight-bit LFBs are identified as indexed. RGB shift/mask fields are zero and
  the palette established by `ENTER` is not changed or queried by this WS.
  Xzed therefore retains its ioctl fallback for indexed 8-bit modes.
- 16/24/32-bit LFBs are direct RGB. A pixel is the unsigned little-endian value
  formed from 2, 3, or 4 consecutive bytes. For each component,
  `(pixel >> component_shift) & component_mask` extracts the normalized
  contiguous component. Masks are nonzero low-bit masks, shifted fields do not
  overlap, and every field lies inside the declared bpp. Unused/alpha bits are
  ignored.
- Xzed scales its internal 8-bit RGB channels to each returned component mask;
  it does not assume RGB565, RGB888, BGR888, RGBX8888, or BGRX8888.

### `mmap` and lifetime

- Userspace calls `mmap(NULL, mapping_length, PROT_READ | PROT_WRITE,
  MAP_SHARED, graphics_fd, 0)`. Device offsets other than zero, `MAP_PRIVATE`,
  a non-NULL address hint/fixed request, executable access, wrong length, or
  mapping before `ENTER` fail in this initial contract.
- The driver supplies a page-aligned, page-multiple mapping span which it can
  prove is wholly dedicated to this framebuffer aperture. The kernel never
  rounds a visible range outward and exposes prefix/suffix bytes merely for
  convenience. `data_offset` may describe a non-page-aligned first visible
  pixel only when the driver also proves ownership of the complete containing
  first and last pages; checked `stride * height` bytes must fit the owned
  span. Without that proof the LFB query reports unavailable and ioctl drawing
  remains active.
- Device pages are mapped with HAL device/non-cacheable attributes initially.
  A future write-combining cache policy is outside this WS.
- A device mapping retains the open file and graphics ownership. Closing the
  descriptor does not resume the console or admit another owner until every
  inherited/split mapping is unmapped or released at process exit. Final
  mapping release performs the existing driver leave/console-resume path once.
- Forked mappings remain shared and retain the same ownership lease.
- Executable access is always forbidden. Permissions may change only within
  the maximum originally granted by `mmap`: an initially RW LFB may transition
  `RW -> RO -> RW`, while an initially RO mapping can never gain write access.

## Selected `mprotect` ceiling

The user selected the initial-protection ceiling on 2026-09-05. Permissions
may change within the protection originally granted by `mmap`. An initial
`PROT_READ | PROT_WRITE` LFB mapping may perform `RW -> RO -> RW`; an initial
read-only mapping can never gain write access, and no device mapping can gain
execute access. Forked and split regions retain that same original maximum.

## Scope

- generic character-device mmap dispatch and device-backed VM-region lifetime;
- optional graphics-driver LFB description and validation;
- acceptance of an advertised 16-bpp mode by the existing central `ENTER`
  validation, without fabricating such a mode on current amd64 firmware;
- amd64 PC/AT boot-framebuffer exposure for the existing GOP/VBE handoff;
- 8/16/24/32 UAPI validation and conversion fixtures;
- Xzed direct dirty-rectangle composition for supported true-color layouts;
- automatic ioctl fallback for absent/invalid/indexed mapping;
- amd64 UEFI QEMU Xzed acceptance and fallback regression.

## Non-goals

- requiring LFB support from any graphics driver;
- changing screen size, selecting a new firmware mode, modesetting, page flip,
  vsync, double buffering, acceleration, GPU ownership, or write combining;
- adding an 8-bit palette query/mutation API;
- mapping PC-98 Cirrus/GDC apertures or changing their implementation;
- replacing `FILL`, `LINE`, `BLIT`, `FLUSH`, or glyph ioctls;
- claiming an i915, `/dev/gpuN`, Vulkan, or Wayland result.

## Dependencies

- [WS007](../ws007-graphics/ws.md) supplies the existing Xzed/session and
  `/dev/graphics` behavior that must remain intact.
- [WS003](../ws003-bringup/ws.md) supplies the validated amd64 UEFI framebuffer
  handoff and QEMU/OVMF boot path.
- [WS014](../ws014-gpu/ws.md) remains the separate future native-GPU workstream;
  this LFB mapping is a boot-framebuffer optimization and fallback.
- WS009 later owns public `/dev/graphics` documentation.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws017-p001` | [Device-mmap and graphics LFB UAPI](phase001-device-mmap-uapi/phase.md) | Queue-ready | Optional cdev mapping, selected initial-protection ceiling, lease lifetime, query validation, 8/16/24/32 layouts, and fallback pass host tests |
| `ws017-p002` | [amd64 boot-framebuffer backend](phase002-amd64-lfb-backend/phase.md) | Planned; Queue-ready after p001 | Existing GOP/VBE LFB is safely described and maps through `/dev/graphics`; non-linear drivers remain unchanged |
| `ws017-p003` | [Xzed mapped-LFB fast path](phase003-xzed-lfb-fast-path/phase.md) | Planned; Queue-ready after p001--p002 | Xzed renders true-color dirty rectangles/cursor through masks and falls back without behavior loss |
| `ws017-p004` | [amd64 UEFI Xzed acceptance](phase004-uefi-xzed-acceptance/phase.md) | Planned; Queue-ready after p001--p003 | QEMU/OVMF visibly launches the desktop through the mapped LFB and proves fallback remains operational |

## WS completion conditions

- The added UAPI is versioned, bounds checked, 32/64-bit stable, and exposes no
  physical address.
- A device mapping cannot outlive its graphics ownership or race console
  resume, close, fork, partial unmap, process exit, or a second open.
- Every mapped physical page is proven framebuffer-owned; an unaligned or
  partial-page visible extent never exposes adjacent RAM or unrelated MMIO.
- 8/16/24/32 layout validation rejects overlap, overflow, short stride/extent,
  and unknown format/flags. Indexed 8-bit operation remains explicit.
- All pre-existing `/dev/graphics` ioctl behavior and non-LFB fallback remain
  usable.
- Xzed selects mmap only for a valid direct-RGB LFB, writes correct colors and
  cursor pixels using stride/offset/masks, and otherwise uses the old path.
- `startx` under amd64 UEFI reaches Xzed/zwm/zshell/zterm with an observable
  mapped-LFB marker and working redraw/input.
- LFB-T001--T012, affected VM/graphics/X11 regressions, `make -j16`, and
  `git diff --check` pass without `make check` or `.internal/`.

## Reconsideration boundaries

Stop for a separate VM design Phase if device mappings cannot retain and split
their file/driver lease safely, or if close/process-exit cannot order unmap
before console resume. Stop for UAPI review if a real 8-bit target requires a
portable palette contract before mapping is useful. Do not expose a physical
address, infer page ownership by rounding, make mmap mandatory, or special-case
PC-98 Cirrus inside this WS.
