<!-- awesome-plan project=zedbsd record=ws353-p007 -->

# ws353-p007: 締め切り B（UI の期限）を timer へ（main loop の 3）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.5、対応表の 2-04・2-18・2-22〜2-25・2-31・2-33・2-35〜2-37・2-39・2-40・2-44。

## 目的

UI の締め切りを timer にする。

## 対象（file）

- band の長押し（shell.c、10 ms ごとをやめ閾値の時刻に 1 回）、titlebar の key repeat（titlebar-shell.c）、apps bar の hover・leave（apps-bar.c）、OSK の手書きの認識（keyboard.c）、greeter の lock の card・security key（greeter.c）、home・corner（home.c・corner.c）、notify-log を閉じる（notify-log.c）、rotate の hold（output-rotate.c）、scanout の pointer の静止・lend の拒み（scanout.c）、layer cache の静けさ（layer-cache.c）、glass の blur の hold（glass.c）、volume の送りの間隔（volume.c）、system の scan の表示（system.c）、printers の 1 s（printers-shell.c、面がある間の timer。周期の読み直しが消えるのは printd の push の後＝範囲の外）。
- `kwl_titlebar_tick`・`kwl_glass_tick` など毎 pass の tick を handler に割る。

## 残す振る舞い

- 各期限の長さと結果。key repeat の間隔、hover の遅れ、layer の焼きの時期。

## 確かめ

- build warning 0。QEMU: key repeat（titlebar の field）、apps bar の hover、OSK の手書き、lock の card、notify-log、rotate、layer cache の焼き（`--log-frames` の行を file から）。idle の main の timeout の起床が、対象の分 0。

## 受け入れ

- 対象の期限が timer で動く。結果・SHA・未実施を書く。

## 依存

- p005（p006 と並べてよい。同じ file を触る所は順に）。
