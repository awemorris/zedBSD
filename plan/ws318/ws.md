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
- backend: hardware は Vulkan Video の 1 本（VPU が別の機械も kernel の Vulkan の executor が Vulkan Video として見せる）→ 無ければ libavcodec を dlopen（無ければ MEDIA_PROBLEM_MISSING）。decode も encode も同じ。
- app の Vulkan の device を共有: `media_vulkan_requirements()` の類で要る拡張・feature・queue family を問い、app が作った instance・physical device・device・queue を渡す。結果は VkImage・形式・layout・semaphore。fd・HANDLE は API に出さない。Vulkan を使わない app には CPU の picture の道を残す。
- 形式を前提にしない（NV12 を捨てる）: decoder・encoder・カメラの形式を実行の時に問い、合わなければ GPU で 1 回変換（compute）。MJPEG のカメラは decode の段を挟む。
- encoder（`media_encoder_*` の類）: 対応する codec と profile・大きさ・rate control を問う口、codec を指定して開く口、picture を送り packet を受ける。codec は H.264・H.265・AAC-LC（AAC は既存の独自の encoder（WS205、短い窓・TNS・PNS 無し）を encoder の口に載せ、profile はデコーダと同じ AAC-LC、既存のライブラリは使わず独自に）。
- カメラ（`media_camera_*` の類）: libmedia が共有の device に作った画像にカメラが直接書く（CAM_MEMORY_IMPORT、i915 などの coherent な memory に条件を広げる）。frame は decoder と同じ picture の型、encode と表示が同じ画像を使い、参照の数で返す。UVC の実機の動作は WS282 の再開が前提。
- 起こしは callback（app は中で kl_app_post）、`media_engine_wake_fd` を外す。libmedia の仕事は名前の付いた thread（WS309 の方針）。
- 基準の機械は i915（5330）。
- H.265 について: WS236（2026-10-14）のユーザーの決定「HEVCはライセンスがないのでやらないです」と、今回の「H.264とH.265のデコーダとエンコーダ」の関係はユーザーに確認中（Q1、2026-10-30）。確認まで H.265 の Phase は planning のまま始めない。

## Phase（案、設計で置き換える）

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws318-p001 | 設計（design.md）: API（共有の device・codec の問い合わせと指定・decoder・encoder・カメラ・picture・callback）、backend の表と fallback、形式の変換、thread、移行の手順。design-reviewer | planning | — |
| ws318-p002 | libavcodec の fallback を libmedia の中へ移し、Keiland の app を libmedia だけに（今の機能のまま入り口を揃える） | planning | p001 |
| ws318-p003 | Vulkan の device の共有と fd を API から外す（decoder の picture・engine の起こし） | planning | p001 |
| ws318-p004 | encoder の口（codec の問い合わせと指定）、libavcodec の encode の fallback、AAC-LC の encoder を口に載せ足りない所を独自に | planning | p003 |
| ws318-p005 | カメラの口（WS282 の UVC が実機で動いた後に実機の試験） | planning | p003、WS282 |
| ws318-p006 | Vulkan Video の encode の backend（i915 の driver の WS319 の前は Venus か software の検証で） | planning | p004 |
| ws318-p007 | 解説（docs）と規約の全文の見直し | planning | p002〜p006 |

## Resume point

- 2026-10-30 Q1: 作成。担当は未割当。
