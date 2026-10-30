<!-- awesome-plan project=zedbsd record=ws332-p002 -->

# ws332-p002: KERN_CLOCK_HZ の削除

Status: planned
Parent: [WS332](../ws.md)
Queue: q1722（P3、p001 の後）

## 範囲

p001 の分類に従い、KERN_CLOCK_HZ を使う所を時間の単位の API に置き換え、include/kern/clock.h の KERN_CLOCK_HZ と HAL_TIMER_FREQUENCY の参照を消す。確かめ: 全 port の build（warning 0）、amd64・pcat・rpi4・riscv64・m68k の QEMU の起動、TCP・wifi・USB の時間に関わる host 試験。
