<!-- awesome-plan project=zedbsd record=ws022-p002 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws022/phase002/phase.md`

親: [ws022](https://github.com/awemorris/zedBSD/issues/23)

# WS022 Phase 002: executable TLS loading and initial thread

Last updated: 2026-09-02

Phase ID: `ws022-p002`

Status: completed (q128)

Parent: [WS022](https://github.com/awemorris/zedBSD/issues/23)

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

詳細設計: [確定TLS契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws022-elf-tls/phase001-contract-and-fixtures/contract.md)。TCB prefix、i386 GS descriptor、exec/spawnのTP commit、template VM所有権をこの順で実装する。

結果: [p002検証記録](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws022-elf-tls/phase002-exec-loader/results.md)。amd64/i386の不正9例exec rollbackと初回TLSをQEMUで確認。pthread、zero-only等の拡張campaignはp003。
