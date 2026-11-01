<!-- awesome-plan project=zedbsd record=ws322-p001 -->

# ws322-p001 調べと設計: VF2 の Wave511（H.264・H.265 decode）と Wave420L（H.265 encode）を PowerVR の executor の Vulkan Video として

- WS: [WS322](../ws.md)。状態: in-progress（2026-11-01 P2 q1845。設計の第 2 版と review 1 の反映は済み、Q1 の受け入れと問いの答え待ち）。担当: P2（q1845）。
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

## 結果（2026-11-01 P2、q1845）

- 設計: [design.md](../design.md)（第 2 版）。敵対的レビュー: [design-review.md](../design-review.md)（27 件、blocker 1。全部を反映。blocker は「出力の宛先の決め方」で、Wave5 の frame buffer の index が参照と表示の組であることを見落としていた）。
- 実機・QEMU の観測は無い（読んだ source と公開の文書だけ）。VF2 の板で読むだけの確かめは、実装の最初の Phase（p002）の probe に入れた（板は P4 が BUG-387 で使用中のため、この Phase では触っていない）。
- HAL の API の変更は要らない（design §19）。

## 判断を待つ点

背景（共通）: WS322 は VisionFive 2（StarFive JH7110、RISC-V の板）の動画の hardware を、GPU（PowerVR）の Vulkan の driver の「Vulkan Video」（動画の decode・encode の Vulkan の拡張）として使えるようにする WS。JH7110 には Chips&Media 社の 2 つの VPU（video processing unit、動画の専用の回路）がある: Wave511（H.264・H.265 の decode）と Wave420L（H.265 の encode）。どちらも中で小さな processor が firmware（機械の中で動く program の file）を動かす形で、firmware は Chips&Media の binary だけが配られている。

### Q322-1 VPU の firmware の配布（Q1 が先例で決められる見込み）

- 今の状態: 要る file は `chagall.bin`（Wave511）と `monet.bin`（Wave420L）。StarFive の GitHub（soft_3rdpart）に Chips&Media の license の文と一緒にある。license は「binary の形での再配布と使用を許す、著作権表示と条件と免責を添える、逆解析（中身を分解して調べる事）を禁ずる、無保証」。
- 選択肢:
  - A（推し）: zedBSD-firmware に入れ、`wave511-firmware`・`wave420l-firmware` の package で `/lib/firmware/` に license と一緒に入れる。image を作る時に license の同意を取る今の仕組み（PowerVR の firmware と同じ）。得: VF2 で動画の hardware が使える。失: 逆解析の禁止を守る（driver は firmware の返事と register だけで作る。PowerVR と同じ）。
  - B: 配らず、使う人が自分で入れる。得: 配布の責任が無い。失: 既定の image で動かない。
- 推しの理由: PowerVR の firmware（同じ形の license、逆解析の禁止）を既に同じ形で配っている。

### Q322-2 Wave420L（H.265 encode）の参考の source の license（ユーザーの判断: 危険の受け入れ）

- 背景: Wave511（decode）は Linux の driver（Chips&Media 自身が `GPL-2.0 OR BSD-3-Clause` で出した物）があり、仕様をそこから読める。Wave420L（encode）は Linux に driver が無く、register（hardware を動かす設定の場所）の値と手順が分かる公開の物は StarFive の soft_3rdpart の「vpuapi」（Chips&Media の参考の library の source）だけ。
- 問題: その source の各 file の先頭には `SPDX-License-Identifier: LGPL-2.1 OR BSD-3-Clause`（どちらかの open source の license を選べる、という機械が読む印）があるが、同じ file に「confidential and proprietary（秘密・専有）、Chips&Media の許諾の下でだけ使える」という古い文もあり、directory の `LICENSE.txt`（2022 年）は「binary の再配布だけを許す、逆解析を禁ずる」の文。印と文が食い違う。
- 今の状態: この設計では file の名前の一覧と license の行だけを見て、中身は読んでいない。encode の Phase（p008〜p011）はこの答えの後。
- 選択肢:
  - A（推し）: file ごとの SPDX の BSD-3-Clause を採る。guardrail の「値は事実として出典を明示して使う、code・comment・構造・名前は写さない」を、この source にも当てる。食い違いは記録に残し、StarFive・Chips&Media から異議があれば encode を外す。得: VF2 で H.265 の hardware の encode ができる。失: license の食い違いの危険を受け入れる。
  - B: 先に StarFive に問い合わせ、はっきりするまで encode を待つ。得: 危険が小さい。失: 答えが来る保証が無く、encode が止まる。
  - C: VF2 の hardware の encode をやめ、H.265 の encode は独自の software（WS330）だけ。得: 危険が無い。失: VF2 の encode は CPU で遅い（1080p30 は届かない見込み）。
