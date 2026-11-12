<!-- awesome-plan project=zedbsd record=ws333-p020 -->

# ws333-p020: Pi 5 の provider（fan の level・rpm、受け手、boost）

Status: planned（2026-11-12 P1 が design.md §9 から立てた。実機の割当が要る）
Parent: [WS333](../ws.md)

## 目的

design.md §4.7・§9 の p020（WS303 design §4.9）: `src/drivers/platform/rpi5/rpi5-fan.c` を熱の core の fan（level と rpm）・strategy の受け手（trip の組、DT より遅くしない）・boost（level の下限、実際の level = max(trip, boost)）として登録する。lock の範囲を整理する。p005 前半で sensor の push（ZONE、CPU、critical は DT の値）は済み。

## 依存

- p005（熱の core）、p016（非常の停止、Pi 5 の critical 110 °C と poweroff）。

## 受け入れ

- host 試験（模擬の fan の register と DT の trip）。
- arm64（rpi5）の vmunix の warning 0、QEMU raspi4b の起動の回帰（rpi4 の kernel）。
- Pi 5 の実機（Q1 の割当）: fan の rpm と level が読め、strategy で trip の組が変わり、boost で level が上がる（WS303 §6.3 の Pi 5）。
