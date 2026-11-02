<!-- awesome-plan project=zedbsd record=ws324-p001 -->

# ws324-p001 調べと設計: RTX 2070 の NVENC（H.264・H.265 encode）と NVDEC（H.265 decode）を nvrtx の Vulkan Video として

Status: cleared（2026-11-03 Q1、ws.md の表のとおり）。前: in-progress（2026-11-01 P1、q1854。design.md 第 2 版（review 1 の反映済み）。Q1 の受け入れと問いの答えが残り）

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

## 結果

- 設計: [design.md](../design.md)（第 2 版）。UAPI の番号と capset の bit の案: [proposed/gpu-op-video-encode.md](../proposed/gpu-op-video-encode.md)。review: [design-review.md](../design-review.md)（31 件、全件の反映の表）。HAL は変えない（design §19）。
- 分かった事の要点:
  - H.265 decode: NVDEC の HEVC の picture の構造と作業の buffer の大きさの式は NVIDIA の open-gpu-doc（MIT）に公開。WS243 の NVDEC の channel と同期の decode の道に、HEVC の作業の buffer・picture の番号の表・行の揃え 64 を足す。filter の buffer の中の並びなどは文書に無く、適合の stream で確かめる。Mesa の NVK の公開の版（26.3.0-rc3）に H.265 は無い。
  - NVENC: open-gpu-doc に class の method と driver との interface（`nvenc_drv.h`）が公開（MIT）。TU106 の class は 0xC4B7。それを駆動する公開の driver は見つからない（zedBSD が最初）。公開の 2 版で H.264・H.265 とも後半の field の位置が 4 byte ずれ、rate control の 4 byte の並びも違う（magic は同じ）。ずれる所を 0 にして使わない。動きの探索などの制御の既定の値は文書に無い。→ channel の確かめ（p009a）と関門（p009b）を先に。
  - firmware: NVDEC と同じく GSP-RM が NVENC の ucode を持つ見込み（未確認、p009a で）。
  - Vulkan の header: ユーザーの決定（2 段）どおり、段 2 で `vulkan_core.h` を 1.4.309（既に pin 済みの入力）で作り直す。1.3.269 の BETA の囲みの 65 個の値は 1.4.309 と全部同じ（host で照合）。
  - libmedia: driver の確かめに libmedia の runtime は要らない（Vulkan を直に使う試験の道具）。要るのは H.265 の解析だけ。
  - timeout の後に止まった engine が書く memory を守る隔離を、NVDEC・NVENC の共通の約束にした（今の H.264 にも無かった）。
- 実機（10.0.10.3）は使っていない（2026-11-01 に Q1 から、P3 が WS291 p027 に使うので触らないよう指示を受けた。この Phase の設計には読むだけの確かめも要らなかった）。
- 記録の注意（Q1 へ）: ws.md の「ユーザーの決定（2026-10-30、原文）」の節はユーザーの発言をそのまま載せている（AGENTS §8 の原文を載せない規則）。Q1 が書いた節なので P1 は変えていない。

## 判断を待つ点

前提: WS324 は RTX 2070（NVIDIA の Turing 世代の GPU、chip は TU106、amd64 の PC の 10.0.10.3 に挿してある）の video の hardware を、zedBSD の kernel の中の Vulkan の実装（nvrtx の executor）の **Vulkan Video**（Vulkan の video の decode・encode の拡張。zedBSD では app が video の hardware を使う唯一の口）として見せる WS。H.264 の decode は WS243 で済み、残りは H.265（HEVC）の decode と、H.264・H.265 の encode。**NVDEC** は NVIDIA の video の decoder の engine、**NVENC** は encoder の engine。どちらも GPU の中の独立の engine で、NVIDIA の firmware（GSP-RM: GPU の中の管理用の processor で動く NVIDIA の資源管理の program）を通して使う。今の状態は設計の第 2 版（敵対的レビューの反映済み）で、code は書いていない。

### Q324-1 H.265 の解析をどこに作るか（Q1）

