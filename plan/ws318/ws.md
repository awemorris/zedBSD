<!-- awesome-plan project=zedbsd record=ws318 -->

# WS318 libmedia の作り直し: Vulkan の device の共有・codec の問い合わせと指定・encoder・カメラ・libavcodec の内部の fallback、Keiland の app を libmedia だけに

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: planning（2026-10-30 Q1 が作成、担当は未割当）
- 目標: libmedia を Keiland の app の唯一の media の入り口にし、hardware は Vulkan Video の 1 本、無ければ libmedia の中で libavcodec を dlopen して使う。decoder・encoder（H.264・H.265・AAC-LC）・カメラを、app と共有する Vulkan の device の上で、fd を API に見せずに扱う。
  個別の GPU・VPU の Vulkan Video の driver は WS319〜WS325（この WS の completed の後）。

## ユーザーの決定（2026-10-30、原文）

1. 「H.264のエンコーダの口をlibmediaに足したいです。カメラ入力からハードウェアエンコーダ（Vulkan Video）まで、Unified Memoryであればゼロコピーでいけるようにしたいです。ということは、libmediaにカメラの口を持たせるのがいいと思いました。」
2. 「libmediaは、fdではなくて、Vulkanのバッファをそのまま指定できればいいんじゃないでしょうか。…この実装なら、Windowsでもfd以外の方法でそのAPIを実装できるように思うのですが。」→ Q1 の案「app の Vulkan device を libmedia と共有する（推し）」に「賛成です」。
3. 「NV12という前提は捨てます。i915をベースラインにするのがいいと思います。」
4. 「ハードウェアエンコーダが利用できないとき、libmediaが内部的にlibavcodecの呼び出しにフォールバックする設計にしてください。既存のKeiland Appsは、libmediaのみを入り口に変更してlibavcodec呼び出しをやめ、libmediaのデコーダが、ハードウェアデコーダがないときに、libavcodecが存在すればdlopenして利用する形に改めます。」
5. 「Pi 4は、たしかVulkan VideoとしてH.264 デコーダをがんばって実装したように記憶していますが…VPUが独立だとしても、我々はVulkan Video driverとして見せる必要があります。それがOSのAPIなので。」
6. 「ありがとうございます。では、ここまでで合意したlibmediaの設計を実装して、H.264とH.265のデコーダとエンコーダの表の中で未の部分を埋めるようにVulkan Video driverを実装して、Appsをlibmedia使用に書き換える、というのを、WSとして立ててください。ドライバは個別のWSに分けて依存関係にしてもかまいません。先にlibmediaの方を作りきってcompleteにしてから、個別のドライバを実装する流れにします。また、libmediaのエンコーダには、codecの対応を調べたり、codecを指定したりする機能を入れます。AACのデコードはlibmediaにすでに入っていると思いますが、追加で、AACのエンコードも実装します。既存のライブラリは使用せず、独自に実装します。プロファイルはデコーダと同様でOKです。」（2026-10-30）

## 設計の合意（Q1 のまとめ、2026-10-30。上の原文が正）

- 入り口は libmedia だけ。今の `userland/desktop/media-app/`（app の中の libavcodec の fallback）は libmedia の中へ移し、Video Player・Music・Browser の `<video>` などは libmedia だけを呼ぶ。
- backend: hardware は Vulkan Video の 1 本（VPU が別の機械も kernel の Vulkan の executor が Vulkan Video として見せる）→ 独自の software の codec（H.264 の decode は [WS326](../ws326/ws.md)、AAC-LC）→ libavcodec を dlopen（無ければ MEDIA_PROBLEM_MISSING）。2026-10-30 ユーザー「libmedia の backend の順番は、Vulkan Video --> 独自ソフトウェアdecoder --> libavcodecです。」
- app の Vulkan の device を共有: `media_vulkan_requirements()` の類で要る拡張・feature・queue family を問い、app が作った instance・physical device・device・queue を渡す。結果は VkImage・形式・layout・semaphore。fd・HANDLE は API に出さない。Vulkan を使わない app には CPU の picture の道を残す。
- 形式を前提にしない（NV12 を捨てる）: decoder・encoder・カメラの形式を実行の時に問い、合わなければ GPU で 1 回変換（compute）。MJPEG のカメラは decode の段を挟む。
- encoder（`media_encoder_*` の類）: 対応する codec と profile・大きさ・rate control を問う口、codec を指定して開く口、picture を送り packet を受ける。codec は H.264・H.265・AAC-LC（AAC は既存の独自の encoder（WS205、短い窓・TNS・PNS 無し）を encoder の口に載せ、profile はデコーダと同じ AAC-LC、既存のライブラリは使わず独自に）。
- カメラ（`media_camera_*` の類）: libmedia が共有の device に作った画像にカメラが直接書く（CAM_MEMORY_IMPORT、i915 などの coherent な memory に条件を広げる）。frame は decoder と同じ picture の型、encode と表示が同じ画像を使い、参照の数で返す。UVC の実機の動作は WS282 の再開が前提。
- 起こしは callback（app は中で kl_app_post）、`media_engine_wake_fd` を外す。libmedia の仕事は名前の付いた thread（WS309 の方針）。
- 基準の機械は i915（5330）。
- H.265（2026-10-30 ユーザー「H.265のパテントプールへの支払いは行う方針です。H.265の実装を行っていいです。」）: WS236（2026-10-14）の「HEVCはライセンスがないのでやらないです」を置き換え、H.265 の decode・encode を実装する。

