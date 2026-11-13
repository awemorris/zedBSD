<!-- awesome-plan project=zedbsd record=ws353-p012 -->

# ws353-p012: cursor の thread（acquire を先に block で、copy の fence をすぐ待つ）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.7、対応表の 1-09〜1-12・2-07。

## 目的

cursor の thread の 1 ms の再試行と 5 ms の pin の見回りを消す。

## 変える file

- `cursor-thread.c`・`cursor-thread.h`: present の後に次の image を `vkAcquireNextImageKHR(UINT64_MAX)` で先に取る。copy を submit したらその fence を無期限で待ち、pin を外し、main に wake（cursor-plane.c の手放しの見張りを p005 で消した物と合わせる）。`CURSOR_RETRY_US`・`CURSOR_PIN_POLL_US`・`cursor_unpin_done` を消す。移動の hold（refresh の終わり近くまで）は cond の期限として残す。
- `cursor-plane.c`: 要る所だけ。

## 残す振る舞い

- 移動を 1 refresh に 1 回、最新の場所で present する事（ws213 の C0）、新しい像は hold しない事、park・停止の答え。

## 確かめ

- build warning 0。QEMU: pointer の移動・client の cursor の像の変更・drag の icon、game mode の出入りの後の cursor plane。WS352 の poll の診断で cursor の thread の timeout の起床が、移動の間の hold の他に 0。

## 受け入れ

- 上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p011（同じ queue の lock と acquire の扱いを揃える）。
