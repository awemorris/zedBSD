<!-- awesome-plan project=zedbsd record=ws333-p003 -->

# ws333-p003: kernel の口の header（include/kern/thermal.h・include/kern/cpuperf.h、kern_cpu_count）

Status: cleared
Parent: [WS333](../ws.md)
Queue: q1754-i01（P7）

## 目的

design.md §9 の p003: 熱の core（p005）と cpuperf の枠組み（p006）と x86 の driver（p011）・WS302 が使う kernel の口を header にし、design-reviewer で 1 回 review して反映する。UAPI（include/uapi/thermal.h）は p004（ユーザーの review）で、ここでは作らない。

## 判断（Q1）

- 2026-10-31 ユーザーから、不在の間は実装できる WS を実装だけでも進め、試験できるなら試験もする、という趣旨の指示を受けた。Q333-1〜7 は答え待ちだが、Q1 は推しで進める判断をした（CPU の道は問いを待たない、design.md §9）。
- review の M10 を受け、Q1 は header（と kern_cpu_count）を p005・p006 より先に main に入れると決めた。そのため header は案（`proposed/`）の後に `include/` に置き、その commit を先に Q1 に渡した。

## 成果

- 案: [proposed/include/kern/cpuperf.h](../proposed/include/kern/cpuperf.h)・[proposed/include/kern/thermal.h](../proposed/include/kern/thermal.h)（review の反映の後の版。`include/kern/` の物と同じ）、[proposed/kern-cpu-count.diff](../proposed/kern-cpu-count.diff)（`git diff` の出力）。
- 置いた物: `include/kern/cpuperf.h`・`include/kern/thermal.h`（宣言だけ。定義は p005・p006）、`include/kern/types.h` に `KERN_CPU_MAX`（512）と `KERN_CPU_MASK_WORDS`、`include/kern/sched_private.h` に `KERN_CPU_MAX == HAL_CPU_MAX` の `_Static_assert`、`include/kern/sched.h`・`src/kern/sched.c` に `kern_cpu_count()`（`hal_cpu_count()` を返す）。hal.h は変えていない。
- 触った file の開発の管理情報の comment を消した（`sched.h` の 2 か所、`sched.c` の 9 か所。comment だけで log の文字列は変えていない）。
- design.md の直し: §9 の p011 の依存に p005（RAPL の meter の登録の所だけ、review M8、Q1 の指示）、§4.2 の `kern_cpu_count` の置き場所（sched.h、上限は types.h）。

## design-reviewer の指摘と反映（1 回、2026-10-31、blocker 1・major 11・minor 15）

