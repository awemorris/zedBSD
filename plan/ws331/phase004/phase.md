<!-- awesome-plan project=zedbsd record=ws331-p004 -->

# ws331-p004: i915 の map と MSI、試験の代役

Status: planned
Parent: [WS331](../ws.md)
Queue: q1717（P2）

## 目的と範囲

p001 §5 の p004 の行のとおり。i915 の WC・読み出しだけの map を B1、MSI を B2〜B5、irq の型を kern_irq_ack_t、host の試験の代役の名前と型を直す。ktest-sync.c の HAL の契約の試験の扱いは Q1 と決める。確かめ: build と i915 の host 試験。

承認: 2026-10-30 ユーザー「表A,Bを許可します」。HAL の API は変えない。
