<!-- awesome-plan project=zedbsd record=ws034p055 -->

# ws034-p055: kernel の乱数

Phase ID: `ws034-p055`
Parent: [WS034](../ws.md)
Status: **cleared**（q369-i01、2026-09-24）
Phase disposition: normal
Queue: q369（q369-i01）
実行: メインセッション
設計: [design.md](design.md)

## 経緯

p054 で、`/dev/random`・`/dev/urandom` が無く、`getentropy` が RDRAND の無い機械（i386・pc98・rpi4 を含む）で ENOSYS、
libc の `arc4random` が黙って userland の予備に落ちていることを見つけた。

## 変更

- 新規 `src/kern/random-crypto.c`・`include/kern/random-crypto.h`: ChaCha20 の block 関数（RFC 8439）と BLAKE2s-256（RFC 7693）。
- 新規 `src/kern/random.c`・`include/kern/random.h`: BLAKE2s の pool に `hal_entropy_fill`・RTC・起動時の counter と、
  **各 CPU の timer の tick ごとの `hal_rtc_read_counter()`** を混ぜる。reseed は `鍵 = BLAKE2s(鍵 ‖ pool)`、seeded になったときと
  その後 5 分ごと。出力は ChaCha20 の fast key erasure（256 byte ずつ lock を取り直す）。seeded は HAL の源か tick 256 回。
  `LOCK_RANK_RANDOM`（147）。
- `getentropy(2)` は CSPRNG から（seeded まで待つ）。ENOSYS をやめた。
- `/dev/random`（seeded まで待つ）・`/dev/urandom`（待たない）。書き込みは pool に混ぜる（評価しない）。`CDEV_READ_NEVER_WAITS`
  で長い読みを満たす。
- libc の `arc4random_buf` は `getentropy`（256 byte ずつ）、失敗したときだけ従来の予備。
- 起動で `kern_random_init()`（`sched_init` の後）、tick で `kern_random_tick()`。4 platform の source 一覧に足した。HAL は不変。

## 検証

| 試験 | 結果 |
| --- | --- |
| host `plan/ws034/tests/random-crypto-test.c`（通常＋ASan/UBSan） | PASS: ChaCha20 の RFC 8439 2.3.2、BLAKE2s の "abc"・空・64 byte・65 byte（Python の `hashlib.blake2s` と一致） |
| guest `plan/ws034/tests/random-test.c`（amd64、3 回の起動、[random-guest.txt](evidence/random-guest.txt)） | 各 8/8: `getentropy` の成功と毎回違う値、257 byte の拒否、`/dev/urandom` の 1 MiB を 1 回の read で・bit の偏りなし、`/dev/random` の読み書き、`arc4random`。標本は起動ごとに違う。**`-cpu host,-rdrand`（RDRAND 無し）でも 8/8** |
| rpi4（HAL の源が無い）QEMU | `/dev/random` から 1 MiB、`/dev/urandom` から 128 KiB を読めた（tick で seeded） |
| CI の kernel（amd64・pcat・pc98・rpi4）・sysroot | warning 0 |

## 残り

- 装置の割り込みの時刻を混ぜる（割り込みの登録を kernel で包む）。
- HAL の新しい源（arm64 の RNDR、BCM2711 の RNG200、pc98・i386 の機種固有）は HAL の差分が要る。
- pcat・pc98 の実行時の確認はしていない（build のみ。仕組みは rpi4 と同じ tick の経路）。
