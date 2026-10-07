<!-- awesome-plan project=zedbsd record=ws187-p001 -->
# ws187-p001: lock の画面の大きな時計

Status: planned
Disposition: normal
Parent: [WS187](../ws.md)
Queue: q864（予約）

## 範囲

- compositor の lock の画面（`userland/desktop/wayland/` の lock の描画、ws035-p102 の物）に時計（時:分、下に日付）を大きく出す。位置は画面の中央より上（例: 縦の 30〜35% の所に中心）。大きさは画面の短い辺に比例させ、論理 px の定数で上限・下限を持つ。
- 縦長（1080x1920）・横長（1920x1080）・小さい画面（1280x800）で、時計と認証の入力が重ならない。
- 規約: plan/coding-style.md の全文。

## 確認

- build（warning 0）、配置の計算の host 試験。QEMU の PNG（縦長・横長）は T1 に依頼（Q1 経由）。

## 記録
