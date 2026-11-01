<!-- awesome-plan project=zedbsd record=ws324-p001 -->

# ws324-p001 調べと設計: RTX 2070 の NVENC（H.264・H.265 encode）と NVDEC（H.265 decode）を nvrtx の Vulkan Video として

Status: planned（2026-11-01 21:46 Q1。担当: P1、q1854）

- WS: [WS324](../ws.md)。背景: 2026-11-01 ユーザーから、WS291 をラップアップし、RTX の H.264 encode・H.265 decode・H.265 encode の実装に移る、という趣旨の指示を受けた。ws.md の依存（WS318 の completed の後）は待たない（WS322 と同じ扱い）。
- 前例: [WS243](../../ws243/ws.md)（nvrtx の NVDEC の H.264 decode、Video Player の GPU の道で 1080p60）。その設計・code・試験の道を土台にする。[WS322](../../ws322/ws.md) の design.md（Q322-4: libvulkan の H.265 と encode の転送は WS322 と WS324 のうち先に着手する方が持つ）。WS318 の ws.md の p011a・p011b（Vulkan の header の版）・p016〜p022（H.265 の decode と encode）の設計の範囲。

## やる事

1. hardware の確認: TU106（RTX 2070）の NVDEC の H.265（Main・Main10、解像度）と NVENC（H.264・H.265、profile、rate control、B frame の有無）、firmware（WS243 の NVDEC の firmware の扱いと NVENC の要る物）。
2. 出典と license: 公開の資料と、参考にできる source（nouveau・NVK・open-gpu-kernel-modules など。GPL の物は仕様の理解だけ、guardrail の「出典」）。
3. Vulkan Video の範囲: `VK_KHR_video_decode_h265`、`VK_KHR_video_encode_queue`・`_h264`・`_h265`。Vulkan の header（sysroot）の版と libvulkan の転送（WS318 p011a・p011b の範囲を、この WS で先に持つか）。sysroot・header の変更は toolchain に当たるので Q1 の許可が要る点を明記する。
4. libmedia の口: H.265 decode の parser（WS318 p016〜p019 の範囲）と encode の口（WS318 p012a・p021・p022）のうち、この WS で要る最小の物。WS318 を始めずに済む形を探す。
5. 試験の道: host の試験、適合の stream（HEVC の適合の stream）、encode の確かめ（自前の decoder・software との比較）、10.0.10.3 の実機（Video Player の GPU の道）。
6. 実装の Phase の分け方と見積り（LW）、design-reviewer の敵対的レビューと反映。

## 受け入れ

- plan/ws324/design.md に 1〜6、design-review.md にレビューと反映の表。
- HAL の API の変更が要るなら、なぜ既存の HAL・driver・kernel の側で足りないかを書いた案だけ。
- ユーザーの判断が要る点は、背景・選択肢・得失・推し・用語の説明つきで phase.md に書き Q1 に返した。