- 背景: Vulkan Video の decode は stateless で、app（か libmedia）が bitstream を解析して H.265 の parameter set（VPS・SPS・PPS: 動画全体・絵ごとの設定）と slice header の値を Vulkan の構造（StdVideoH265…）にして driver に渡す。driver の試験にも、この解析が要る。libmedia の H.265 の解析は WS318 p016〜p019（標準廉価実装手順で Sonnet が作る予定）の範囲で、まだ無い。
- 選択肢:
  - A（推し）: WS318 p016〜p019 の範囲を、標準廉価実装手順の形のまま（設計の Phase p006a を Opus、実装の 3 本 p006b〜d を Sonnet で 1 本ずつ）WS324 で先に作る。置き場は libmedia の独立の module（`h265*.c`。libmedia の build と runtime には繋がない）、適合の stream の cache は WS318 と同じ `build/sources/h265-conformance/`。試験の道具（`vkvideo-probe`）がそれを直に compile する。WS318 は後でそれを runtime（p020）に繋ぐ。得: 解析を一度だけ書く、WS318 の手間も減る。失: WS318 の Phase を前借りし、probe が libmedia の内部の file を compile する形になるので、WS318 の design の H.265 の節と所有の記録を直す必要（Q1）。
  - B: 試験の道具の中に試験だけの解析を作る（今の `vkvideo-probe` の H.264 と同じ形）。得: WS318 に触らない。失: 約 2500 行の重複。
  - C: WS318 p016〜p019 を待つ。失: ユーザーの指示（WS318 を待たない）に反する。

### Q324-2 Vulkan の header の選び直し・作り直しと sysroot の作り直し（Q1、toolchain の許可）

- 背景: zedBSD の Vulkan の header（`include/libc/vulkan/`）は Khronos の宣言から必要な物だけを選んで作る。今は H.264 の decode だけ。H.265 decode と encode の宣言を足す必要がある。header は target の build が共有の sysroot（cross build 用の header と library の置き場）から読むので、変えたら sysroot の作り直しが要り、これは toolchain の扱い（Q1 の許可）。2026-11-01 のユーザーの答え（WS318 Q318-3 の A）は、2 段で更新する（版を変えずに H.265 decode を選び直し、次に正式の encode のある 1.3.274 以降の固定の版で作り直す）という趣旨。
- 推し A: ユーザーの答えどおり 2 段（p002: H.265 decode の選び、p003: `vulkan_core.h` の作り直しと encode の選び）。段 2 の固定の版は Debian の Vulkan-Headers 1.4.309（`vulkan_video.h` の入力として既に SHA-256 を記録済みで、host にある）。1.3.269 の試作（BETA）の囲みの値 65 個は 1.4.309 の正式の値と全部同じ（照合済み）。
- 第 1 版で推した「`vulkan_core.h` を変えずに別名を足す」案は、C として成り立たず（再定義、`case` に使えない）、ユーザーの答えとも違うので取り下げた（review 1）。
- 許可が要る物: header の変更（p002・p003）と、その後の共有の sysroot の作り直し（Q1 が行う）。

### Q324-3 libvulkan の転送と capset の bit・命令の番号の持ち主（Q1）

- 背景: libvulkan（zedBSD の userland の Vulkan の library）は Vulkan の呼び出しを kernel の executor へ送る wire の形に変える。H.265 decode と encode の wire は誰も作っていない。WS322（VisionFive 2 の VPU）の design は「先に着手する方が持つ」（Q322-4）、WS318 §10.3 は WS319（i915）を想定。capset（device が何を扱えるかを libvulkan に伝える表）の新しい bit と GPU の命令の番号（UAPI、全部の executor の台帳）の割り当ても要る。
- 推し A: WS324 が持つ（p004・p008a・p008b）。bit 2 = H.265 decode、bit 3 = H.264 encode、bit 4 = H.265 encode、命令の番号は reply 付き 0x10010〜・記録 0x10020〜（[proposed/](../proposed/gpu-op-video-encode.md)）。WS322 の design の bit の案（bit 3 = H.265 encode）を WS324 の案に揃え、WS322 p005・p010 の libvulkan の部分を取り下げ（PowerVR の reader だけ残る）、WS318 §10.3・WS319 の記録も直す。

