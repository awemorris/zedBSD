# WS004 Phase 026: Archer T3U Nano identity and firmware policy

Last updated: 2026-08-30

Phase ID: `ws004-p026`

Status: planned evidence and policy checkpoint; not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Freeze the exact first WLAN hardware target before a USB ID is bound or a
binary firmware file is distributed. The selected product is TP-Link Archer
T3U Nano, but a product name or vendor download page is not sufficient device
identity. This Phase records the purchased unit's label and descriptors,
separates verified V1.0 facts from later-revision inference, and decides the
provenance and licensing boundary for the required Realtek firmware.

No driver, firmware loader, WLAN UAPI, or command is implemented in this
Phase. `ws004-p027` through `p030` remain planning-book entries and are not
authorized for implementation by creating this Phase.

## Verified facts and inference boundary

| Claim | Classification | Evidence and consequence |
| --- | --- | --- |
| Archer T3U Nano V1.0 contains RTL8822BU | Verified documentary fact | FCC filing `2AXJ4T3UNANO` identifies the product and the internal photograph, page 6 of 6, shows a package marked `RTL8822BU`. The official TP-Link Windows archive also labels the device section as 8822B. |
| TP-Link USB `2357:012e` uses the RTL8822B USB driver | Verified software identity | TP-Link's INF maps `USB\VID_2357&PID_012E` in its 8822B section; Linux mainline maps `2357:012e` to `rtw8822b_hw_spec` in `rtw8822bu.c`. |
| The target is RTL8828BU | Rejected | No primary source found for this product supports RTL8828BU. The implementation name, documentation, diagnostics, and firmware selection must use RTL8822BU unless the purchased unit proves a different identity. |
| Archer T3U Nano V1.40/V1.46/V1.60/V1.80 is the same silicon and USB ID | Inference only | TP-Link regional pages expose later hardware revisions and may offer a shared driver archive. A compatible archive is not proof that every revision retains the same chip, USB ID, RF front end, or firmware. |
| The purchased adapter is `2357:012e`/RTL8822BU | Unverified until intake | The unit may enter `p028` only after its own label and complete USB descriptors match the frozen profile. A mismatch stops binding; it is not added as a speculative alias. |

The B and C suffixes are material. RTL8822BU is the 8822B family on USB and
uses `rtw8822b_fw.bin`; the Latitude's retained built-in RTL8822CE is the 8822C
family on PCIe and uses `rtw8822c_fw.bin`. They may share the future generic
WLAN core, but are not interchangeable chip or bus drivers.

## One read-only development-host intake

Before `p028` can be proposed for a Queue, capture one read-only evidence
record for the exact adapter that will be used in development. This is an
inventory action on an already working development host, not a zedBSD boot,
driver bind, firmware upload, or radio test. Prefer an existing SSH-accessible
Linux/FreeBSD host and one command such as `lsusb -v -d 2357:012e` or the exact
`usbconfig ... dump_all_desc` equivalent; do not request a candidate zedBSD
image merely to identify the adapter.

1. Photograph or transcribe the product label, model, region, and printed
   hardware revision. Redact the serial number from shared evidence.
2. On a known-working host, retain the complete device and configuration
   descriptors: `idVendor`, `idProduct`, `bcdDevice`, USB speed, manufacturer,
   product, every interface class/subclass/protocol, alternate setting, and
   endpoint address/type/direction/max-packet/burst values.
3. Record the host OS, inspection command, and raw descriptor output. Do not
   exercise a host driver or radio and do not add an unplug/replug campaign to
   this one inventory. A product string alone is never accepted as identity.
4. Require `2357:012e` and the exact vendor-specific interface tuple captured
   from the unit before enabling the RTL8822BU match. Do not match only vendor,
   product text, interface class, or a broad Realtek family ID.
5. Compare the descriptor with the TP-Link INF and Linux `rtw8822bu` table.
   If the ID, interface layout, endpoint topology, or reported revision differs,
   stop and create a new identity Phase. Do not widen `p028` in place.

Opening the enclosure is not required. FCC internal photographs establish the
documentary V1.0 chip claim; the non-destructive descriptor is the binding
authority for the purchased unit.

If this one read-only inventory cannot be obtained, finish the planned
documentary/firmware analysis but mark p026 `uncleared` with the missing host
evidence as its resume condition. Do not treat public `2357:012e` evidence as
permission to bind an uninspected purchased revision, and do not queue p028's
driver binding. This prevents a mistaken later revision from receiving 8822B
register writes or firmware.

## Firmware policy

Linux mainline names the 8822B payload
`rtw88/rtw8822b_fw.bin`. The candidate upstream source is the official
`linux-firmware` repository. The payload is not zlib-licensed source and must
not be committed, linked into, or represented as part of the permissively
licensed zedBSD base tree.

The blob inspected during planning reports firmware version `30.20.0`. That is
a research candidate, not a floating-version promise: p026 still pins the
exact upstream commit, bytes, size, and digest obtained for the approved
package, and p028 reports the parsed version from those pinned bytes.

