# WS022 Phase 003: pthread TLS lifetime and runtime acceptance

Last updated: 2026-09-02

Phase ID: `ws022-p003`

Status: completed (q128)

Parent: [WS022](../ws.md)

Depends on: `ws022-p002`

## Objective

Complete the static TLS lifetime for threads and prove compiler-emitted
`_Thread_local` state on supported x86 runtime paths.

## Work and acceptance

1. Replace temporary static-libc process-global fallbacks with real
   `_Thread_local` storage where the API contract requires per-thread state.
2. Make pthread creation clone the executable TLS initialization image, zero
   the remainder, install a distinct thread pointer before user code, and
   release the mapping exactly once on every exit/failure path.
3. Exercise initialized, zero-filled, aligned, address-taken, and independently
   mutated TLS variables across the initial thread and multiple concurrent
   pthreads, including repeated create/join and injected allocation failure.
4. Verify fork/exec behavior against p001's frozen ownership contract and
   retain existing dynamic executable/rtld regressions.
5. Run one final amd64 and i386 PC/AT QEMU campaign. Record exact ELF and
   runtime evidence before completing WS022; no physical checkpoint is needed.

## q128詳細設計

- static-tls.cは現TCBのimmutable template metadataからRW blockを確保。TPはpage aligned、template bytesをTP-distanceへcopyし、残りはanon zero。prefixのmapping ownerをfreeで使う。
- no-TLSのlazy attachとfailed SET_TLSを保持。thread_create失敗/join/detached reaperは既存ownerへ統合し、解放経路を増やさない。
- 両static user linkerに.tdata/.tbssとPT_TLSを追加し、通常no-TLSの挙動を維持。
- hostはproduction allocatorをmmap/syscall fixtureで実行し、clone/zero/alignment/100回回収/failed alloc/failed attachを通常とASan/UBSanで検証。
- guestは実pthread同時独立更新、100回create/join、fork、signal、初回TLS、旧imageへのexec rollback、dynamic dyntestを確認。通常buildを復元し、全証拠をまとめる。

実行中の追加修正: signal/TLS検証でraise()がkill(getpid())を使い別threadへ届くことを再現。pthread_kill(pthread_self())へ変更し、既存POSIXエラー形式を維持する。同じthread-local signal fixtureで検証する。

動的回帰で既存libc.soのTLSサイズ0x1a48がrtldの1-page上限を超え起動拒否されることを確認。q126の変更前成果物も同じ0x1a48だった。allocator/unmapperは既にpage roundingを支持するため、登録上限を共有TLS上限1MiBに合わせ、alignment上限4096は維持してdyntestで検証する。

plugin TLS回帰でpure-BSS PT_LOADを一度全域anonymous mapした後、同じ領域へ追加zero-fill mapしてEEXISTとなる既存バグを再現。追加mapはfile-backed portionがある場合だけに限定する。実tlstest.soのreload/close campaignで回収も確認する。

結果: [q128全受け入れと実装記録](results.md)。
