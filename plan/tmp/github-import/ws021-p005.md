<!-- awesome-plan project=zedbsd record=ws021-p005 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws021/phase005/phase.md`

親: [ws021](https://github.com/awemorris/zedBSD/issues/22)

# WS021 Phase 005: BIOS/UEFI loader and disk-image toolchain closure

Last updated: 2026-09-02

WSID: `ws021`

Phase ID: `p005`

Combined ID: `ws021-p005`

Status: complete (`q064`)

Parent: [WS021](https://github.com/awemorris/zedBSD/issues/22)

Depends on: `ws021-p004`

## Objective

Remove the remaining host target GNU-binutils and MinGW requirements from x86
bootloader and disk-image production so a configured x86 image needs only the
host bootstrap tools plus `build/llvm/`.

## Work packages

1. Build PC/AT and PC-98 BIOS stage 1/stage 2, BOOTZBSD and test payloads with
   the installed Clang integrated assembler, LLD and LLVM binary tools in
   freestanding mode. Preserve exact sector, entry, relocation and size
   contracts.
2. Build amd64 BIOS loader artifacts through the same i386 freestanding path.
3. Replace `x86_64-w64-mingw32-*` with the installed Clang/LLD PE/COFF path for
   `BOOTX64.EFI`. Use a fixed freestanding Windows/UEFI code-generation triple
   and EFI subsystem/entry contract; do not import MinGW headers or runtime.
4. Route all ELF/PE inspection, archive, symbol, strip and object-copy steps to
   LLVM tools where they operate on target artifacts.
5. Build all amd64 image Variants plus PC/AT i386 and PC-98 images from clean
   output directories. Validate bytes/layout/configuration without booting.
6. Run a dependency and command-log audit proving no target `gcc`, GNU `ld`,
   GNU binutils, or MinGW executable is invoked. Host compiler/linker use is
   permitted only inside the recorded host Noct/LLVM bootstrap.

## Verification

- BIOS size/signature/entry/loader-chain and PC-98 native IPL checks pass on
  LLVM-built artifacts.
- `BOOTX64.EFI` is valid PE32+ EFI application with the expected machine,
  subsystem, entry and imports, and is byte-identical to the installed ESP
  payload checked by current image preflight.
- Hybrid, UEFI-only, BIOS-only, PC/AT i386 and PC-98 image host-layout tests
  pass from clean target outputs.
- A hostile PATH with target GNU/MinGW tools unavailable still builds every
  image; no QEMU runtime is executed.

## Completion conditions

Every maintained x86 bootloader and disk image is produced through
`build/llvm/`, all static format/layout gates pass, and p006 is the only
remaining runtime acceptance boundary.