### Q324-4 NVENC の関門と時間の上限（ユーザー: 費用と危険の受け入れ）

- 背景: NVENC の使い方は NVIDIA が文書（MIT の header）で公開しているが、それを使って NVENC を動かす公開の driver は無い（Linux の nouveau・Mesa の NVK も decode だけ）。zedBSD が最初になる。文書には (1) 構造の版が 2 つあり Turing（RTX 20）がどちらを読むか書いていない、(2) 動きの探索・mode の判定・量子化の細かい制御（数百 byte）の「普通の値」が書いていない、という穴がある。試行錯誤で値を決めるので、どれだけ時間が掛かるか読めない。失敗すると engine が止まり、1 回ごとに実機の再起動（Q1 の電源の入れ直し）が要る。
- 選択肢:
  - A（推し）: 先に channel の確かめ（p009a、8 LW）、次に関門（p009b: kernel の中の自己診断で 256×128 の絵を I・P の 2 枚 encode し、host の ffmpeg で decode できるか）を置き、関門の時間の上限を p009a の後から 20 LW（約 5〜8 時間）にする。版・設定は boot の引数で切り替え、build のやり直しを減らす。越えたら止めて Q1 に返し、ユーザーが続けるか決める。H.265 decode の Phase（p002〜p007）は関門と独立に進める。
  - B: 上限を置かずに進める。失: 費用が読めない。
  - C: hardware の encode をやめ、独自の software の encoder（WS329・WS330）に任せる。失: ユーザーの指示（RTX の encode の実装）に反する。
  - 選ばない案: NVIDIA の proprietary の driver の動きを観測して値を取る（license の逆解析の禁止に当たる恐れ）。

### Q324-5 encode を decode と同じ queue family に置くか（Q1、技術）

- 背景: queue family は GPU の命令の受け口の種類。今の nvrtx は 1 つの family に描画・計算・転送・video decode を置く。encode も同じ family に置けば app（libmedia）の device の作り方が簡単。Vulkan の仕様の該当の頁（`VkQueueFamilyVideoPropertiesKHR`）に、1 つの family に decode と encode を置くことを禁じる文は無い（review 1 で確かめた）。
- 推し A: 同じ family 0。他の規則で駄目と分かれば family 1 を encode に分ける（+8 LW、確率は低い）。

### Q324-6 WS324 の完了の条件に libmedia の道を残すか（ユーザー: 範囲）

- 背景: ws.md の目標は「libmedia から使えるようにする」（2026-10-30 のユーザーの決定から）。libmedia の Vulkan Video の道は H.264 だけで、H.265 の再生の道と encoder の口は WS318（p020・p021・p022）が作る。WS318 を待たない指示があるので、WS324 の driver の確かめは Vulkan を直に使う試験の道具で行う。問いは、WS318 の物が出来た後の「RTX で libmedia から使えることの確かめ」を WS324 の完了の条件に残すか。
- 選択肢:
  - A（推し）: 残す。最後に p016（WS318 p020・p022 の後に、Video Player の H.265 の再生と libmedia の encoder の往復を RTX で確かめる）を置き、WS324 はそれまで incomplete。目標を変えない。
  - B: 外す。driver の受け入れは試験の道具の hash・PSNR だけにし、libmedia での確かめは WS318 の受け入れ（p026）に移す。得: WS324 が早く completed になる。失: WS243 で「libmedia・Video Player は変えない」と設計して実機で範囲が足りなかった前例がある。

## Resume point

- 2026-11-01 P1: design.md 第 2 版（review 1 の 31 件を反映）、proposed/ の案、問い Q324-1〜6。次は Q1 の受け入れと問いの答え。答えの後に ws.md の Phase の表を確定し、p002（Q1 の許可）・p009a（実機の割り当て）から。
