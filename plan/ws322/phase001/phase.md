<!-- awesome-plan project=zedbsd record=ws322-p001 -->

# ws322-p001 調べと設計: VF2 の Wave511（H.264・H.265 decode）と Wave420L（H.265 encode）を PowerVR の executor の Vulkan Video として

- WS: [WS322](../ws.md)。状態: planned（2026-11-01 20:43 Q1）。担当: P2（q1845）。
- 背景: 2026-11-01 ユーザーから、P2 で WS322 を実行する、という趣旨の指示を受けた。ws.md の依存（WS318 の completed の後）より前倒し。WS318（libmedia の作り直し）は planning のままなので、この Phase は libmedia の今の Vulkan Video の利用の形（WS236 の Pi 4・WS243 の NVDEC が使う口）に合わせて設計し、WS318 で口が変わる所は「WS318 に依る」として分けて書く。

## 目的

VF2（JH7110）の video の hardware を zedBSD で使うための調べと設計。実装はしない（実装の Phase の分け方まで）。

## やる事

1. hardware の確認: JH7110 の Wave511（decode、H.264・H.265 の profile・level・解像度）と Wave420L（encode、H.265 の profile・rate control）の register・memory・IRQ・clock・reset・power domain、device tree の node。可能なら VF2（10.0.30.2）の上で読むだけの確かめ（板は今 P4 が BUG-387 の試験中。使う時は Q1 に窓を頼む）。
2. firmware: Chips&Media の firmware の入手元・license・配布の可否（zedBSD-firmware に入れられるか）。
3. 出典と license: 参考にできる公開の資料・driver（Linux の wave5 の driver は GPL なので code を写さない、仕様の理解だけ。guardrail の「出典」に従う）。
4. Vulkan Video の範囲: `VK_KHR_video_queue`・`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264`・`_h265`・`VK_KHR_video_encode_queue`・`VK_KHR_video_encode_h265` のどこまで。VPU は GPU と別の hardware だが PowerVR の Vulkan の executor（`src/drivers/gpu/powervr/`）の video queue として見せる（ユーザーの決定）。既存の例: WS236（Pi 4 の MMAL を Vulkan Video に）・WS243（nvrtx の NVDEC）。
5. memory: DPB・bitstream の buffer と PowerVR の image の共有（zero copy の可否、cache の一貫性。VF2 は uncached の別名が効かない事に注意: kern_pmem_map_uncached）。
6. 試験の道: host の試験、JVT・HEVC の適合の stream、WS326 の software の decoder との一致、VF2 の実機。
7. 実装の Phase の分け方と見積り（LW）。

## 受け入れ

- 設計文書（plan/ws322/design.md）に上の 1〜7 が書かれ、design-reviewer の敵対的レビューを通して指摘を反映した。
- HAL の API の変更が要るなら、なぜ既存の HAL・driver・kernel の側で足りないかを書いた案だけ（承認は別）。
- ユーザーの判断が要る点（firmware の配布、範囲など）は問いとして背景・選択肢・推しを書いて Q1 に返した。
- ws.md の Phase の表に実装の Phase を足す案。
