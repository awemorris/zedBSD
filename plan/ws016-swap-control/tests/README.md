# WS016 shared test cases

Parent: [WS016](../ws.md)

No test implementation exists yet. Future Queue execution places reusable
fixtures in this directory and disposable QEMU evidence under `../temp/`.

| Case ID | Owning Phase | Required observation |
| --- | --- | --- |
| SWAP-T001 | p001 | Source/local token encoding preserves IDs, boundaries, sentinel, and exact I/O routing |
| SWAP-T002 | p001 | Sparse boot IDs and runtime lowest-free IDs allocate in deterministic numeric order |
| SWAP-T003 | p001 | Duplicate inode/disk aliases, root overlap, malformed headers, and unsupported files unwind every claim |
| SWAP-T004 | p001 | Write/truncate/rename/unlink/loop and separately mounted writable aliases return `EBUSY` while active |
| SWAP-T005 | p001 | Concurrent fault/allocation/drain reaches zero target ownership before removal; injected failure retains a usable source |
| SWAP-T006 | p001 | Add/remove updates commit capacity atomically and rejects a limit below reserved commitment |
| SWAP-T007 | p002 | Versioned 32/64 UAPI layout, bounds, pointers, strings, reserved fields, and copyout are exact |
| SWAP-T008 | p002 | Root control, non-root `EPERM`, enumeration, canonical matching, and interrupted failure atomicity pass |
| SWAP-T009 | p003 | `swapon` parsing, `--`, operand order, diagnostics, continuation, and exit status pass |
| SWAP-T010 | p003 | `swapoff` parsing, `--`, operand order, diagnostics, continuation, and exit status pass |
| SWAP-T011 | p004 | amd64 QEMU add/use/drain/remove/reuse preserves page patterns and reports coherent source/aggregate stats |
| SWAP-T012 | p004 | Negative runtime cases preserve the old pool and representative q015 file/raw/mixed boot cases remain passing |

The supported build gate is `make -j16`; the aggregate `make check` target and
repository `.internal/` tests are not part of this WS.
