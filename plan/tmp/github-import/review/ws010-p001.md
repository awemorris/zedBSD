# ws010-p001: Noct host toolchain

WSID: `ws010`

Phase ID: `p001`

Status: complete

Parent WS: [WS010](../ws.md)

## Objective

Make `make toolchain` obtain upstream Noct in `build/NoctLang` and build the
static host CLI used for every subsequent script migration.

## Work packages

- [x] Add common Noct repository/source/binary variables.
- [x] Clone only when the checkout is absent and preserve an existing checkout.
- [x] Configure and build the upstream `static` CMake preset.
- [x] Make every supported platform `toolchain` target depend on host Noct.
- [x] Run a Noct smoke script using File and Process/System APIs.

## Completion conditions

- Removing `build/NoctLang` followed by `make toolchain` recreates the checkout.
- `build/NoctLang/build-static/noct --version` succeeds.
- A second `make toolchain` succeeds without recloning or corrupting the tree.
- The host Noct smoke test passes.

## Verification record

`make toolchain` and an immediate second invocation succeeded at pinned commit
`7d856856e16eb2d889ba49f557f2fda4dcaeea7e`; `noct --version` reported
`Noct 1.0-current`, and the newly authored WS010 smoke test passed.
