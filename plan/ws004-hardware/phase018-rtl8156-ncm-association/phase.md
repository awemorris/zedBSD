# WS004 Phase 018: RTL8156 CDC NCM association and binding diagnostics

Last updated: 2026-08-29

Phase ID: `ws004-p018`

Status: complete (`q028`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Make the generic CDC NCM driver recognize a standards-shaped NCM function that
uses a CDC Union descriptor without an Interface Association Descriptor, as
observed after inserting the Latitude test adapter (`0bda:8156`). Let the
existing driver-aware USB configuration selection choose that NCM function and
publish `ue0`, while retaining strict rejection of ambiguous or contradictory
function associations.

Make a later class-binding failure diagnosable from the console without
mistaking device-level class `00` for the class of its interfaces.

## Evidence that opened this Phase

The physical insertion reached successful USB configuration:

```text
usb0: device 1 port 3 0bda:8156 class 00 configured
```

`net show` nevertheless listed only `lo0`. Device class `00` means that class
identity is carried by interfaces; it does not rule out CDC NCM. The retained
USB core already enumerates all configurations and scores registered function
drivers before selecting one. The p014 matcher, however, accepts the CDC Union
association only when a matching two-interface NCM IAD also exists. An
RTL8156-shaped vendor/NCM/ECM descriptor set whose NCM configuration omits the
IAD therefore gives the NCM driver a zero match score, allowing the first
unsupported configuration to remain selected.

The simultaneous `usb1` port enumeration failure is recorded but is not
treated as the cause of the successfully configured `usb0` device. This Phase
must not modify xHCI port routing merely to hide that independent diagnostic.

## Dependencies

- `ws004-p010`: retained multi-configuration model and driver-aware
  configuration selection.
- `ws004-p014`: strict CDC NCM binding, data path, and lifecycle.
- `ws004-p015`: transactional interface binding and alternate ownership.

## Frozen association contract

1. The active communication interface still matches class/subclass/protocol
   `02/0d/00`.
2. Exactly one valid CDC Union functional descriptor is mandatory. Its master
   is the communication interface and its subordinate identifies a distinct
   CDC data interface in the same configuration.
3. The associated interface must still satisfy the p014 CDC data alternate and
   endpoint contract. Mere adjacency is never sufficient.
4. Zero IADs is valid. The Union descriptor alone supplies the normative
   control/data association.
5. If an IAD covers the communication interface, exactly one relevant IAD must
   describe the same two-interface NCM function. A conflicting class tuple,
   range, subordinate, duplicate, or ambiguous association is rejected.
6. Unrelated IADs in the same configuration do not invalidate a valid NCM
   function.
7. Header, Ethernet, NCM functional descriptor, MAC-string, negotiation, HCD
   concurrency, alternate selection, endpoint, and ownership checks remain
   unchanged unless a focused regression proves that a separate defect is
   within this Phase.
8. No VID:PID or fixed `bConfigurationValue` is used to force the result. The
   NCM function earns the winning score from its descriptors; unsupported
   vendor and ECM configurations remain unbound by this driver.

## Planned implementation

1. Refactor the p014 IAD predicate into optional corroboration:
   - return success when there is no IAD covering the NCM control function;
   - accept one exact two-interface NCM IAD that agrees with the Union;
   - reject relevant contradictory, duplicate, or ambiguous IADs.
2. Keep the Union parser mandatory and feed its subordinate interface number
   into the existing data-interface validation before returning a positive
   NCM match score.
3. Extend the production USB-core fixture with a retained
   three-configuration RTL8156-shaped device:
   - unsupported vendor-specific configuration first;
   - NCM `02/0d/00` communication plus CDC data configuration with a valid
     Union and no IAD;
   - ECM configuration after it.
   Assert that the NCM configuration wins by score, is selected using its own
   descriptor value, and binds once. In the production NCM-driver fixture,
   assert independently that the same Union-only association publishes
   exactly one `ue0`.
4. Add negative association cases for missing Union, contradictory IAD,
   duplicate/ambiguous relevant IAD, wrong Union master/subordinate, and a data
   interface outside the selected configuration. Retain positive coverage for
   a matching IAD and unrelated IADs.
5. Add concise generic USB binding diagnostics:
   - show the selected configuration value on successful enumeration;
   - when an active interface remains unbound, identify its interface number
     and class/subclass/protocol tuple;
   - when a matching driver's attach/probe fails, identify the driver and
     error without printing arbitrary descriptor contents.
   Suppress a false warning for a data sibling that the communication driver
   has claimed successfully.
6. Run the p010 USB-function/binding regressions, p014 production NCM fixture,
   relevant NCM wire/xHCI/net-device/USB Storage regressions, and static
   analysis declared by the existing Phase-owned scripts.
7. Run `make -j16` for the supported default build and the configured x86
   build already used by p014, then boot one disposable amd64 image with
   `qemu-system-x86_64`, q35, qemu-xhci, and USB Mass Storage to `login:` with
   no USB/storage/kernel regression marker.
8. After every automatic gate passes, request one Latitude boot of the exact
   resulting production image and one adapter insertion. The single action is
   to confirm selected NCM configuration and `ue0` publication; repeated
   success testing is deferred to the final physical networking acceptance.

## Automatic implementation result

The automatic candidate is complete. The generic NCM binding now requires a
valid CDC Union descriptor but accepts zero IADs. A relevant IAD remains strict
corroboration: duplicate, overlapping, malformed, or contradictory associations
are rejected. No Realtek VID:PID, configuration number, or vendor protocol was
added.

The USB core now reports the selected configuration value and, for every active
interface, one bounded binding outcome: bound driver, successful composite
sibling claim, no matching driver, or attach error. The ordinary q35 USB-root
log demonstrates the non-NCM regression path as
`configuration=1` followed by `interface 0 class 08/06/50 driver=usb-storage`.

Automatic evidence on 2026-08-29:

- the production NCM-driver fixture passes 1,283 checks in ordinary and
  ASan/UBSan modes plus GCC `-fanalyzer`; its Union-only case publishes `ue0`;
- the production USB-core fixture passes 1,404 checks in ordinary and
  ASan/UBSan modes plus analyzer; its vendor/NCM/ECM descriptor set selects
  `bConfigurationValue=2`, binds the NCM control interface, claims the data
  interface, and contains no attach-failure diagnostic;
- USB binding transactions pass 971 checks in both runtime modes and their
  production-source gate; NCM wire, concurrent xHCI, net-device hotplug, and
  USB Storage SCSI regressions pass;
- default amd64 `make -j16` and configured i386/PC/AT xHCI disk-image builds
  pass; and
- one disposable amd64 q35/qemu-xhci USB Mass Storage boot reaches `login:`
  with no USB-storage, overlay-write, xHCI-transfer, kernel-fault, or panic
  marker.

## Physical acceptance result

The user's single Latitude acceptance on 2026-08-29 passed the p018 boundary.
The RTL8156 selected configuration 2, the `02/0d/00` control interface bound to
`usb-cdc-ncm`, the `0a/00/01` sibling was claimed by that driver, `ue0` was
published with its MAC address, and `net show` listed `ue0`.

DHCP subsequently reported an error; static IPv4 assignment succeeded but a
peer ping did not, with an apparent stop on inbound activity. Those facts do
not retract the explicitly enumeration-only p018 result. They are recorded in
[`ws005-p001`](../../ws005-networking/phase001-usb-ncm-physical-datapath/phase.md)
as the physical carrier/data-path handoff.

## Completion conditions

- The production USB-core and NCM-driver fixtures together prove that an
  RTL8156-shaped three-configuration device selects and binds its IAD-less,
  Union-associated NCM function and publishes exactly one `ue0` without a
  VID-specific configuration quirk.
- A matching IAD remains accepted, absence of IAD remains accepted, unrelated
  IADs are ignored, and contradictory or ambiguous relevant IADs plus every
  malformed/missing Union case are rejected.
- The normal enumeration message identifies the selected configuration, and
  an unsupported or failed interface reports a concise, actionable binding
  outcome without misclassifying claimed siblings.
- Existing strict NCM, USB binding, xHCI concurrency, removable net-device,
  USB Storage, build, and QEMU USB-root gates pass.
- One final Latitude acceptance boot using the explicitly linked production
  image observes the RTL8156 NCM configuration bind and `net show` list `ue0`.
  Link, DHCP, packet transfer, reconnect, and multi-run reliability remain
  WS005 and are not falsely claimed here.

## Reconsideration boundary

Stop and mark the Phase `uncleared` rather than broadening it if the physical
device's selected function is vendor-specific, requires a Realtek vendor
initialization protocol or firmware download, lacks the declared CDC NCM
descriptors, or cannot publish `ue0`. Record the exact selected configuration
and interface tuples. Once `ue0` is published, a later carrier, DHCP, packet,
or xHCI runtime failure belongs in a distinct WS005 data-path Phase and does
not broaden this association Phase.
