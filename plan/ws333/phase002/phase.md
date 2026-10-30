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

## 実装（P1、2026-10-31）

- `src/hal/amd64/bsp-pcat/idle-suspend.c`: 通常の idle（`bsp_cpu_idle()`）を足した。起動の CPU で一度、CPUID で MONITOR/MWAIT（leaf 1 ECX bit 3）、leaf 5 の拡張と masked の割り込みでの break（ECX bit 0・1）、ARAT（leaf 6 EAX bit 2、local APIC の timer が全 C-state で動く）を確かめ、leaf 5 EDX の sub-state のある C-state のうち `IDLE_WAIT_DEEPEST_CSTATE`（既定 3 = hint の C-state 欄 2、Alder Lake の C6）以下の最深の物の最深の sub-state を hint にする。待ちは S0ix（hal_cpu_idle_suspend）と同じ形（割り込みを mask したまま MONITOR・MWAIT ECX=1、`sti; nop; cli` で割り込みを受ける）、tick は止めない。どれかが無い CPU、TSC が invariant でない時は前と同じ `sti; hlt; cli`。
- `src/hal/amd64/task.c` の `hal_cpu_idle()` は `bsp_cpu_idle()` を呼ぶ（`bsp.h` に宣言）。`include/hal/hal.h` は変えていない。
- build: `make ZEDBSD_CONFIG=plan/ws291/tests/config-5330.mk BUILD=build/p022 build/p022/vmunix`（-Werror、warning 0）。

## 計測（実機 5330、NVMe の zedBSD、main 48a27d158 の image、kernel は WS291 p022 の GPU の変更＋`plan/ws291/tests/i915-stats-diag.patch` の診断、`plan/ws291/tests/5330-mview.sh idle`・`run`）

| idle | idle の package（20 s） | c600 の 1 回目（起動から約 1 分） | c600 の 2 回目 |
| --- | --- | --- | --- |
| 前: HLT（C1） | 13.00 W | 30.51 fps、GT 800〜850 MHz、13.0 W | 30.72 fps、850・800 MHz、13.0 W |
| 後: MWAIT C6（hint 0x2x、既定） | 11.70 W | 34.91 fps、850〜1150 MHz、15.9 W | 32.58 fps、850・900 MHz、13.1 W |
| 試し: MWAIT C10（`IDLE_WAIT_DEEPEST_CSTATE` 7） | 11.59 W | 35.03 fps、15.6 W | 32.71 fps、13.1 W |

- PL1（MMIO）13.0 W。前は idle で既に PL1 に張り付き、run の始めの余裕が無かった。後は idle が PL1 を下回り、1 回目は PL1 の時間窓の余裕で 15.6〜15.9 W・最高 1150 MHz、続く run は 13.1 W・850〜900 MHz（約 +6%）。C6 と C10 の差は計測の揺れの内なので、出口の遅れの小さい C6 を既定にした。
- 起動: 5330 で C6・C10 の両方の kernel が起動し、desktop の session・mview が動いた。HAL の起動の行（`A64 IDLE WAIT ...`）は dmesg・`/boot/boot0/log` に出ず、MWAIT の路を通った事は電力の差からの推定（未確認）。
- 未実施: QEMU（MONITOR の無い CPU で HLT に戻る路）の起動の確かめ、AMD の CPU。
- **残り**: idle は Linux（3.2〜3.5 W）にまだ遠い（11.6 W）。core の C-state だけでは package が深い状態に入れていない見込み（1 kHz の tick の全 CPU の起床、P-state（HWP 無し）、PCIe の ASPM・display の PSR などの package C-state の妨げ、未調査）。P-state は p001 の設計の後。
