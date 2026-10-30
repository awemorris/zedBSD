<!-- awesome-plan project=zedbsd record=ws331-p002 -->

# ws331-p002: kernel に表 B を足す（driver は変えない）

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1717（P2）

## 目的と範囲

p001 の表 B の B1〜B8 を include/kern と src/kern に足す。B1 は kern_device_map の宣言を変えず実装で HAL_SPACE_WC と READ だけの map を作る。B2〜B5 の errno（irq_error に HAL_ERR_STATE → EBUSY）。確かめ: 全 port の vmunix の build（warning 0）。

承認: 2026-10-30 ユーザー「表A,Bを許可します」。HAL の API は変えない。
