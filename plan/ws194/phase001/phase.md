<!-- awesome-plan project=zedbsd record=ws194-p001 -->

# ws194-p001: keiland-linux・keiland-freebsd の package の確認と install の確認

Status: planned（2026-10-09 Q1、q919 P1）
Parent: [WS194](../ws.md)

## 手順

1. root の Makefile の keiland-linux・keiland-freebsd の規則、plan/tools/keiland-linux・keiland-freebsd の guest の build の手順（必要な package の既存の一覧）を読む。
2. package の一覧を一箇所に（apt・dnf/yum・pacman・pkg の名前の対応）。足りない物の検出（dpkg-query・rpm -q・pacman -Q・pkg info）。
3. 対話: 一覧を出し「install しますか [y/N]」、sudo で導入。build の成功の後に install の確認。端末でない時は止まって一覧を出す。
4. host 試験（検出と一覧の形、対話の分岐を模擬）。

## 受け入れ

- 設計と実装の記録、host 試験 PASS、T1 の依頼の行（p002）。
