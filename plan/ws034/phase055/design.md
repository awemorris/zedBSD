<!-- awesome-plan project=zedbsd record=ws034-p055-design -->

# kernel の乱数（ws034-p055）の設計

2026-09-24。p054 の調査で見つけた。

## 今の状態

- kernel に乱数の器が無い。`getentropy(2)` は `hal_entropy_fill()` を毎回呼ぶだけで、HAL の実装は amd64 の RDRAND だけ
  （i386・pc98・arm64 は常に false）。RDRAND の無い CPU、i386・pc98・rpi4 では **`getentropy` が ENOSYS** になる。
- `/dev/random`・`/dev/urandom` が**存在しない**。libc の `arc4random_buf()` は `/dev/urandom` を開き、開けないと userland の
  予備の生成器に黙って落ちる（種は時刻など）。暗号の鍵・ssh の host key・TCP の初期値に使う値としては弱い。

## 決定

### D1. kernel の CSPRNG（`src/kern/random.c`、新規）

- 出力: **ChaCha20**（鍵 32 byte）。要求ごとに block を作り、渡す前に次の 32 byte で鍵を置き換える（fast key erasure）。
  1 回の lock の下で作るのは 256 byte までにし、長い要求は分けて作る（途中で他の thread が割り込める）。
- 集める側: **BLAKE2s** の状態 1 つに入力を足していく。**reseed** で `鍵 = BLAKE2s(旧の鍵 ‖ 集めた pool)` にする。
- 入力（HAL の変更なし）:
  - 起動時と reseed ごとに `hal_entropy_fill()`（RDRAND 等。無ければ false で、足さない）。
  - timer の tick ごと（各 CPU）に `hal_rtc_read_counter()` の値と CPU 番号・tick 数（`kernel_timer_handler` から）。
    **実装の決定（2026-09-24）**: 装置の割り込みは kernel を通らず HAL の handler へ直接届くので、登録を包む変更を避け、
    tick の時刻の揺らぎを使った。装置の割り込みの時刻は後の課題。
  - 起動時の RTC の時刻、boot handoff の内容（MAC address 等の機械固有の値。弱いが種の違いを作る）。
- **seeded** の判定: HAL の源から 32 byte を得たか、tick の時刻を 256 回以上集めたとき（1 kHz の tick で起動後約 0.25 秒）。それまでの `/dev/random` と
  `getrandom(0)` は待つ（seeded の waitq）。`/dev/urandom`・`getrandom(GRND_INSECURE)` は待たない。
  QEMU や RDRAND の無い機械でも起動の途中で割り込みは十分に来るので、長くは待たない見込み。
- reseed は seeded になったときと、その後 5 分ごと（出力を作るときに時刻を見る）。

### D2. 公開するもの

- `getentropy(2)`: CSPRNG から。seeded まで待つ（今の ENOSYS をやめる）。
- `/dev/random`（seeded まで待つ）、`/dev/urandom`（待たない）: `src/drivers/generic/memory-device.c` と同じ所に。
  書き込みは pool に混ぜる（評価は増やさない）。読みは `CDEV_READ_NEVER_WAITS`（p054）で長い要求を一度に満たす。
- libc: `arc4random_buf()` は `getentropy`（256 byte ずつ）を使い、失敗したときだけ今の予備へ。

### D3. 範囲外

- entropy の見積もりの細かい会計（Linux の旧方式）はしない。seeded かどうかだけ。
- HAL への新しい源（arm64 の RNDR、BCM2711 の RNG200）は HAL の差分が要るので別 Phase。

## 試験

- host: ChaCha20 の RFC 8439 の試験ベクトル、BLAKE2s の RFC 7693 のベクトル、鍵の置き換え（同じ出力が二度出ない）。
- guest: `/dev/urandom` から 1 MiB（`dd bs=1M count=1`）、`getentropy` が amd64 で成功、2 回の起動で出力が違う、
  RDRAND を隠した CPU（`-cpu host,-rdrand`）でも `getentropy` が成功する。pcat（i386）でも `getentropy` が成功する。
