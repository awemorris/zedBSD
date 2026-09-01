# WS004 P038 AX211 implementation notes

Last updated: 2026-09-02

These notes freeze the permissive-source implementation facts for the exact
P038 device and firmware. They are an implementation aid, not completion
evidence. Values described as local policy are zedBSD bounds, not Intel
hardware specifications.

## Source and license boundary

The implementation reference is the official OpenBSD source mirror at commit
[`0f464d413c50396e4e6cd70948f15613d6a73081`](https://github.com/openbsd/src/commit/0f464d413c50396e4e6cd70948f15613d6a73081):

- [`sys/dev/pci/if_iwx.c`](https://github.com/openbsd/src/blob/0f464d413c50396e4e6cd70948f15613d6a73081/sys/dev/pci/if_iwx.c)
- [`sys/dev/pci/if_iwxreg.h`](https://github.com/openbsd/src/blob/0f464d413c50396e4e6cd70948f15613d6a73081/sys/dev/pci/if_iwxreg.h)
- [`sys/dev/pci/if_iwxvar.h`](https://github.com/openbsd/src/blob/0f464d413c50396e4e6cd70948f15613d6a73081/sys/dev/pci/if_iwxvar.h)

Those files contain ISC-style portions and portions offered under a dual
BSD-3-Clause/GPLv2 choice. zedBSD may adapt only the ISC and BSD-3-Clause
terms, must retain every applicable copyright and permission notice, and must
record the source commit in the eventual code provenance. GPL-only Linux
`iwlwifi` source is facts-only reference and must not be copied.

The firmware is a separate, default-off package. Its immutable source,
digests, redistribution conditions, and license placement remain those frozen
by P037. Driver source licensing does not change the firmware-package license.

## Exact attach and transport identity

Match all of the following before enabling bus mastering, interrupts, or DMA:

| Field | Required value |
| --- | --- |
| PCI vendor/device | `8086:51f0` |
| subsystem vendor/device | `8086:4090` |
| PCI revision | `01` |
| BAR0 | memory BAR, at least `0x4000` bytes |
| MAC type | `IWX_CFG_MAC_TYPE_SO` (`0x37`) or `IWX_CFG_MAC_TYPE_SOF` (`0x43`) |
| RF type | `IWX_CFG_RF_TYPE_GF` (`0x10d`) |
| CDB | clear |

OpenBSD's `iwx_match()` matches `8086:51f0` without subsystem or revision and
then `iwx_find_device_cfg()` selects a runtime configuration. P038 must be
stricter: the generic OpenBSD row is supporting evidence, not authorization to
broaden the frozen tuple.

The first direct, PCI-visible diagnostic reads are:

- `IWX_CSR_HW_REV` at BAR0 `0x028`;
- `IWX_CSR_HW_RF_ID` at BAR0 `0x09c`.

Derive and validate:

```text
mac_type = (hw_rev & 0x000fff0) >> 4
rf_type  = (hw_rf_id & 0x00fff000) >> 12
cdb      = (hw_rf_id & 0x10000000) >> 28
```

The recorded RF-ID value evaluates to GF and non-CDB. It does not choose SO
versus SOF; only the runtime `IWX_CSR_HW_REV` read does that. The subsystem
value also cannot choose SO versus SOF. Do not access HBUS, PRPH, UMAC PRPH, or
device SRAM until card ownership, clock readiness, and `iwx_nic_lock()` have
succeeded.

The observed BAR0 is exactly 16 KiB. The highest MSI-X IVAR byte used by the
OpenBSD AX210-family path is `0x28be`; the conservative implementation also
retains room through the PBA region at `0x3000`. Requiring the observed
power-of-two `0x4000` BAR is therefore the P038 fail-closed rule.

OpenBSD assigns `8086:51f0` the following transport settings; preserve them
even though the marketed product is CNVio2:

```text
firmware             IWX_SO_A_GF_A_FW
PNVM                 IWX_SO_A_GF_A_PNVM
device family        IWX_DEVICE_FAMILY_AX210
integrated flag      0
LTR delay            NONE
low-latency XTAL     0
XTAL latency         0
SISO diversity       0
UHB supported        1
UMAC PRPH offset     0x300000
MAC-address CSR base 0x380
```

## Ownership, reset, and the shortest path to ALIVE

Use the OpenBSD order and names:

1. `iwx_prepare_card_hw()` sets
   `IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY`, polls for 50 microseconds, and on
   success sets `IWX_CSR_MBOX_SET_REG_OS_ALIVE`. Its fallback disables link
   power management, sets `IWX_CSR_HW_IF_CONFIG_REG_PREPARE`, and uses a finite
   retry loop. Failure is terminal for this attach attempt.
2. `iwx_sw_reset()` sets `IWX_CSR_RESET_REG_FLAG_SW_RESET` in
   `IWX_CSR_RESET` and waits 5 milliseconds for this AX210-family path.
3. `iwx_apm_init()` applies the L0s/L1A workarounds, sets
   `IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE`, and waits at most 25 milliseconds for
   `IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY`.
4. Configure MSI-X and RF-kill handling. Start with one vector if that is all
   zedBSD can own; do not assume the Linux-reported vector count is available.
5. `iwx_nic_lock()` sets
   `IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ`, waits 2 microseconds, then polls
   for at most 150 milliseconds until clock-ready is set and going-to-sleep is
   clear. Every PRPH/UMAC PRPH transaction is inside this ownership scope.
6. `iwx_start_fw()` acknowledges and masks stale causes, clears firmware
   RF-kill/command-blocked handshake bits, initializes the NIC/rings, and
   enables only firmware-load interrupts.
7. `iwx_ctxt_info_gen3_init()` publishes the Gen3 context and IML described
   below, enables automatic function boot, and writes `1` to
   `IWX_UREG_CPU_INIT_RUN` (`0xa05c44`) through the UMAC PRPH aperture.
8. `iwx_load_firmware()` waits at most 1 second for a valid ALIVE. Timeout,
   malformed ALIVE, or firmware error resets the device and unwinds DMA.
9. Load the exact PNVM and wait for PNVM completion before sending init/NVM
   commands.

All paths use one reverse-unwind state machine. Interrupts are disabled and
device DMA is quiesced before any published DMA object is unmapped or freed.

## Frozen API89 firmware parser

The only admitted runtime image is:

```text
intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode
size    1736748
sha256  c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514
```

Require the 88-byte `iwx_tlv_ucode_header`, `zero == 0`, and
`magic == IWX_TLV_UCODE_MAGIC` (`0x0a4c5749`). Each record is a little-endian
`u32 type`, `u32 length`, payload, then four-byte padding. Check addition and
round-up overflow before every range operation. Reject truncation, conflicting
singleton TLVs, more than 64 image sections in any DRAM class, an empty IML,
and unknown TLVs in this digest-pinned image.

Required boot TLVs are `IWX_UCODE_TLV_SEC_RT` (19) and
`IWX_UCODE_TLV_IML` (52). Correct init and scan also require:

- `IWX_UCODE_TLV_PROBE_MAX_LEN` (6);
- `IWX_UCODE_TLV_FLAGS` (18), with zero-length `PAN` (7) accepted as a marker;
- `IWX_UCODE_TLV_DEF_CALIB` (22);
- `IWX_UCODE_TLV_PHY_SKU` (23);
- `IWX_UCODE_TLV_NUM_OF_CPU` (27), restricted to 1 or 2;
- `IWX_UCODE_TLV_API_CHANGES_SET` (29);
- `IWX_UCODE_TLV_ENABLED_CAPABILITIES` (30);
- `IWX_UCODE_TLV_N_SCAN_CHANNELS` (31), bounded before allocation;
- `IWX_UCODE_TLV_FW_VERSION` (36);
- `IWX_UCODE_TLV_CMD_VERSIONS` (48), whose length must be a multiple of four.

Validate but do not use normal-path metadata/debug TLVs 32, 51, 54, 55, 57,
58, 60, 61, 68, 69, 256, 258, and `0x1000005` through `0x100000c`. OpenBSD
also ignores undocumented type 69; its presence in the frozen image is not a
license to accept new unknown types.

The frozen command-version facts are:

| Group/command | Command version | notification/response version |
| --- | ---: | ---: |
| `ALIVE`, `g0/c1` | unknown (`99`) | 6 |
| `SCAN_CFG`, `g1/c0c` | 5 | 0 |
| `SCAN_REQ_UMAC`, `g1/c0d` | 17 | 0 |
| `NVM_ACCESS_COMPLETE`, `g12/c0` | 1 | 0 |
| `NVM_GET_INFO`, `g12/c2` | 1 | 4 |
| `PNVM_INIT_COMPLETE`, `g12/cfe` | unknown (`99`) | 1 |

Dispatch on these exact versions. API number 89 alone does not establish any
command layout.

### Exact runtime sections

Each `SEC_RT` payload begins with a four-byte device offset. Allocate and copy
only the following data bytes after that offset. Separator entries are not DMA
objects.

| Class | Entries | Frozen offset/size inventory |
| --- | --- | --- |
| LMAC | 0-14 | `00440000/1656`; `00800000/32768`; `00000000/32768`; `00008000/32768`; `00010000/32760`; offsets `004b6000` through `004ee000` in `0x8000` steps, each `32768`; `004f6000/9176`; `00629980/22720` |
| separator | 15 | `IWX_CPU1_CPU2_SEPARATOR_SECTION` (`ffffcccc`), ignored |
| UMAC | 16-32 | `80440000/1656`; `c0080000/32768`; `c0088000/32768`; `c0880000/32768`; `80447000/32768`; offsets `80469000` through `804a9000` in `0x8000` steps, each `32768`; `804b1000/3220`; `80415000/4700`; `80409000/26236` |
| separator | 33 | `IWX_PAGING_SEPARATOR_SECTION` (`aaaabbbb`), ignored |
| paging | 34-59 | `00000000/1656`; offsets `01000000` through `010b8000` in `0x8000` steps, each `32768`; `010c0000/8192` |

The IML is exactly 13,944 bytes. The UMAC section of 3,220 bytes is specific to
the frozen P038 artifact and is a useful regression check against a floating
linux-firmware image.

## Gen3 DMA contract

All boot/ring objects are zero-filled, coherent, 64-bit-addressable,
single-segment DMA allocations. If the zedBSD DMA API does not guarantee CPU
to device visibility for coherent mappings, perform the equivalent PREWRITE
sync before publishing their addresses.

| Object | Count and size | Alignment |
| --- | ---: | ---: |
| `iwx_context_info_gen3` | 104 bytes | API default |
| `iwx_prph_scratch` | 1,660 bytes | API default |
| PRPH information/dummy indices | one 4,096-byte page | API default |
| ICT | 4,096 bytes | 4,096 |
| command TFD ring | 256 x 256 = 65,536 bytes | 256 |
| Gen3 byte-count table | 1,024 x 2 = 2,048 bytes | 128 |
| command slots | 256 x 324 = 82,944 bytes | 64 |
| RX free/transfer ring | 512 x 16 = 8,192 bytes | 256 |
| RX completion ring | 512 x 32 = 16,384 bytes | 256 |
| RX status/head | 2 bytes | 16 |
| RX buffers | 512 x 4,096 bytes, separately mapped | mapping default |
| IML | exactly 13,944 bytes | API default |
| LMAC/UMAC/paging | one exact-size object per inventory entry | API default |

`iwx_prph_scratch` is 76 bytes of control, 48 reserved bytes, and three arrays
of 64 64-bit image addresses. Populate scratch first, including the RX-free
ring and the LMAC, UMAC, and paging arrays. Populate the Gen3 context second:
PRPH-info and scratch addresses, RX status as completion-head, the dummy index
areas at page offsets 2,048 and 3,072, command TFDs as MTR, RX completions as
MCR, and the encoded ring sizes.

Publish in this order:

1. context physical address low/high to `IWX_CSR_CTXT_INFO_ADDR` (`0x118` and
   `0x11c`);
2. IML physical address low/high to `IWX_CSR_IML_DATA_ADDR` (`0x120` and
   `0x124`);
3. IML byte length to `IWX_CSR_IML_SIZE_ADDR` (`0x128`);
4. set `IWX_CSR_AUTO_FUNC_BOOT_ENA` in `IWX_CSR_CTXT_INFO_BOOT_CTRL`;
5. acquire the NIC, apply the OpenBSD LTR bootstrap setting, write
   `IWX_UREG_CPU_INIT_RUN`, and release the NIC.

LMAC and UMAC image buffers may be released only after accepted ALIVE. Paging
remains live until device stop.

## ALIVE v6 and exact PNVM

Require an exact 144-byte packed `iwx_alive_resp_v6` and
`status == IWX_ALIVE_STATUS_OK` (`0xcafe`). Reject error status `0xdead`, a
different notification version, a short/long payload, or a failed RX header.
The required fields are:

- status and flags;
- two 48-byte LMAC records, including versions, timestamp, and diagnostic
  pointers;
- one 16-byte UMAC record;
- all three 32-bit SKU words;
- IMR base, size, and enabled fields.

Diagnostic firmware pointers are never host pointers and are not dereferenced
without device-memory bounds. The exact 51f0 OpenBSD configuration does not
enable IMR; reject an inconsistent enabled/range combination. Do not expose or
retain any network identity while recording ALIVE diagnostics.

The only admitted PNVM is:

```text
intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm
size    55176
sha256  efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a
```

Select an outer `IWX_UCODE_TLV_PNVM_SKU` (64) only when all three words equal
ALIVE. Inside that section, require a `PNVM_VERSION` (62), an `HW_TYPE` (58)
matching runtime SO-or-SOF plus GF, and one or more nonempty `SEC_RT` (19)
segments. The deprecated `0xddddeeee` separator is the only ignored section.

The frozen file has four relevant SKU alternatives. Each supports both
`0037:010d` and `0043:010d`; their two segment-size alternatives are:

```text
1656 + 12004
1656 + 12184
1656 + 11928
1656 + 12052
```

The ALIVE SKU, not PCI subsystem or RF-ID, selects one alternative. A zero SKU,
no exact SKU/HW match, a missing PNVM, more than 64 segments, or zero total
payload is terminal in P038.

The frozen firmware advertises `IWX_UCODE_TLV_CAPA_FRAGMENTED_PNVM_IMG`.
Allocate a 512-byte `iwx_pnvm_info_dram` pointer table plus one exact-size DMA
object for each selected segment. Put the pointer-table address and the sum of
segment bytes in scratch, synchronize them for device read, then under NIC
ownership write `IWX_UREG_DOORBELL_TO_ISR6_PNVM` (`1 << 20`) to
`IWX_UREG_DOORBELL_TO_ISR6` (`0xa05c04`). Require version-1
`PNVM_INIT_COMPLETE` within 2 seconds.

## NVM and channel discovery

The first NVM pass is `iwx_run_init_mvm_ucode(sc, 1)`:

1. load firmware, accept ALIVE, load PNVM, and run `iwx_post_alive()`;
2. send System-group `IWX_INIT_EXTENDED_CFG_CMD` (`g2/c03`) v1 with
   `IWX_INIT_NVM`;
3. send Regulatory/NVM `IWX_NVM_ACCESS_COMPLETE` (`g12/c00`) v1;
4. require `IWX_INIT_COMPLETE_NOTIF` (`g0/c04`) within 2 seconds;
5. send the four-byte-zero `IWX_NVM_GET_INFO` (`g12/c02`) v1 request;
6. require an unfailed v4 response of exactly 468 bytes within the common
   1-second command deadline.

The v4 response supplies general flags/version/board type/address count, MAC
SKU band and PHY-mode flags, TX/RX chain masks, LAR state, a channel count, and
110 32-bit channel profiles. It does **not** supply the station address.
Acquire the NIC and read the strap words at CSR base `0x380 + 0x8/+0xc`, with
OTP fallback at `0x380 + 0/+0x4`; apply the OpenBSD byte order and reject zero,
broadcast, multicast, and the reserved sentinel. Never print that address.

Unlike current OpenBSD, validate `n_channels <= 110` and consume only that many
profiles. Intersect them with the fixed Intel channel-number table and the
P038 2.4-GHz/20-MHz scope. Active probing is permitted only when NVM/MCC marks
the channel valid and active. Require the 2.4-GHz SKU bit, nonzero TX/RX chain
masks, and at least one valid 2.4-GHz channel.

After the read-NVM pass, stop/reset and perform a fresh operational firmware
load. Do not reuse partially initialized firmware state.

## Operational init and first scan

Mirror the proven OpenBSD command order instead of speculating that commands
can be omitted:

1. `iwx_run_init_mvm_ucode(sc, 0)`: ALIVE, PNVM, post-ALIVE,
   `INIT_EXTENDED_CFG`, `NVM_ACCESS_COMPLETE`, and `INIT_COMPLETE`;
2. `iwx_send_tx_ant_cfg()` / `IWX_TX_ANT_CONFIGURATION_CMD` (`0x98`);
3. skip `iwx_send_phy_cfg_cmd()` because this exact configuration has SISO
   diversity disabled;
4. `iwx_send_bt_init_conf()` / `IWX_BT_CONFIG` (`0x9b`);
5. `iwx_send_soc_conf()` with the exact 51f0 discrete/zero-latency settings;
6. `iwx_send_dqa_cmd()` because the pinned firmware advertises DQA;
7. `iwx_config_ltr()` only when PCIe LTREN is enabled;
8. `iwx_send_temp_report_ths_cmd()` because the pinned firmware advertises
   firmware CT-kill;
9. `iwx_set_pslevel()` with power saving disabled for the first path;
10. if LAR is enabled, `iwx_send_update_mcc_cmd("ZZ")` and validate its
    returned channel map;
11. `iwx_config_umac_scan_reduced()` using exact `SCAN_CFG` v5 and validated
    TX/RX chain masks;
12. `iwx_disable_beacon_filter()`.

Each synchronous command has the OpenBSD 1-second acknowledgement deadline.
Failure stops the device; initialization does not continue in a degraded
state.

The first scan does not require a PHY context, MAC context, binding, or station
entry in the OpenBSD path. `iwx_initiate_scan()` must dispatch exact
`SCAN_REQ_UMAC` v17 to the equivalent of `iwx_umac_scan_v17()`:

- UID 0, one iteration, priority `IWX_SCAN_PRIORITY_EXT_6`;
- v11 general parameters and v7 channel parameters;
- only NVM/MCC-approved 2.4-GHz channels, at most 14;
- forced passive for the first hardware gate;
- active dwell 10 TU, passive dwell 110 TU, full-scan adaptive budget 300 TU;
- probe-request construction bounded by TLV `PROBE_MAX_LEN` before any later
  active/directed scan is enabled.

The request acknowledgement deadline is 1 second. Receive and validate scan
MPDU notifications until `IWX_SCAN_COMPLETE_UMAC` (`0x0f`) or
`IWX_SCAN_ITERATION_COMPLETE_UMAC` (`0xb5`). OpenBSD has no whole-scan timeout;
P038 adds a **local 5-second watchdog** for the at-most-14-channel passive
scan. Expiry aborts the scan if the firmware still accepts commands, then
stops/resets the device. The value is a conservative local bound to validate
on the exact machine, not an Intel timing guarantee.

Do not create association contexts or transmit authentication/data until a
truthful scan result has passed the existing common WLAN bounds. WPA2/CCMP
then follows the P038 phase contract and the existing common controlled-port
state; firmware scan completion alone never raises carrier.

## Timeouts and hard bounds

| Operation | Bound |
| --- | ---: |
| initial NIC-ready poll | 50 microseconds |
| APM clock-ready | 25 milliseconds |
| NIC ownership | 150 milliseconds |
| AX210-family software-reset delay | 5 milliseconds |
| ALIVE | 1 second |
| synchronous firmware command | 1 second |
| PNVM completion | 2 seconds |
| init completion | 2 seconds |
| first passive scan completion | 5 seconds, local P038 policy |
| firmware sections per DRAM class | 64 |
| one firmware section | 32,768 bytes maximum in the frozen inventory |
| TX command entries | 256, never queue all 256 simultaneously |
| RX entries/buffers | 512 / 4,096 bytes each |
| NVM channel profiles | 110 decoded; at most 14 admitted to P038 scan |
| command payload | checked against the 4,096-byte wide-command transport bound |

Every timeout is generation-tagged. A late command response, ALIVE, PNVM,
init, or scan notification from an earlier generation is discarded. Ring
indices, payload lengths, DMA spans, and notification versions are checked
before use.

## Automatic proof and hardware proof boundary

Before direct boot, automatic tests can establish:

- exact firmware/PNVM size and SHA-256 reproduction;
- complete TLV parsing, four-byte padding, overflow rejection, exact command
  versions, section counts/order/sizes, and PNVM alternatives;
- compile-time packed-structure sizes and endian codecs;
- DMA count/alignment/address arithmetic and ring wrap;
- a mocked MMIO trace for ownership, reset, context/IML publication, boot kick,
  timeouts, stale notifications, and reverse unwind;
- malformed ALIVE, PNVM, NVM, command response, RX event, and scan event
  rejection;
- a fake-transport 2.4-GHz scan and the existing WPA2/CCMP controlled-port
  suites without a public WLAN UAPI change.

Only a bounded direct boot on the exact P038 machine can establish:

- the runtime SO versus SOF value and successful CNVio2 platform ownership;
- actual MSI/MSI-X delivery, DMA/IOMMU coherency, and ALIVE status;
- the ALIVE SKU and therefore the selected PNVM alternative;
- PNVM and init completion on this board;
- the actual NVM chain masks, regulatory channel set, and usable RF-kill state;
- scan RF results and the later association, CCMP, DHCP, and IP checkpoint.

The transport identifiers, PCI subsystem, firmware metadata, or a Linux host
success cannot substitute for those direct-boot gates. No network identity,
credential, or machine-specific network value may enter the evidence.

## Decision boundary

The exact 2.4-GHz passive scan and subsequent WPA2-CCMP normal path are
implementable without another human protocol-design decision: permissive
OpenBSD code supplies an executable transport/firmware reference, and the
existing zedBSD WLAN core owns authentication, CCMP, and controlled-port
semantics. Implement in staged hardware gates and fail closed.

A new human decision is required only to broaden the frozen PCI or firmware
matrix, accept another command/API version, omit part of the proven init
sequence, enable active transmission before regulatory resolution, change the
firmware redistribution boundary, or add a public WLAN semantic. Failure of a
hardware gate is evidence to diagnose, not permission to guess a proprietary
CNVio2 behavior.
