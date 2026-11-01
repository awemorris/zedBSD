<!-- awesome-plan project=zedbsd record=ws324-p001 -->

# ws324-p001 調べと設計: RTX 2070 の NVENC（H.264・H.265 encode）と NVDEC（H.265 decode）を nvrtx の Vulkan Video として

Status: in-progress（2026-11-01 22:30 P1、q1854。design.md 第 1 版を書いた。design-reviewer の review と反映、Q1 の受け入れが残り）

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

## 結果（第 1 版）

- 設計: [design.md](../design.md)。UAPI の番号と capset の bit の案: [proposed/gpu-op-video-encode.md](../proposed/gpu-op-video-encode.md)。HAL は変えない（design §19）。
- 分かった事の要点:
  - H.265 decode: NVDEC の HEVC の picture の構造と作業の buffer の大きさの式は NVIDIA の open-gpu-doc（MIT）に公開。WS243 の NVDEC の channel と同期の decode の道に足すだけで済む見込み。Mesa の NVK の公開の版（26.3.0-rc3）に H.265 は無い。
  - NVENC: open-gpu-doc に class の method と driver との interface（`nvenc_drv.h`）が公開（MIT）。TU106 の class は 0xC4B7。それを駆動する公開の driver は見つからない（zedBSD が最初）。公開の 2 版で構造の末尾が 4 byte ずれる。動きの探索などの制御の既定の値は文書に無い。→ 関門の Phase（p009）を先に。
  - firmware: NVDEC と同じく GSP-RM が NVENC の ucode を持つ見込み（未確認、p009 で）。
  - Vulkan の header: `vulkan_video.h` は既に 1.4.309 の入力から選んでおり、H.265 decode と KHR の encode も同じ入力にある。`vulkan_core.h` の BETA の囲みの encode の値 51 個は 1.4.309 の KHR と全部同じ（host で照合）。
  - libmedia: driver の確かめに libmedia は要らない（Vulkan を直に使う試験の道具）。要るのは H.265 の解析だけ。
- 実機（10.0.10.3）は使っていない（2026-11-01 に Q1 から、P3 が WS291 p027 に使うので触らないよう指示を受けた。この Phase の設計には読むだけの確かめも要らなかった）。

## 判断を待つ点

前提: WS324 は RTX 2070（NVIDIA の Turing 世代の GPU、chip は TU106、amd64 の PC の 10.0.10.3 に挿してある）の video の hardware を、zedBSD の kernel の中の Vulkan の実装（nvrtx の executor）の **Vulkan Video**（Vulkan の video の decode・encode の拡張。zedBSD では app が video の hardware を使う唯一の口）として見せる WS。H.264 の decode は WS243 で済み、残りは H.265（HEVC）の decode と、H.264・H.265 の encode。**NVDEC** は NVIDIA の video の decoder の engine、**NVENC** は encoder の engine。どちらも GPU の中の独立の engine で、NVIDIA の firmware（GSP-RM: GPU の中の管理用の processor で動く NVIDIA の資源管理の program）を通して使う。今の状態は設計の第 1 版で、code は書いていない。

### Q324-1 H.265 の解析をどこに作るか（Q1）

- 背景: Vulkan Video の decode は stateless で、app（か libmedia）が bitstream を解析して H.265 の parameter set（VPS・SPS・PPS: 動画全体・絵ごとの設定）と slice header の値を Vulkan の構造（StdVideoH265…）にして driver に渡す。driver の試験にも、この解析が要る。libmedia の H.265 の解析は WS318 p016〜p019（標準廉価実装手順で Sonnet が作る予定）の範囲で、まだ無い。
- 選択肢:
  - A（推し）: WS318 p016〜p019 の範囲を、libmedia の独立の module（`h265*.c`、libmedia の runtime には繋がない）として WS324 で先に作り、試験の道具（`vkvideo-probe`）がそれを直に compile する。WS318 は後でそれを runtime（p020）に繋ぐ。得: 解析を一度だけ書く、WS318 の手間も減る。失: WS318 の Phase を前借りするので、WS318 の design の H.265 の節を直す必要（Q1 が WS318 に記録）。
  - B: 試験の道具の中に試験だけの解析を作る（今の `vkvideo-probe` の H.264 と同じ形）。得: WS318 に触らない。失: 約 2500 行の重複。
  - C: WS318 p016〜p019 を待つ。失: ユーザーの指示（WS318 を待たない）に反する。
- Q1 の判断で決められる見込み（WS 間の範囲の移し）。

### Q324-2 Vulkan の header の選び直しと sysroot の作り直し（Q1、toolchain の許可）

