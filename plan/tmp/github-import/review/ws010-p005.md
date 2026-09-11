# WS010 Phase 005: userland source-distribution lifecycle

Last updated: 2026-09-02

Phase ID: `ws010-p005`

Combined ID: `ws010-p005`

Status: complete (`q063`, 2026-09-02)

Parent: [WS010](../ws.md)

Tests: [WS010 tests](../tests/README.md)

## Objective

Give every repository-owned `userland` item the same directly callable source
lifecycle:

```text
make download -> make patch -> make build -> make install
```

An in-tree item may implement `download` and `patch` as successful no-ops.
External inputs use an item-owned, immutable, verified acquisition rule.
Top-level `make download` acquires every currently declared external userland
input, including optional firmware and the Noct release source, without
building an image. A freshly cloned Git development tree is thereby distinct
from a distributable source tree whose ignored external blobs have already
been materialized in place and can be archived with their multiple licenses.

## Fixed contract

- Every tracked `userland/**/Makefile`, excluding Makefiles inside downloaded
  upstream trees, accepts `download`, `patch`, `build`, and `install`.
- `download` and `patch` work before `config.mk` exists. `build` and `install`
  retain the configured-target requirement where compilation needs it.
- `patch` depends on `download`; `build` depends on `patch`; `install` depends
  on `build`. Existing `all` remains a compatibility alias for `build`.
- In-tree source packages use no-op acquisition/patch stages and do not create
  placeholder blobs.
- External data is downloaded to a sibling temporary path, checked for exact
  type, size, SHA-256 and expected archive shape where applicable, then
  published atomically. Existing corruption is an error, not an implicit
  replacement.
- Top-level `make download` includes unselected optional external inputs. It
  performs no target compilation and does not silently occur during an
  ordinary `make -j16`.
- Ignored downloaded bytes remain beneath the source working tree so a source
  distribution can be made from the post-download tree. Git does not track
  those bytes.
- Item-owned tracked patches are explicit inputs. Patch application is strict,
  repeatable, and happens in a temporary extraction before atomic publication;
  an upstream tarball is never modified in place.
- `.internal/` is not an input.

## Initial external inputs

- Noct `v2.0.1` source archive, shared by host-toolchain and target-userland
  integration through WS008 p010/p009.
- RTL8822B firmware, its license, and WHENCE through the existing immutable
  cache rule.
- Intel AX211 firmware, PNVM, license, and WHENCE through the existing
  immutable cache rule.
- Other packages receive the lifecycle interface even when their first
  `download`/`patch` implementation is a no-op, as explicitly permitted for
  this initial generalization.

## Work packages

1. Add one common standalone lifecycle include and connect the ordinary base
   package helper plus custom X11, firmware, compiler, data-license, and
   optional-package Makefiles.
2. Add top-level `download`, config-optional operation, and help text. Aggregate
   declared external inputs from package metadata rather than walking newly
   downloaded upstream Makefiles.
3. Connect both firmware packages to their existing verified cache targets.
4. Add Noct's versioned archive and strict patch-extraction targets as the
   first source package using all four lifecycle stages.
5. Audit every tracked userland Makefile and document the source-distribution
   versus Git-tree boundary.

## Verification

- A maintained audit enumerates tracked `userland/**/Makefile` files and proves
  each exposes all four targets without descending into downloaded upstream
  trees.
- Representative base, X11, firmware, compiler, license/data, and package
  entries accept their lifecycle from their own directory.
- A missing `config.mk` still permits representative `download` and `patch`.
- Top-level `make download` validates the existing firmware caches and the
  exact Noct archive; a second invocation succeeds from local bytes.
- Focused acquisition fixtures cover wrong size/digest, unsafe type, partial
  publication, and strict patch application without using production URLs.
- `make -j16`, applicable WS008 host/target gates, and `git diff --check` pass.
  Do not run aggregate `make check`.

## Completion conditions

- Every tracked userland item exposes the four-stage lifecycle.
- Top-level `make download` materializes and verifies all external inputs
  declared by this Phase while ordinary builds retain their documented
  network boundary.
- Git tracks acquisition identities, manifests, licenses, and patches but no
  downloaded binary/source archive.
- The post-download working tree is sufficient to preserve those external
  inputs in a separately packed, multi-license source distribution.

## Q063 result

Complete. A common lifecycle include plus the ordinary package helper now give
all 177 maintained `userland/**/Makefile` entries directly callable
`download`, `patch`, `build`, and `install` targets. In-tree entries use
successful no-op preparation; external entries retain their own immutable,
verified acquisition. The top-level configuration-optional `make download`
materializes Noct `v2.0.1`, RTL8822B firmware, and Intel AX211 firmware without
building an image, and a second invocation revalidates local bytes.

Noct exercises all four stages with a size/hash/shape-checked archive,
temporary extraction, zero-fuzz tracked patch, source manifest, and atomic
publication. Standalone Noct build/install produced byte-identical installed
artifacts. Both existing firmware package suites, config-free preparation,
host-artifact recovery, lifecycle inventory, ordinary `make -j16`, and
`git diff --check` passed. Seven maintained network-free acquisition cases
also reject wrong size/digest, unsafe path/type, interrupted transfer and
fuzz-only patching before publishing invalid archive/source state. Downloaded
source and firmware bytes remain ignored; Git contains only identities,
acquisition logic, patches, manifests, and license metadata. `.internal/` was
not used.