The accepted distribution boundary is:

- zedBSD base contains the native driver and a fixed firmware request for
  `/lib/firmware/rtw88/rtw8822b_fw.bin`, but no Realtek binary;
- an optional, separately identified firmware package installs the unmodified
  upstream blob, its `LICENCE.rtlwifi_firmware.txt`, provenance metadata,
  upstream revision, byte size, and SHA-256 digest;
- the package and image manifest state the Realtek binary-firmware terms
  separately from the zedBSD source license; installation is an explicit
  packaging choice, never a silent build-time or runtime download;
- the binary is redistributed only unmodified and with the required copyright
  notice and disclaimer. The license's reverse-engineering/decompilation/
  disassembly restriction and limited patent language are recorded, not
  paraphrased away;
- `p028` accepts only the pinned size and digest selected by this Phase. A
  missing, truncated, substituted, or unapproved newer blob fails visibly and
  leaves the interface down; it never falls back to an embedded copy; and
- firmware updates are separate reviewed package changes with their own
  hardware regression evidence. A driver change does not silently advance the
  blob.

The kernel loads the file on the first interface open after the root filesystem
is available. Boot-time USB discovery may publish an uninitialized WLAN device,
but it must not claim a working radio. `ENOENT`, digest mismatch, unsupported
firmware header/version, upload timeout, and firmware-start failure remain
distinct diagnostics. If a safe kernel file-loading boundary cannot be
defined without a broader VFS or credential change, stop before `p028` rather
than adding an ad-hoc firmware-upload ioctl.

## Primary sources

- FCC Equipment Authorization System internal-photograph attachment for FCC
  ID `2AXJ4T3UNANO`:
  <https://apps.fcc.gov/eas/GetApplicationAttachment.html?id=5468516>
- TP-Link's official V1.40 download page, which proves that later labeled
  revisions exist but not their internal chip identity:
  <https://www.tp-link.com/uk/support/download/archer-t3u-nano/v1.40/>
- TP-Link's official driver archive containing the 8822B/`2357:012e` INF
  mapping:
  <https://static.tp-link.com/upload/driver/2025/202512/20251231/Archer%20T3U%20Nano.zip>
- Linux mainline 8822BU USB ID table and 8822B firmware selection:
  <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/rtw8822bu.c>
  and
  <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/realtek/rtw88/rtw8822b.c>
- Official `linux-firmware` provenance and Realtek license files:
  <https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/WHENCE>
  and
  <https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/LICENCE.rtlwifi_firmware.txt>
- FreeBSD's current rtw88 module makefile and manual. They are useful
  behavioral/inventory references only; no implementation is imported into
  the zedBSD base. USB is disabled in the module makefile and the manual does
  not establish a completed native 8822BU USB path:
  <https://cgit.freebsd.org/src/tree/sys/modules/rtw88/Makefile> and
  <https://man.freebsd.org/cgi/man.cgi?query=rtw88&sektion=4>

## Verification plan

1. Complete `HW-T32` with the exact adapter label and raw descriptor record.
   This is the single development-host inventory above; it is not a zedBSD
   physical acceptance run.
2. Extract the TP-Link archive without executing its installer; retain the
   archive digest and the exact INF stanza that maps `2357:012e` to 8822B.
3. Pin one upstream `linux-firmware` revision. Record the blob path, version
   reported by the accepted parser, byte size, SHA-256, WHENCE entry, and exact
   accompanying license text.
4. Review the optional-package manifest against the policy above. Verify that
   a normal base image contains no Realtek blob and performs no network fetch.
5. Prepare negative fixtures for absent, short, oversized, wrong-digest, and
   unsupported-header payloads for `p028`; this Phase records them but does not
   implement the loader.

## Completion conditions

- The exact physical unit has a V1.0-equivalent printed label and is proven as
  `2357:012e` with a retained complete read-only development-host descriptor,
  or the Phase remains `uncleared` with a new identity decision as the explicit
  resume condition. A later printed revision is not cleared merely because it
  reuses that VID:PID.
- Every document calls the verified target RTL8822BU; RTL8828BU is not retained
  as an alias or guess.
- Later TP-Link hardware revisions are explicitly marked inference-only until
  independently captured.
- One exact firmware revision, size, digest, upstream provenance, license text,
  install path, and update rule are frozen.
- The base-versus-optional-package license boundary and all failure behavior
  are accepted before `p028` is eligible for a Queue proposal.
- No zedBSD candidate is booted for p026; the first zedBSD physical observation
  is the single combined checkpoint owned with WS005 p008 after p030's
  automatic gates.

## Reconsideration boundary

Return to planning if the physical unit is not `2357:012e`, its descriptor is
not compatible with the Linux 8822BU match, the upstream blob cannot legally or
technically be distributed under the declared optional-package policy, the
firmware header cannot be validated before upload, or initialization requires
another undisclosed board-specific file. Those findings are not permission to
bind a neighboring USB ID, use an out-of-tree binary driver, or embed firmware
in the base kernel.
