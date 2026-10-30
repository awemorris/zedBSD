<!-- awesome-plan project=zedbsd record=ws291-p014 -->

# ws291-p014: NVIDIA: 命令の記録の段と pass の終わりの全画面の copy を減らし、big-1000・subs-1000 を NVK に近づける

Status: in-progress
Parent: [WS291](../ws.md)
Queue: q1741（P6）

## 目的

2026-10-31 ユーザーから、担当の無い WS291 の RTX の続きを P6 に割り当てる、という趣旨の指示を受けた。p013（uncleared）の差の内訳: big-1000 は frame 14.4〜16.2 ms = record 6.7〜7.5 ms（48000 record の解読、NVK に無い段）+ submit 2.3〜2.7 + fence 5.1〜5.9。subs は GPU が 1 run 約 89 µs（pass の終わりの全画面の shadow→target の copy）。

## 範囲

1. record の段を無くす新しい形（Vulkan の command buffer を記録の時に直に push buffer に組む、など）の設計と実装。
2. pass の終わりの全画面の copy を減らす（要らない時は省く、または直に描く）。
3. big-100・big-1000・subs-100・subs-1000・scene-1500・scene-5000、desktop の 60 fps、画面の画素の比べ（p013 の rtx-p013.sh）。HAL は変えない、driver は hal_* を使わない。