| 指摘 | 扱い |
| --- | --- |
| B1 HWP の能力は enable の後・その CPU でしか読めず、thread で作った request の数では「その時の guaranteed」にならない | 反映: 箱には方針（intent）を置き、notify の中で start（初回）→ sample → 計算 → apply。provider に `start` を足した。describe の RANGE の anchor は enable の前は UNKNOWN でよい |
| M1 idle_package の文脈の食い違い | 反映: notify の文脈の op に入れ、呼ぶ CPU が package に属する時はその場で（他の CPU を起こさない） |
| M2 feedback の戻り 1 が EDOM とぶつかる、並行 | 反映: `feedback(cpu, bool *changed)`、feedback・feedback_read は mutex の下で直列 |
| M3 package の MSR を thread で読む | 反映: feedback は notify の文脈で package の CPU の上。meter は「package が 1 つの機械だけで package の meter を登録する」と契約に書いた |
| M4 FREQUENCY の測りの道が無い | 反映: provider に `measure(cpu, begin, &mhz)`、framework が対象の CPU に束ねた thread で domain を highest にして 50 ms 回す。FREQUENCY の値は framework が測った MHz から出す |
| M5 公開の関数の文脈・lock・errno | 反映: 関数ごとに comment |
| M6 generation が topology・domain で変わらない | 反映: 契約に足した（実装は p006） |
| M7 thermal → cpuperf の直の呼び | 反映: 受け手の ops に `poll` を足し、`kern_cpuperf_poll` を公開しない |
| M8 p011 は p005 に依る | 反映: design.md §9（Q1 の指示） |
| M9 meter の読みが 60 秒古い | 反映: poll の thread が毎秒読む、count_bits ≤ 32、単位は 32 bit の分数 |
| M10 header の持ち主・init の順・weak | 反映: header を先に main（Q1）、`kern_thermal_init` を宣言し「init の前の登録は保たれる」、weak は p006 の .c の中だけ |
| M11 CPU の最大の数の kern の名前 | 反映: `KERN_CPU_MAX`・`KERN_CPU_MASK_WORDS` と `_Static_assert` |
| m1 firmware_id | 反映: levels（describe）にも置き、framework が topology に写す |
| m2 不明の値 | 反映: 欄ごとに comment。hybrid でない CPU は PERFORMANCE、UNKNOWN は「分からない」 |
| m3 既定の topology | 反映 |
| m4 STEPS の意味 | 反映: anchor と bound は段の値、framework が段に丸める、RANGE は段の欄を使わない |
| m5 段が 16 を超える | 反映: provider が両端を残して間引く |
| m6 `_PSD` | 反映: PACKAGE に倒す、share が違う CPU は同じ domain にしない |
| m7 apply の失敗の後 | 反映: errno は待つ側に、次の post まで再び試さない |
| m8 notify の op に cpu、速い道 | 反映: op は cpu を受ける、notify は何も無ければ lock を取らずに戻る |
| m9 delivered の区間 | 反映: READ_SAMPLE は 100 ms 空けた 2 回の sample |
| m10 型 | 反映: flags を uint32_t |
| m11 header の冒頭の文 | 反映 |
| m12 値の出典 | 反映: x86 の値の記述を消した（driver の Phase で出典つきで書く） |
| m13 開発の管理情報 | 反映: 「UAPI に移す」の文を消した |
| m14 UAPI に出る値の名前 | 一部: macro は `KERN_THERMAL_*` に揃えた（fan も）。UAPI の名前と値は p004 で決め、kernel の値はそれに合わせる。危険: p004 のユーザーの review で strategy・事象の組が変わると p005・p006 の code が追従する |
| m15 受け手の契約の穴 | 反映: firmware_name・is_custom・*flags・limit の errno・name の写し・ENOSPC・文脈 |

## 確かめた事

- `plan/ws333/tests/build-vmunix-all.sh build/p003`（全 20 port の vmunix、各 make は 30 分の上限）: 20 port 全部 rc=0、warning 0（amd64・intelmac・pcat・pc98・rpi4・rpi5・odroid-c5・visionfive2・zybo・armv7-virt・riscv64-virt・riscv32-virt・m68k-virt・ppc32-mac99・ppc64-pseries・mips32-malta・mips64-malta・mips32-rg350・sun4u・x68k）。arm64・armv7・i386・riscv64 の sysroot は main の checkout の物を `build/<arch>/sysroot` に symlink で読むだけにした（`make -n` で sysroot・toolchain を作り直さない事を先に確かめた）。
- 2 つの header を host の clang で `-m64`・`-m32`、`-std=c11 -Wall -Wextra -Werror -fsyntax-only` で通した。
- 未実施: header の型の中身を使う code は p005・p006 まで無い（宣言だけなので link の確かめは p006）。

## 持ち越し

- p004: UAPI の名前と値（strategy・事象・CPU の flag）は kernel の header の値を保つか、変えるなら p005・p006 の code を追従させる。
- p005: `kern_thermal_init`・受け手の `poll` を毎秒呼ぶ・meter を毎秒読む。cpuperf.c の weak（`kern_thermal_receiver_register`・`kern_thermal_event`）は p005 が全 port に入った後に外せる。
- p011: provider の `start`・`measure`・`feedback` の文脈（notify の中、割り込みを止めて短く）。段の間引き。firmware_id は describe で。