- 背景: zedBSD の Vulkan の header（`include/libc/vulkan/`）は Khronos の宣言から必要な物だけを選んで作る。今は H.264 の decode だけ。H.265 decode と encode の宣言を足す必要がある。header は target の build が共有の sysroot（cross build 用の header と library の置き場）から読むので、変えたら sysroot の作り直しが要り、これは toolchain の扱い（Q1 の許可）。2026-11-01 のユーザーの答え（WS318 Q318-3 の A）は「2 段で更新: 版を変えずに H.265 decode を選び、次に正式の encode のある 1.3.274 以降の固定の版で」。
- 調べた事: video の宣言（`vulkan_video.h`）は既に Debian の Vulkan-Headers 1.4.309（1.3.274 より新しい固定の版、SHA-256 を記録済み）から選んでいる。H.265 decode も KHR の encode もその入力にある。基本の header（`vulkan_core.h`）は 1.3.269 で、encode の値は BETA（試作）の囲みの中にあるが、値は 1.4.309 の正式の物と全部同じ（51 個を照合）。
- 選択肢:
  - A（推し）: 2 段とも `vulkan_video.h` の選びを 1.4.309 の同じ入力から拡げる（段 1: H.265 decode、段 2: encode と、BETA の値の正式の名前）。`vulkan_core.h` は変えない。得: 影響が video の宣言に閉じる。ユーザーの 2 段の答えに沿う。
  - B: 段 2 で `vulkan_core.h` も 1.4.309 で作り直す。失: 全部の code に差が出て、ABI の照合をやり直す。
- 許可が要る物: header の変更（p002・p003）と、その後の共有の sysroot の作り直し（Q1 が行う）。

### Q324-3 libvulkan の転送と capset の bit の持ち主（Q1）

- 背景: libvulkan（zedBSD の userland の Vulkan の library）は Vulkan の呼び出しを kernel の executor へ送る wire の形に変える。H.265 decode と encode の wire は誰も作っていない。WS322（VisionFive 2 の VPU）の design は「先に着手する方が持つ」（Q322-4）。capset（device が何を扱えるかを libvulkan に伝える表）の新しい bit の割り当ても要る。
- 推し A: WS324 が持つ（p004・p008）。bit 2 = H.265 decode、bit 3 = H.264 encode、bit 4 = H.265 encode（[proposed/](../proposed/gpu-op-video-encode.md)）。WS322 の design の bit の案（bit 3 = H.265 encode）を WS324 の案に揃え、WS322 p005・p010 の libvulkan の部分を取り下げる（PowerVR の reader だけ残る）。

### Q324-4 NVENC の関門と時間の上限（ユーザー: 費用と危険の受け入れ）

- 背景: NVENC の使い方は NVIDIA が文書（MIT の header）で公開しているが、それを使って NVENC を動かす公開の driver は無い（Linux の nouveau・Mesa の NVK も decode だけ）。zedBSD が最初になる。文書には (1) 構造の版が 2 つあり Turing（RTX 20）がどちらを読むか書いていない、(2) 動きの探索・mode の判定・量子化の細かい制御（数百 byte）の「普通の値」が書いていない、という穴がある。試行錯誤で値を決めるので、どれだけ時間が掛かるか読めない。
- 選択肢:
  - A（推し）: 最初に関門の Phase（p009、kernel の中の小さな試験で 64×64 の絵を 1 枚 encode し、host の ffmpeg で decode できるか）を置き、時間の上限を 20 LW（約 5〜8 時間）にする。越えたら止めて Q1 に返し、ユーザーが続けるか決める。H.265 decode の Phase（p002〜p007）は関門と独立に進める。
  - B: 上限を置かずに進める。失: 費用が読めない。
  - C: hardware の encode をやめ、独自の software の encoder（WS329・WS330）に任せる。失: ユーザーの指示（RTX の encode の実装）に反する。
  - 選ばない案: NVIDIA の proprietary の driver の動きを観測して値を取る（license の逆解析の禁止に当たる恐れ）。

### Q324-5 encode を decode と同じ queue family に置くか（Q1、技術）

- 背景: queue family は GPU の命令の受け口の種類。今の nvrtx は 1 つの family に描画・計算・転送・video decode を置く。encode も同じ family に置けば app（libmedia）の device の作り方が簡単。Vulkan の仕様が 1 つの family に decode と encode を両方置くことを許すかは未照合（p008 の最初に読む）。
- 推し A: 同じ family 0。仕様が許さなければ family 1 を encode に分ける（+8 LW）。

### Q324-6 WS324 の受け入れに Video Player の H.265 の再生を入れるか（Q1）

- 背景: ws.md の目標は「libmedia から使えるようにする」。libmedia の Vulkan Video の道は H.264 だけで、H.265 の再生の道と encoder の口は WS318（p020・p021・p022）が作る。WS318 を待たない指示があるので、WS324 では Vulkan を直に使う試験の道具で driver を確かめる。
- 推し A: 入れない。driver の受け入れは試験の道具の hash・PSNR。Video Player の H.265 の再生と libmedia の encoder は WS318 の受け入れ（p020・p026）。

## Resume point

- 2026-11-01 22:30 P1: design.md 第 1 版、proposed/ の案、問い Q324-1〜6。次は design-reviewer の review 1 と反映（design-review.md）、ws.md の Phase の表の案。