- 推しの理由: 同じ会社が同じ系統の code（Wave5 の Linux の driver）を BSD-3-Clause で公開しており、file の SPDX は著作者の明示の印。値と手順の引用だけなら code の license の問題は小さい（guardrail が GPL で同じ扱いを既に認めている）。

### Q322-3 受け入れの解像度と速さ（ユーザーの判断: 範囲）

- 背景: Wave511 は公表で 4K60 の decode ができる。PowerVR の GPU と memory の帯域の余裕は VF2 では大きくない。今の libmedia は decode の絵を一度 CPU に読み出す（readback）ので、GPU で直に表示する道（WS318）の前は速さに限りがある。
- 選択肢:
  - A（推し）: 4K（H.264 4096×2304・H.265 4096×2176）まで Vulkan に報告し、受け入れは「1080p30 の再生が落ちない」と「1080p60・4K30 の decode の正しさ（hash の一致）と時間」。1080p60 の再生は WS318 の GPU の道ができた後の最後の Phase（p012）で確かめる（今の libmedia は絵を CPU に読み出す道で、VF2 の GPU の memory は CPU から cache 無しでしか読めないので、1080p60 に届かない見込み）。
  - B: 1080p30 だけを受け入れにする（報告も 1920×1088 まで）。得: 早く終わる。失: 4K の動画は software に落ちる。
  - C: 4K60 の再生まで受け入れにする。失: WS318 の GPU の道と帯域の測りが前提で、届かない危険が高い。
- 推しの理由: decode の正しさは解像度によらず同じ code で確かめられ、再生の速さの目標は今の板と今の libmedia で届く見込みの所に置き、高い目標は WS318 の後に回す。

### Q322-4 libvulkan の H.265 と encode の転送の持ち主（Q1）

- WS318 §10.3 は「最初にそれを要る driver の WS（WS319 を想定）が足す」。WS322 が WS319 より先に進むなら WS322 の p005（H.265 decode）・p010（encode）で足す。推し: 先に着手する方が持ち、もう一方は取り下げ。

### Q322-5 ws.md の題の直し（Q1）

- 題の「H.264 encode は hardware に無く libavcodec」は 2026-11-01 のユーザーの決定（WS318 Q318-2、libavcodec を使わない）で古い。「独自の software（WS329）」に直す。master.md の行も同じ（Q1）。この Phase では ws.md の題を変えず、Q1 に任せる。

### Q322-6 実装の Phase の前倒し（Q1）

- WS318 の ws.md のユーザーの決定は「libmedia を completed にしてから個別の driver」。2026-11-01 の前倒しで明示されたのは p001（この Phase）。p002〜p004（VF2 の VPU の bring-up と H.264 decode）は今の libvulkan・libmedia で確かめられ、WS318 と file が重ならない（libmedia の DISTINCT の道の小さな直しを除く、Q322-7）。推し: A 始める。ユーザーの指示の範囲の読みなので、Q1 が迷えばユーザーへ。

### Q322-7 libmedia の DISTINCT の道の直しの持ち主（Q1）

- 今の libmedia は DISTINCT（参照と出力が別の image）を扱う code を持つが、i915・nvrtx・Pi 4 の driver は全部 COINCIDE（同じ image）なので、どの実の driver でもまだ走っていない。VF2 で初めて走る。推し: A WS322 p004 の中で最小に直し、WS318 の担当に知らせる（WS318 は後で frame の model を作り直す）。

### 記録の注意（Q1）

- ws.md の「ユーザーの決定（2026-10-30、原文）」の節は AGENTS.md §8（原文を載せない）に合わない（review 1 の 27 番）。この Phase の作った物でないので、直すかは Q1。
