<!-- awesome-plan project=zedbsd record=ws022-p001 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws022/phase001/phase.md`

親: [ws022](https://github.com/awemorris/zedBSD/issues/23)

# WS022 Phase 001: TLS ABI contract and ELF fixtures

Last updated: 2026-09-02

Phase ID: `ws022-p001`

Status: completed (q127)

Parent: [WS022](https://github.com/awemorris/zedBSD/issues/23)

## Objective

Audit the current TCB, rtld, exec, pthread, compiler, and linker behavior; then
freeze the first x86 `PT_TLS` contract and its executable fixtures before
changing loader behavior.

## Work and acceptance

1. Record the amd64 and i386 thread-pointer convention, TCB location, static
   TLS offsets, alignment, and ownership across exec, pthread creation, exit,
   fork, and failed creation.
2. Produce compiler-emitted fixtures for initialized TLS, zero-fill TLS,
   alignment greater than the natural word size, multiple symbols, and
   per-thread mutation. Inspect them with the project `llvm-readelf`.
3. Add malformed fixtures for duplicate `PT_TLS`, `p_filesz > p_memsz`, file
   truncation, non-power-of-two/unsupported alignment, address/size overflow,
   and the chosen implementation size limit.
4. Freeze whether startup shared objects participate in the initial static TLS
   block. If supporting them requires a broader rtld ABI change, extract that
   work as a separate Phase before implementation rather than guessing.
5. Complete when the layout and ownership contract is written here, fixtures
   are repeatable, and no public ABI question remains for p002.

## q127結果

[確定契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws022-elf-tls/phase001-contract-and-fixtures/contract.md)にTCB ABI改訂、両x86 HAL、template所有権、dynamic境界を記録。project clang/ld.lldの正常2・不正18 ELF fixture生成とFS/GSコード確認PASS。実loader/runtime受け入れはp002/p003で実施する。
