<!-- awesome-plan project=zedbsd record=ws333-p002 -->

# ws333-p002: amd64 の CPU の idle を HLT から MWAIT の C-state に（5330 で idle の電力を下げる）

Status: in-progress
Parent: [WS333](../ws.md)
Queue: q1735（P1）

## 目的

2026-10-31 ユーザーから、WS291 p022 の件で CPU の省電力（idle の C-state・P-state）を先に直す、という趣旨の指示を受けた。5330 の zedBSD は idle で package 12.99 W（Linux 3.2〜3.5 W）。idle が HLT（C1）だけの見込み。

## 範囲

- amd64 の HAL の実装（src/hal/amd64/ の idle）を MWAIT に（CPUID で MWAIT・ARAT を確かめ、最深の C-state の hint、割り込みで起きる ECX bit0）。hal.h は変えない（実装の補完）。MWAIT の無い CPU は HLT のまま。
- 5330 で idle の package の電力と、WS291 p022 の qs40 × 600 の GPU の周波数・fps を前後で測る。起動しない時は grub・BootNext で Debian に戻す形を保つ。
- P-state（HWP）は p001 の設計の後の Phase。
