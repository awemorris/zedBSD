<!-- awesome-plan project=zedbsd record=ws055p001 -->

# ws055-p001: zedbsd の clang の driver が link に `--undefined-version` を渡す

Phase ID: `ws055-p001`
Parent: [WS055](../ws.md)
Status: planned
Queue: none

## 調べたこと（2026-09-25、q417 の間の計画）

- link の引数は `toolchain/llvm/patches/0001-add-zedbsd-x86-target.patch` の `clang/lib/Driver/ToolChains/ZedBSD.cpp`、`tools::zedbsd::Linker::ConstructJob()` で組む。
  `--allow-shlib-undefined` の隣に `--undefined-version` を足す。利用者の `-Wl,...` は `AddLinkerInputs()` で後ろに並ぶので、`-Wl,--no-undefined-version` で戻せる。
- patch を変えると `toolchain/llvm/version.mk` の `ZEDBSD_LLVM_PATCH_LEVEL`（今 `zedbsd6`）を上げる。source の identity（patch の sha256）が変わり、`build/llvm-source` を tarball から展開し直して patch を当て、
  host の LLVM（`make toolchain`、`ZEDBSD_LLVM_COMPILE_JOBS=64 ZEDBSD_LLVM_LINK_JOBS=8` で数十分）を作り直す。`build/llvm` は識別が違えば置き換えられる（`llvm.mk`）。
- guest の clang（`userland/packages/lang/clang`）と libc++（`devel/libcxx`）は同じ source の stamp（`.zedbsd-source-VERSION-PATCHLEVEL`）を見るので、guest の image の build で作り直される。
- rev-0 の binary cache（`ZEDBSD_LLVM_CACHE_ASSET`・`ZEDBSD_LLVM_CACHE_SHA256`）は新しい patch に合わない。`ZEDBSD_LLVM_CACHE_SHA256` を `PENDING` に戻すと `make toolchain-cache` が明示的に断る。
  新しい資産の公開（GitHub の release）はユーザーの判断・作業（**確認事項**）。

## 手順

1. patch に `--undefined-version` を足し（comment: version script の未定義の symbol を GNU ld と同じく通す）、`ZEDBSD_LLVM_PATCH_LEVEL := zedbsd7`、`ZEDBSD_LLVM_CACHE_SHA256 := PENDING`。
2. `make ZEDBSD_LLVM_COMPILE_JOBS=64 ZEDBSD_LLVM_LINK_JOBS=8 toolchain`。
3. host の試験: 未定義の symbol を持つ version script で `clang --target=x86_64-unknown-zedbsd -shared` が通る、`-Wl,--no-undefined-version` で error になる。i386 の target も同じ。
4. guest の image を作り直し（clang の package が作り直される）、guest で zlib の configure が共有 library を作り、`make check`・install の一覧が host と一致する（q412 の `--undefined-version` 付きと同じ）。
5. 回帰: 4 platform の build（warning 0）と boot、guest の make の差分試験（clang を使う case があれば）。

## 受け入れ

- 3〜5 が通る。`ZEDBSD_LLVM_PATCH_LEVEL` と cache の扱いが記録されている。
