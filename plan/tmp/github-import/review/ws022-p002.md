# WS022 Phase 002: executable TLS loading and initial thread

Last updated: 2026-09-02

Phase ID: `ws022-p002`

Status: completed (q128)

Parent: [WS022](../ws.md)

Depends on: `ws022-p001`

## Objective

Make exec consume the frozen `PT_TLS` contract, initialize the new process's
TLS block, and install its thread pointer before the first user instruction.

## Work and acceptance

1. Parse at most one permitted executable `PT_TLS` segment and validate every
   file, memory, alignment, arithmetic, and implementation bound before exec's
   commit point.
2. Allocate an aligned TLS/TCB mapping, copy exactly `p_filesz`, zero the
   remainder through `p_memsz`, populate the frozen metadata, and install the
   thread pointer through the existing architecture/thread boundary.
3. Make every failure unwind the candidate address space and TLS allocation
   while retaining the old executable image and exact errno.
4. Keep no-TLS executables behavior-compatible and retain W^X and ordinary
   segment validation.
5. Pass all valid/malformed fixture tests on amd64 and i386 plus focused exec
   rollback tests and `make -j16`.

詳細設計: [確定TLS契約](../phase001/contract.md)。TCB prefix、i386 GS descriptor、exec/spawnのTP commit、template VM所有権をこの順で実装する。

結果: [p002検証記録](results.md)。amd64/i386の不正9例exec rollbackと初回TLSをQEMUで確認。pthread、zero-only等の拡張campaignはp003。
