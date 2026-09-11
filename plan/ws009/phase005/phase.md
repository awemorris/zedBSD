# WS009 Phase 005: kernel boot-parameter reference closure

Last updated: 2026-08-31

Phase ID: `ws009-p005`

Status: completed (`q046`, 2026-08-31)

Parent: [WS009 documentation](../ws.md)

Product reference: [kernel boot parameters](../../../docs/reference/kernel-boot-parameters.md)

Tests: [WS009 test index](../tests/README.md)

## Objective

Close DOC-31 by auditing the existing kernel boot-parameter reference against
the common parser, selector/root/swap consumers, all four x86 handoffs, and the
completed q015/q031/q032 evidence. Correct stale wording and add missing source
anchors without changing the implemented boot contract.

## Frozen boundary

- The key set remains `boot0`--`boot3`, exclusive `rootpart` or the paired
  `overlay-root`/`overlay-data` mode, `swap0`--`swap3`, and `init`.
- Preserve the documented loader-origin default, UUID/PARTUUID/device
  selectors, same-boot-disk rules, UEFI `/zedbsd.cfg`, BIOS configuration
  convergence, ignored UEFI LoadOptions, and non-x86 NULL-source compatibility
  boundary.
- Do not introduce CPAR menus, ABI metadata, new parameters, fallback paths,
  or loader/source changes. This Phase is documentation reconciliation only.

## Verification

1. Audit names, duplicates, bounds, defaults, unknown-key rejection, and
   root-mode exclusivity against `include/boot/parameters.h` and production
   boot parsing/selection code.
2. Audit i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI transport wording
   against the current loader sources and q015/q031/q032 Phase evidence.
3. Confirm every positive and negative example has an implementation or
   retained-test anchor and that planned CPAR behavior is absent.
4. Run DOC-T00 relative-link validation and the DOC-T31 source-anchor audit.
   Do not use aggregate `make check`, `.internal/`, or Noct source changes.

## Completion conditions

- every documented parameter, default, validation rule, and unknown-key
  behavior matches current production code;
- all four x86 handoff/configuration paths and the non-x86 compatibility
  boundary are current and traceable;
- DOC-T00, DOC-T10, and DOC-T31 pass; and
- WS009 marks DOC-31 complete with the exact evidence boundary.

## Reconsideration boundary

Stop and return a discrepancy to WS003/WS013 if code and the already approved
contract disagree. Do not resolve a semantic mismatch by silently editing the
public contract or production source inside this documentation Phase.

## q046 result

- Reconciled the public reference against the common parser, boot-source/root
  consumers, swap activation, init selection, all current x86 loader sources,
  and retained q015/q031/q032 evidence without changing production code or the
  approved parameter contract.
- Corrected the stale pre-q032 statement that a current production loader
  invents the overlay/swap default when configuration is missing. Current
  PC/AT BIOS, PC-98, amd64 BIOS, and amd64 UEFI paths require their documented
  configuration file and have no embedded/fixed-kernel production fallback;
  the separate old-handoff HAL default remains documented as compatibility.
- Distinguished immutable boot-time `swapN=` selection from the implemented
  runtime `swapon`/`swapoff` control path.
- Added direct relative anchors to the public headers, production parser,
  selectors, VFS/root, swap, init, three configured loaders, focused fixtures,
  and q015/q031/q032 completion records. DOC-T10 and DOC-T31 therefore have a
  reviewable source/evidence chain rather than relying on prose alone.

### Verification evidence

- DOC-T00 passed for the active `docs` tree and the WS009/WS003/WS013 plan
  scopes plus the referenced WS016 evidence (314 links). The unfiltered
  whole-`plan` invocation additionally
  descended into an ignored WS008 `temp/` source-copy artifact and reported
  that copy's intentionally absent upstream design document; disposable
  `temp/` content is outside the planning-book link contract.
- BR-T42 passed against the production common parser and init path.
- BR-T44 passed against the production selector, boot-reference, root-mode,
  and private-slot implementation.
- The WS013 configured-loader parser passed ordinary, sanitizer, and available
  static-analyzer gates.
- A source-anchor audit confirmed that every new relative target exists and
  that the documented key/default/configuration names occur in their cited
  production files; `git diff --check` passed.

The retained q015 31-cell run remains the integrated kernel semantic evidence.
q031 supplies the current UEFI configuration/discovery evidence, and q032
supplies the current PC/AT/amd64 BIOS and PC-98 required-configuration and
no-fallback evidence. No semantic discrepancy requiring return to WS003 or
WS013 was found.
