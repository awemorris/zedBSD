<!-- awesome-plan project=zedbsd record=ws171-p001 -->
# ws171-p001: hal.h の関数の一覧、今の comment の状態、各 arch の実装、雛形

Status: cleared 候補（2026-10-09 P1。文書と一覧だけ、hal.h は変えていない。Q1 の判定待ち）
Parent: [WS171](../ws.md)
Queue: Q1 の dispatch（P1、2026-10-09、ベータ3 の合間の仕事。ユーザーの一覧に WS171）

## 方法

- `plan/ws171/phase001/inventory.py` が `include/hal/hal.h` を読んで、行の頭の `hal_name(` を関数の宣言とみなす。その上の comment を取り、4 つに分ける:
  - none: comment が無い。
  - placeholder: 「XXX: Add explanation.」。
  - short: 何をするかだけ。
  - contract: 25 語以上で、返り値・HAL_ERR・割り込み・lock・context・caller のどれかを述べる。
- 定義の場所は、`src/hal/<arch>/`（amd64・i386・arm64・sparcv9・m68k）、`src/hal/x86/`、`src/hal/*.c`（共有）の C・asm の file の中で、行の頭に `hal_name(` がある物。
- 出力: [inventory.md](inventory.md)（130 行の表）。

## 結果（2026-10-09 の main、hal.h 1560 行）

- 関数は 130。comment の状態は contract 20、short 62、none 42、placeholder 6。WS171 の目標の水準（HAL v2）に届いているのは 20 だけ。
- 領域（名前の 2 語目）ごとの数: task 28、irq 19、cpu 16（mask 5 を含む）、space 15、io 9、mmio 8、pmem 7、文字列と console（strlen・mem*・put*・printf）8、assert・fatal・panic・halt・reset・poweroff 6、cache（dcache 3・icache 1）4、barrier（mb・rmb・wmb・sync）4、rtc 2、get 2、entropy 1。
- none の 42 の多くは、宣言が並ぶ群の 2 つ目以降（群の頭に 1 つの comment がある形）と、文字列の関数（hal_strlen ほか）。
- 「macro or none」の 6（`hal_cpu_mask_*` の 5 と `hal_panic`）は、hal.h の中の inline か、HAL の外（kernel）の定義。
- 一覧の定義の欄の限り: arm64・sparcv9 の asm の定義や別名の関数は、正規表現に掛からないことがある（例: `hal_strlen` は amd64・i386・m68k にしか出ない）。p002 で領域ごとに、各 arch の実装を読んで確かめる。

## 雛形（HAL v2 の comment から。例: `hal_cpu_notify`）

```
/*
 * <何をするか、1 文。呼ばれた後に kernel の何が起きるか>
 *
 * <呼んでよい文脈: 割り込みの可否、lock を持ったままでよいか、どの CPU から、boot のどの段から>
 * <他の HAL の操作との関係: 何と対になるか、何の後でなければならないか、何が止めないか>
 *
 * Returns HAL_OK, HAL_ERR_<X> for <条件>, ... or HAL_ERR_UNSUPPORTED on a
 * machine without <機能>.  <失敗の時に何も変わらないか>
 */
```

- 返り値の無い関数は「Returns」の段を「On return <状態>」にする。_Noreturn は戻らない条件を書く。
- architecture が対応しない時の扱い（UNSUPPORTED を返す、何もしない、HAL_FATAL）を必ず書く。
- 実装と合わない所は契約を決めず、p003 で bug かユーザーの判断に出す（WS171 の制約）。

## p002 の分け方（案）

差分の案は、領域ごとに 6 つに分けて review を受ける:
1. 文字列と console・assert・fatal（11）
2. CPU・mask・idle・panic・halt・reset・poweroff（20）
3. IRQ（19）
4. task と context（28）
5. 空間・pmem・mmio・io・barrier・cache（47）
6. rtc・entropy・get（5）

最初は 4 と 3（短い comment が多く、kernel の使い方が複雑）から始める案。

## 確認

- `python3 plan/ws171/phase001/inventory.py > plan/ws171/phase001/inventory.md`（成功、130 行）。hal.h は変えていない。
