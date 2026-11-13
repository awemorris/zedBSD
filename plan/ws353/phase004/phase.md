<!-- awesome-plan project=zedbsd record=ws353-p004 -->

# ws353-p004: compositor の hotplug の thread（250 ms の tick を消す）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.3・§5.4。

## 目的

hotplug の fence を名前の付いた thread が無期限に待ち、signal で main に知らせる。main の `kwl_output_tick` の 250 ms の look（output-switch.c:46・145〜213）を消し、出力の追従を事象で行う。ユーザーの決定 2 の「最初に直す」の仕上げ。

## 変える file

- `userland/desktop/wayland/output-switch.c`: hotplug の thread（設計 §5.3 の流れ、停止の fence）、`kwl_output_follow`（今の tick の後半）、`follow_pending`。`OUTPUT_HOTPLUG_MS`・`hotplug_checked_ms` を消す。
- `compose.h`・`kwl.h`: hotplug の thread の状態と出口、`follow_pending`。
- `display.c`: `kwl_schedule` から `kwl_output_tick` を外す。
- `compose.c`・`heads.c`・`output-switch.c`: `output_lost`・`heads_stale`・head の `lost` を立てる所（compose.c:552・4665・4892・4906、heads.c:580・695・766）で `kwl_output_follow` を求める。`kwl_display_take` の完了と `output_listed` の終わり、window mode に入った時（`enter_window_mode` の後）で `follow_pending` を見る。window mode でない間（起動の直後、game mode）・display が busy・列挙の途中に来た FIRST・HOTPLUG・lost は `follow_pending` に覚えるだけ（今の tick の条件、output-switch.c:158〜163。設計 §5.4、レビュー M5）。
- `displays-shell.c`: `kwl_displays_pending_tick` を今の期限の仕組みのまま `kwl_displays_tick` 側へ（timer は p006）。
- `main.c`: wake の後の take に hotplug の出口を足す。終了で停止の fence の empty submit（queue の lock の下）と join（`threads.c` の台帳、`KWL THREADS` に `hotplug`）。submit が `VK_ERROR_DEVICE_LOST` で失敗しても join してよい（thread の待ちは device の error で返る、p003 の (7)。設計 §5.3、レビュー M3）。

## 残す振る舞い

- 「新しい fence を先に登録し、列挙はその後」の不変条件、OUTPUT_LOOK_FIRST・SIGNAL・HEADS・RECOVER の意味、limited・move_failed の消し方、出力の移動と head の作り直しの結果。
- Makefile は変えない（新しい file を作らない）。

## 確かめ

- build: compositor（x86_64・arm64・riscv64）warning 0。
- QEMU（Venus、VENUS_DISPLAY=dbus）: 1 本の script で、head の抜き差し（venus-head.sh）→ 出力の移動・head の追従（`KWL OUTPUT` の行を guest の log の file から）、表示中の display を抜いた時の移動、抜き差しの無い 60 s の hotplug の look の回数が 0（main の 10 ms の pass が残る間は `timeouts` では区別できないので数えない。レビュー L12）、game mode の間の抜き差しは window mode に戻った時に追う（M5）、終了で hotplug の thread が join される。
- 回帰: WS113 の出力の移動の試験（plan/tools/guest/ の該当、Q1 の割当）。

## 受け入れ

- 上の判定が PASS。250 ms の look が code に無い。結果・SHA・未実施を書く。

## 依存

- p003。
