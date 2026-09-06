# Q085 / P012 acceptance results

Date: 2026-09-06. The user released the previous pre-acceptance hold and authorized
P048 implementation followed by the full saved 30-story campaign. Final result:
**30/30 ordinary and 30/30 ASan/UBSan PASS**, with recovery endpoints and no final
descriptor leak. No real wireless credentials are used.

## Reproduce

From the repository root:

```sh
sh plan/ws005-networking/tests/run-wifi-stories.sh
STORY_VARIANT=sanitize sh plan/ws005-networking/tests/run-wifi-stories.sh
```

`STORY_ID=29` selects one story for diagnosis. The runner retains its ordinary
and sanitized binaries under the WS temp directory. Final logs are
`plan/ws005-networking/temp/q085-wifi-scenarios/stories-final-ordinary.log` and
`stories-final-sanitize.log`. Earlier complete passes are also retained as
`stories-ordinary.log` and `stories-sanitize.log`.

The numbered definitions are in [scenarios.md](scenarios.md); their executable
command sequences and fault/recovery steps are in `tests/wifi-story-test.c` in
this workstream. Each row below is one multi-command story, not an assertion count.

| ID | Main acceptance condition | Ordinary | ASan/UBSan |
| --- | --- | --- | --- |
| 01 | Normal full lifecycle, exactly two DHCP acquisitions | PASS | PASS |
| 02 | Enable before automatic key notification converges | PASS | PASS |
| 03 | Repeated enable preserves active L2/L3 | PASS | PASS |
| 04 | Repeated idle enable then key remains usable | PASS | PASS |
| 05 | Wrong key, failure cleanup, corrected key | PASS | PASS |
| 06 | Manual pause survives profile B notification; explicit B works | PASS | PASS |
| 07 | Saving a disabled profile does not enable Wi-Fi | PASS | PASS |
| 08 | Disabled connect fails; manual profile recovery works | PASS | PASS |
| 09 | Unknown SSID leaves the active connection intact | PASS | PASS |
| 10 | Repeated disconnect and enable recovery | PASS | PASS |
| 11 | Repeated disable; pending stop stays visible, rejects enable, auto-retires | PASS | PASS |
| 12 | Manual profile requires explicit connect | PASS | PASS |
| 13 | Invalid other-owner store cannot destroy current ownership | PASS | PASS |
| 14 | Other-owner commands denied; profile notification ignored | PASS | PASS |
| 15 | No radios initially; later attach converges | PASS | PASS |
| 16 | First radio up failure permits healthy second candidate | PASS | PASS |
| 17 | First scan failure permits healthy second candidate | PASS | PASS |
| 18 | Partial list and known-status failure retain identity/healthy results | PASS | PASS |
| 19 | Malformed first-radio output cannot hide second candidate | PASS | PASS |
| 20 | Nonblocking scan-pending list, then eventual connection | PASS | PASS |
| 21 | Twenty stale-snapshot retries keep real WIFI1 stream within bounds | PASS | PASS |
| 22 | EOF before exit; closed-pipe hang is killed/reaped by real deadline | PASS | PASS |
| 23 | Carrier loss then explicit connect retires the previous identity | PASS | PASS |
| 24 | External L3 replacement survives old-owner retirement | PASS | PASS |
| 25 | Busy disconnect retains token and suppresses recovery until repaired | PASS | PASS |
| 26 | Failed DHCP cleans L2/L3 before the next connection | PASS | PASS |
| 27 | Failed candidate cleanup prevents trying a second connection | PASS | PASS |
| 28 | Removed ifindex cannot mutate a replacement using the same name | PASS | PASS |
| 29 | Real concurrent list/profile/disconnect/disable during background work | PASS | PASS |
| 30 | Lost child-wait result is a failure; retry restores usable state | PASS | PASS |

All commands check the expected exit result, a bounded simulated command budget,
and at most one connected radio. Scenario checkpoints additionally verify daemon
state, owner, selected interface and L2/L3 claims; each story ends with successful
disable and no owned L3 or active radio. These are explicit checkpoints, not an
exhaustive assertion of every status field after every command. The child runner
uses real host deadlines, including the 5-second list timeout after both pipes
close. The aggregate FD check covers descriptors 0 through 255, where the fixture
allocates its small set of channels; sanitizer execution also enables leak checks.

## What is production and what is simulated

Separate translation units compile production `net/main.c` dispatch,
`networkd/main.c`, `wifi/main.c`, `wifi-child.c` and netutil. Production protocol,
wifi-conf, managed-wlan and confirmed code is linked. Requests pass through the
actual six-command grammar, ZNV2 encoder/decoder and authenticated daemon handler.
Wi-Fi children use real host fork/pipes/poll/signals/waitpid and real WIFI1
production/parsing; the exec adapter invokes the compiled wifi entrypoint and
implements its descriptor-close contract. No success stream is hand-authored.

Radio ioctls, RF outcomes, interface indices, monotonic radio time, peer identity,
credential storage and L3 services are controlled external boundaries. Actual
wifi-conf model validation/serialization is retained. DHCP crosses real fork/wait
but its executable boundary injects address/route/resolver effects. Story 29 uses
a separately forked client and actual wait-pump/deferred-dispatch paths; ordinary
requests use a synchronous socketpair server adapter. See
[boundary description](acceptance-boundaries.md).

This supplies the practical command/daemon integration smoke for P012. It is
host acceptance, not native QEMU or physical RTL8822BU/AX211 RF acceptance.
Kernel deferred-stop lifetime and both driver adapters have separate production
fixtures documented in [P048 results](../../ws004-hardware/phase048-wlan-deferred-stop/results.md).

## Maintained regressions and source corrections

All these runners under `plan/ws005-networking/tests/` passed their ordinary,
sanitizer and analyzer gates where provided (invoke each using `sh`):

| Runner | Final log under `temp/q085-wifi-scenarios/` |
| --- | --- |
| run-wifi-command-test.sh | p048-wifi.log |
| run-networkd-wifi-child-test.sh | p048-final-networkd-wifi-child.log |
| run-networkd-protocol-test.sh | p048-networkd-protocol.log |
| run-wifi-conf-store-test.sh | p048-wifi-conf-store.log |
| run-networkd-managed-wlan-test.sh | p048-final-networkd-managed-wlan.log |
| run-networkd-retire-stage-test.sh | p048-final-networkd-retire-stage.log |
| run-userland-network-recovery-test.sh | p048-userland-network-recovery.log |
| run-networkd-auth-test.sh | p048-networkd-auth.log |
| run-inet-ioctl-authorization-test.sh | p048-inet-ioctl-auth.log |

The old selector fixture was adapted to the production candidate-wave contract;
retirement tests distinguish a removed old identity from failed observation.
Direct down fixtures now require the status confirmation. Initial failures were
kept and localized before changing source or expectations. In this campaign,
the common final-scan retry, RTL failed-stop state retention, and preservation of
fcntl errno across pipe cleanup were corrected; details are in P048 results.
The preceding structural rewrite and review dispositions remain in
[implementation.md](implementation.md) and [review2-response.md](review2-response.md).

All three serialized amd64/PCAT/PC98 `make -j16` configuration builds pass, with
logs `p048-final-{amd64,pcat,pc98}.log` in the same temp directory. No aggregate
`make check`, hardware operations or commit was performed. Final changed-source
and fixture SHA-256 records are retained as `final-source-sha256.txt` there.
