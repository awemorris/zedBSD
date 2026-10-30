<!-- awesome-plan project=zedbsd record=ws291-p013 -->

# ws291-p013: NVIDIA: draw と submit の多い bench の CPU の側の費用を Linux（NVK）並みに

Status: in-progress
Parent: [WS291](../ws.md)
Queue: q1728（P3）

## 目的

2026-10-30 ユーザーから、WS291 の RTX の改善を P3 で行う、という趣旨の指示を受けた。p011 の最後の計測（kernel k5）で、scene は NVK と同等だが、draw と submit の多い bench が大きく負けている:

| bench | zedBSD | NVK |
| --- | --- | --- |
| big-1000 | 52 fps（record 約 8 ms・submit 約 10.6 ms、decode 約 6 ms、draw 1 回 約 0.6 µs） | 136.2 fps（1 frame 7.3 ms） |
| subs-1000 | 10.9 fps | 75.4 fps |

## 範囲

1. big-N（record・decode・executor の draw ごとの費用）と subs-N（submit 1 回の費用）の profile。
2. 効く所から nvrtx の Vulkan の backend と kernel の driver を直し、NVK に近づける（目標は NVK と同等、少なくとも差の原因を全部説明できる所まで）。scene-1500・scene-5000 と desktop の 60 fps を後退させない。
3. 計測は 1 本の script（plan/ws291/tests/rtx-bench.sh ほか）、要約だけを読む。HAL の API は変えない。driver は hal_* を使わない。
