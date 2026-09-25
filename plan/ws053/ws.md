<!-- awesome-plan project=zedbsd record=ws053 -->

# WS053: clang/LLVM の LTO を vmunix に安全に適用する

<!-- awesome-plan-current:start -->
Status: completed（2026-09-24、q410）
Primary Milestone: MG001
Related Milestones: MG003, MG008
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（完了）。実機の確認はユーザー
<!-- awesome-plan-current:end -->

## 目標

kernel（vmunix）を clang/LLVM の LTO で build できるようにし、動作が変わらないことと効果を確かめて、実現できるなら既定の build に適用する。

きっかけ: 2026-09-24 ユーザー指示「clang/llvmのLTOを安全にvmunixに適用できないか、検討をお願いします。」「LTOは検討後、実現可能なら、適用をお願いします。これは優先度高めでお願いします。」
HAL の扱い: 2026-09-24 ユーザー判断「HALとカーネルを別にコンパイルする必要はないので、統合して大丈夫です。」

## 結果

**clang と ld.lld で build する 4 platform（amd64・arm64・pcat・pc98）の vmunix は、既定で full LTO（HAL を含む、assembly は native）で build される。**

- top の `Makefile` の `ZEDBSD_KERNEL_LTO`（`full` 既定・`thin`・`none`。他の値は error）が `ZEDBSD_KERNEL_LTO_CFLAGS` を決める。command line か config の .mk で選ぶ。
  開発で link を速くしたいときは `thin`（link 約 3 秒）か `none`（約 1 秒）。full の link は amd64 で約 20 秒。
- 各 `platform/*/vmunix.mk` が kernel・driver・HAL・kernel の libc の C の object にその flag を付け、kernel の object は `$(BUILD)/.kernel-lto` の stamp
  （内容が変わったときだけ書き直す）に依存する。mode を変えると全ての kernel の C が compile し直される。
- 安全の論点（名前付きの section、weak、assembly と linker script からの参照、file ごとの flag、barrier を関数の呼び出しに頼る待ちの loop）を確かめた（p001）。
- 見つけて直した潜在の不具合: kernel に `memmove`・`memcmp` が無かった（compiler は freestanding でも呼んでよい）。`src/kern/kcrt.c` に別名を足した。
- 効果（amd64、guest、中央値）: system call の入口 −10%、cached file の read −16%、exec −3%、anonymous の fault −3%、遅くなった試験は無い。
  vmunix の大きさ: amd64 −1.5〜−2.8%、pcat −1.7%、pc98 −2.1%、arm64 +2.0%。
- 回帰（amd64、guest）: sh の差分試験 1388/1425（LTO の前と同じ数）、make 91/91、対話 41/41。boot（QEMU）: amd64・rpi4（raspi4b）・pcat（BIOS）・pc98（PC-98 fork）で login。

## 制限と移管

- 実機の確認（amd64 の機械、Raspberry Pi 4、PC/AT、PC-98）は未実施。ユーザーに頼む。
- guest の clang の遅さ（BUG-033）は LTO では直らない（page の fault・fork・exec の処理の量）。WS046 p007 で扱う。
- sparcv9・x68k は gcc で build していて、この WS の範囲外。
- 残した道具: [plan/tools/kbench/](../tools/kbench/)（kernel の microbenchmark）、[plan/tools/pc98-boot.py](../tools/pc98-boot.py)（pc98 の起動の確認）。

## Phase 一覧

| Phase | 内容 | 結果 |
| --- | --- | --- |
| ws053-p001 | 調査と設計 | cleared（q406-i01）。安全に適用できる。`memmove`・`memcmp` の欠けを直した。full LTO+HAL を既定にする設計 |
| ws053-p002 | amd64 に適用 | cleared（q407-i01）。既定で full LTO、回帰が LTO の前と同じ |
| ws053-p003 | arm64（rpi4）に適用 | cleared（q408-i01）。QEMU raspi4b で login、vmunix +2.0% |
| ws053-p004 | pcat・pc98（i386）に適用、mode の定義を top の Makefile にまとめる | cleared（q409-i01）。両方 login、pcat −1.7%・pc98 −2.1% |
| ws053-p005 | 規約の確認と最後の回帰 | cleared（q410-i01）。違反 0、既定の 5 config が full LTO・warning 0、amd64・rpi4 で login |

Phase の記録は WS の完了で削除した（git の履歴に残る。[q406](../history/queue-q406.md)〜[q410](../history/queue-q410.md)）。
