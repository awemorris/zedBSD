<!-- awesome-plan project=zedbsd record=ws353-p012 -->

# ws353-p012: cursor の thread（acquire は変化の後だけ、copy の fence をすぐ待つ）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.7、対応表の 1-09〜1-12・2-07。

## 目的

cursor の thread の 1 ms の再試行と 5 ms の pin の見回りを消す。idle で acquire に block しない。

## 変える file

- `cursor-thread.c`・`cursor-thread.h`: box の変化の後に image を timeout 0 で取り、取れなければ `vkAcquireNextImageKHR(UINT64_MAX)`（cursor の chain の image は前の cursor の present の job が終われば空く。先回りの acquire はしない。設計 §5.7、レビュー S4）。copy を submit したらその fence を無期限で待ち（今の present の経路の `KWL_CURSOR_FENCE_WAIT_NS` 100 ms、cursor-thread.c:477 を無期限に）、pin を外し、main に wake（cursor-plane.c の手放しの見張りを p005 で消した物と合わせる）。`CURSOR_RETRY_US`・`CURSOR_PIN_POLL_US`・`cursor_unpin_done` を消す。移動の hold（refresh の終わり近くまで）は cond の期限として残す。
- `cursor-plane.c`: 要る所だけ。

## 残す振る舞い

- 移動を 1 refresh に 1 回、最新の場所で present する事（ws213 の C0）、新しい像は hold しない事、park の答え（上限 2 s、`PLANE_PARK_WAIT_MS` cursor-plane.c:54）、停止の時の copy の fence の待ち（`kwl_cursor_thread_finish` の 100 ms と `stop_failed`、打ち切りとして残す。レビュー L6）。

## 確かめ

- build warning 0。QEMU: pointer の移動・client の cursor の像の変更・drag の icon、game mode の出入りの後の cursor plane、sleep の前の park（`sleep_hold`）が 2 s 以内に答える。WS352 の poll の診断で cursor の thread の timeout の起床が、移動の間の hold の他に 0。

## 受け入れ

- 上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p011（同じ queue の lock と acquire の扱いを揃える）。
