<!-- awesome-plan project=zedbsd record=ws193-p001 -->

# ws193-p001: menuconfig の新しい階層と Build boot image

Status: planned（2026-10-09 Q1、q918 P1）
Parent: [WS193](../ws.md)

## 手順

1. tools/menuconfig.py・root の Makefile の menuconfig の規則・config.mk の変数・menuconfig-target-host-test.py を読む。
2. ws.md の階層を実装する。Base・Desktop の All / Select、Packages の階層は userland/packages の directory の分類から作る。
3. Build boot image: menu から image の build を `-j$(nproc)` で走らせ、進捗の bar と今の対象の名前を出す（make の出力から対象を拾う形など、設計を phase.md に）。失敗の時は log の場所と末尾を出す。
4. host 試験（menuconfig-host-test の追従、階層・既定値・保存の形）。build warning 0 に相当する確認。

## 受け入れ

- 設計と実装の記録、host 試験 PASS、T1 の依頼の行（p002）。
