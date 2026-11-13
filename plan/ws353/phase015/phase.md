<!-- awesome-plan project=zedbsd record=ws353-p015 -->

# ws353-p015: backend と WSI の小さな直し

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.11・§6、対応表の 1-16、「poll-report に無く見つけた物」。

## 目的

daemon link の network の答えの 10 ms と、Wayland の WSI の 50 ms の slice を消す。

## 変える file

- `userland/desktop/libkeiland-backend-zedbsd/network-zedbsd.c`: `NETWORK_ANSWER_LOOK_MS` をやめ、答えの fd を link の poll（`network_poll_fill`）に入れる（自分の書きの shutdown の POLLERR は kernel の BUG-149 の直しで起きない事を確かめる）。
- `userland/desktop/libvulkan/wsi-wayland.c`: frame の進みの待ちを、接続の fd と progress pipe の poll の無期限（呼び手の期限と 1 s の pace の残り）に。`WAYLAND_WAIT_SLICE_MS` を消す。1 s の pace は残す（設計 §6）。

## 確かめ

- build warning 0。QEMU: network の scan・接続の要求の答え（settings の network の面）、FIFO の Vulkan の client が止まらない・隠れた surface の app が 1 s の pace で進む。poll の診断で daemon link（network）と client の 50 ms の起床が無い。

## 受け入れ

- 上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p001 だけ（別の file・別の thread で、main loop の Phase に依らない。いつでも並べて進めてよい。レビュー L11）。
