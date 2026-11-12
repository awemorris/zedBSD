<!-- awesome-plan project=zedbsd record=ws333-p029 -->

# ws333-p029: i915 の GT を RC6 に入れる（HW_CONTROL で Vulkan が DEVICE_LOST になる原因と、forcewake の参照の数の性能）

Status: in-progress（2026-11-13 Q1、q2037-i01 P3。考える Phase、実機は最後に）
Parent: [WS333](../ws.md)

## 目的

[p027](../phase027/phase.md) の続き。5330 の package の C-state（PC2）には、GT が RC6 に入る事と tick を下げる事の両方が要る（tick は 2026-11-13 に amd64 を 100 Hz にした）。p027 の結果:

- forcewake を参照の数で握る形（agent/p3-ws333b 6e208bfd3b）と RC_CONTROL の HW_CONTROL|EI_MODE|RC6_ENABLE で、GT RC6 99.9 %・PC2 85〜88 %・4.34 W。
- ただし HW_CONTROL を入れると qs40 で Vulkan が DEVICE_LOST。p027 は RC6_ENABLE だけに戻した（GT は RC6 に入らない）。
- forcewake の直しだけで qs40 c1 が 338 → 163 fps（1 回ずつの数、未確かめ）、c600 は 39.30 → 37.35 fps。

p027 の i915 の source の直しは、性能の回帰の疑いがあるので main に merge していない。

## 解析する事（code と PRM で根拠を書く）

1. HW_CONTROL で RC6 に入った後に GT の仕事が失われる理由: RC6 の出入りで要る物（render の power context の置き場、RC6 から戻る時の待ち、RC の各 threshold・EI・idle の値、context の save/restore、forcewake を握らずに触っている register、GuC の無い時の ring の扱い）。PRM の RC6 の節と、値の出典（guardrail の GPL の値の規則）。
2. forcewake の参照の数の形の性能: worker が 20 ms で返す・RPS の hook・telemetry の一瞬の wake が描画の流れで何回 wake を起こすか。c1 の fps の差が本当か（複数回の測り）と原因。本来の形（Linux の i915 のように仕事の有る間は握り、idle で離す、等）を考えて直す。
3. 直した後に 5330 で: qs40 の c1・c600 が直す前と同等（DEVICE_LOST 無し）、普段の起動の idle で GT RC6・PC2 が出る。全部一回だけの起動と watchdog の下。

## 受け入れ

- DEVICE_LOST の原因が根拠付きで分かり直っている。qs40 の c1・c600 が直す前の ±5 % の中（数回の測りの平均）。idle の GT RC6 と PC2 が p027f と同等。build warning 0、host 試験 PASS。
