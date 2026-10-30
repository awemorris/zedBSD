<!-- awesome-plan project=zedbsd record=ws331-p011 -->

# ws331-p011: arch の spinlock の取得と待ちの合図を HAL の atomic.h の macro の契約へ

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1724（P2）

## 目的と範囲

2026-10-30 ユーザーから、hal_atomic_uint_try_acquire（と hal_atomic_relax）を include/hal/arch/atomic/<arch>.h のような file に分け、hal/atomic.h から include すれば済む、という趣旨の提案を受け、実装の指示を受けた。

1. 13 個の arch の header（include/hal/arch/*.h）の hal_atomic_uint_try_acquire・hal_atomic_relax を、include/hal/arch/atomic/<arch>.h に移す。形は「macro だけ」の契約に合わせ、asm を含む statement expression の macro にする。外の関数を呼ぶ arch（m68k など）は、macro の中の block で宣言して呼ぶ（直列化の enter・leave と同じ）。
2. hal/atomic.h は、build の -DHAL_ARCH_* で arch の file を選んで include する（hal/arch.h は include しない）。comment の契約に、この 2 つも macro として入る事を書く。arch の header（hal/arch/<arch>.h）からは消す。HAL の中で使う所（あれば）は hal/atomic.h 経由に直す。
3. kernel の atomic.h: p010 の案 C の out-of-line の関数（src/kern/lock.c の atomic_raw_try_acquire_zero・atomic_spin_hint）をやめ、HAL の macro で inline 関数に戻し、使った後に undef する。p010 で host 試験に足した stub が要らなくなれば外す。
4. 確かめ: 20 port の build（warning 0）、driver の -E に file scope の hal_ が無い事、LTO を使わない port（m68k-virt・ppc32・ppc64・sparcv9・x68k）で spinlock の取得の call が無い事（nm・逆 assemble）、QEMU pcat・amd64・rpi4・m68k の起動、p010 で流した host 試験。
