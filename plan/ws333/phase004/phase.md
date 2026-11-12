<!-- awesome-plan project=zedbsd record=ws333-p004 -->

# ws333-p004: `/dev/thermal` の UAPI の全文の案（ユーザーの review）

Status: cleared（2026-11-12 Q1 判定: ユーザーが UAPI の案を承認）
Parent: [WS333](../ws.md)
Queue: q2012-i01（P1）

## 目的

design.md §4.10・§9 の p004: `include/uapi/thermal.h` の全文（ioctl の番号と struct、事象の record、`/dev/thermal-inject`）と、`/sbin/thermal` の使い方の案を書き、design-reviewer を通してユーザーの review に出す。実装（header を `include/uapi/` に置く・device・道具）はこの Phase ではしない（p005 後半・p013）。

## 依存

- p001（設計）。p003・p005 前半・p006・p011・p016 の kernel の口（`include/kern/thermal.h`・`thermal_private.h`・`cpuperf.h`）が main にあり、UAPI の struct はその写しの形に合わせる。
- 2026-11-01 のユーザーの決定（Q333-1〜7 を推しで、Q333-4 は 100 = その物が多い）。

## 受け入れ

- 案の全文（header の案と使い方の文書）が `plan/ws333/` にある。
- design-reviewer 1 回、指摘を反映（反映の表）。
- -m64・-m32 の layout の確かめ（host の cc で `_Static_assert` の付いた案の header を compile する）。
- ユーザーの答え（review）。答えで変わる所を直した版。

## 成果

- 案の header: [../proposed/include/uapi/thermal.h](../proposed/include/uapi/thermal.h)
- 案の説明と `/sbin/thermal` の使い方: [uapi-proposal.md](uapi-proposal.md)
- review: [uapi-review.md](uapi-review.md)

## 進み（2026-11-12 P1）

- 第 1 版（4b7f85a9d6）→ design-reviewer 1 回（blocker 0・major 7・minor 15、[uapi-review.md](uapi-review.md)）→ 第 2 版（m1 を除き全部反映、m1 は reviewer の読み違いと確かめた）。
- layout: `gcc`・`clang` の -m64・-m32 で [layout-test.c](layout-test.c) PASS（status 80・sensor 96・fan 96・meter 80・strategy 48・receiver 64・event 128・cpu 128・domain 248・idle 128 byte、両方で同じ）。zedBSD の clang（`build/llvm/bin/clang`）の `-Wpadded -Werror` で armv7・riscv32・m68k・powerpc・mips・sparcv9・i386 の target が rc 0。
- ユーザーに見てほしい点は [uapi-proposal.md](uapi-proposal.md) §7（ioctl と struct、`/sbin/thermal` の命令、電力の読みを誰に許すかの問い）。

## 他の Phase への持ち越し（案が了承されたら）

- p005 後半: `include/uapi/thermal.h` を置き、kernel の header が include して二重の定義を外す。proposal §6 の「kernel の側に足す事」。試験の注入の switch（`CONFIG_THERMAL_TEST_INJECT`）の mk の案。
- p013: CPU の ioctl（32〜38）と `/sbin/thermal` の CPU・電力・idle・保存と restore。

## ユーザーの決定

- 2026-11-12 ユーザーから、UAPI の案を承認し P1 で進める、という趣旨の指示を受けた。案の §7 の電力の読みの問いは、案の推し A（kernel の 1 秒の写しだけを誰でも、細かい sample は root だけ）で承認されたと Q1 が解釈した。
