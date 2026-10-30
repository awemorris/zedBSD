<!-- awesome-plan project=zedbsd record=ws331-p003 -->

# ws331-p003: 表 A の機械的な置き換え

Status: planned
Parent: [WS331](../ws.md)
Queue: q1717（P2）

## 目的と範囲

p001 §2 の表 A と §5 の p003 の行のとおり、driver の hal_* と HAL の型・定数を kern_* に、include だけの file から HAL の include を外す。sun4u の 2 file は sparcv9（ユーザーが main で手を入れている）なので別の commit。確かめ: 各 port の build（warning 0）と、変えた driver を含む port の起動の確認（boot-test.sh）。

承認: 2026-10-30 ユーザー「表A,Bを許可します」。HAL の API は変えない。