## Phase（p001 の設計の案、2026-10-31 P5。詳細と受け入れは [design.md](design.md) §14）

設計は [design.md](design.md)、公開の interface の案は [proposed/](proposed/)、ユーザーの判断を待つ点は [user-questions.md](user-questions.md)（Q318-1〜14）、敵対的レビューは [design-review.md](design-review.md)。型: O = Opus、S = Sonnet 5.5 high（標準廉価実装手順）、D = 文書・規約。

| Phase | 目的 | 型 | 状態 | 依存 |
| --- | --- | --- | --- | --- |
| ws318-p001 | 設計（design.md・proposed の header・問い・design-reviewer） | O | in-progress（q1752 P5。設計と review の反映は済み、Q1 の受け入れと Q318 の回答待ち） | — |
| ws318-p002 | 公開の header（media.h・media-vulkan.h）・内部の header・骨格・exports の名前の列・run-unit と check-skeleton | O | planning | p001 |
| ws318-p003a | vkfake の核（代わりの libvulkan、lavapipe、1.0 と許可の表、video の family と caps） | O | planning | p002 |
| ws318-p003b | vkfake の video（session・parameter・decode の模倣と記録、今の runtime を通す） | O | planning | p003a |
| ws318-p004 | libavcodec の decode を libmedia の中へ、Video Player・Music を libmedia だけに、media-app を外す（今の機能のまま） | O | planning | WS326 p023 |
| ws318-p005 | context と device の共有の核（NONE・PRIVATE・SHARED、requirements、queue の lock、旧い口の shim） | O | planning | p003b、p004 |
| ws318-p006a | frame の model と ops v2（参照の数・identity・staging、backend と media-probe・host-codec の追従） | O | planning | p005、WS326 completed |
| ws318-p006b | hardware の decode の VkImage の出力（DISTINCT・別名・i915 の transfer の道） | O | planning | p006a |
| ws318-p007a | 変換の描画の pass の核（shader の生成、yuv420-to-rgb、交渉） | O | planning | p006b |
| ws318-p007b | 残りの変換（packed422・rgb-to-yuv420・readback）と色の係数 | O（S） | planning | p007a |
| ws318-p008 | thread の台帳と notify の callback、engine と libbrowser | O | planning | p005 |
| ws318-p009 | Video Player を共有の device と plane の frame へ | O | planning | p007a、p008 |
| ws318-p010 | Music・media-probe・libbrowser を新しい口へ、旧い口と media-decoder.h を消す | O | planning | p009 |
| ws318-p011a | Vulkan の header に H.265 の decode を選び直す（版はそのまま）、sysroot は Q1 | O | planning | Q318-3 |
| ws318-p011b | Vulkan の header を KHR の encode のある版へ、sysroot は Q1 | O | planning | Q318-3 |
| ws318-p012a | encoder の核・codec の問い合わせ・packet の枠・AAC-LC の backend | O | planning | p007b |
| ws318-p012b | libavcodec の encode の add-in | O | planning | p012a |
| ws318-p013 | AAC の追加（窓の切り替え・TNS）の設計（標準廉価、判定を数値まで固定） | O | planning | p012a、Q318-5 |
| ws318-p014 | AAC: 窓の切り替えと短い窓 | S | planning | p013 |
| ws318-p015 | AAC: TNS | S | planning | p014 |
| ws318-p016 | H.265 の Vulkan Video の decode の設計（標準廉価） | O | planning | p011a |
| ws318-p017 | H.265: NAL・VPS・SPS | S | planning | p016 |
| ws318-p018 | H.265: PPS・slice segment header | S | planning | p017 |
| ws318-p019 | H.265: POC・RPS・DPB | S | planning | p018 |
| ws318-p020 | H.265 の decode の runtime（vkfake、適合の stream の解析） | O | planning | p006b、p019 |
| ws318-p021 | Vulkan Video の encode（H.264） | O | planning | p011b、p012a |
| ws318-p022 | Vulkan Video の encode（H.265） | O | planning | p021、p017 |
| ws318-p023a | camera の層（slot の状態・待ちの列・notify）と偽の source の試験 | O | planning | p007b、p012a |
| ws318-p023b | camera の zedBSD の source（DEVICE・IMPORT、OS の境界の checker） | O | planning | p023a |
| ws318-p024 | MJPEG の段 | O（S） | planning | p023a、Q318-4 |
| ws318-p025 | kernel: camera の import を i915 の coherent な memory へ（HAL は変えない） | O | planning | p023b |
| ws318-p026 | 実機の受け入れ（5330 の Video Player、RTX・Pi 4 の非退行、QEMU Venus） | O | planning | p010、p020 |
| ws318-p027 | camera の実機（5330 ＋ webcam） | O | planning | p023b〜p025、WS282 の実機、Q318-6 |
| ws318-p028 | 解説の文書と規約の全文の見直し | D | planning | 全部 |

## Resume point

- 2026-10-30 Q1: 作成。担当は未割当。
- 2026-10-31 P5（q1752-i01）: p001 の設計を書き（design.md・proposed/・user-questions.md）、design-reviewer の 37 件（blocker 3）を反映した（design-review.md）。次: Q1 が受け入れ、ユーザーが Q318-1〜14 に答えた後に p002 から。WS326 に触れない Phase を先に始めるかは Q318-14。

## 日程（2026-10-30 ユーザー、原文）

「WS318はH.264デコーダのあとにすぐスケジュールします。」→ WS326 の completed の直後
