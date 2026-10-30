<!-- awesome-plan project=zedbsd record=ws306 -->

# WS306 GPU の driver の枠組みの「ゲーム最適化」の UAPI と driver の識別

- Status: planning（2026-10-27 新設。p001 の設計は第 3 版（2026-10-31）。WS291 の RDNA の最適化に依存するのは p007 だけで、他の Phase を先に進めるかは Q306-9）
- Master: [plan/master.md](../master.md)。Primary Milestone: MG006。関係: [WS291](../ws291/ws.md)（RDNA の NGG の culling を、この flag の時だけ入れる）、[WS226](../ws226/ws.md)（Vulkan 1.3 相当と拡張の capability。VK_EXT_mesh_shader の task・mesh の feature はそちら）。

## 目標

`/dev/gpu`（GPU の driver の枠組み）に、session（`/dev/gpuN` の open ごと。dup・fork で共有し、1 process の 2 つの VkInstance は別の session）ごとの「ゲーム最適化」の UAPI を足す。ON/OFF に加えて、細かい option の ON/OFF を明示する文字列も渡せる。GPU の名前と driver の版をアプリが取れるようにし、ゲームが driver を見て option を選び、driver の不具合を避けながら互換性をゲームの側で制御できるようにする。全 GPU の driver（amdrdna・NVIDIA・PowerVR など）が同じ UAPI で自分の最適化を切り替える。

## ユーザーの決定（原文）

- 2026-10-27 ユーザー「GPUドライバフレームワークに、ゲーム最適化、というUAPIを追加して、このゲーム最適化がかかっているときだけ、NGGをmesh shader以外でも有効にするのが、私のセンスでは正しいような気がします。」
- 2026-10-27 18:25 ユーザー「では、ゲーム最適化はON/OFFの他に、細かいオプション群のON/OFFを明示指定もできる文字列も渡せるようにしましょう。あと、GPUの名前やドライバのバージョンも取得できるようにしましょう。これにより、ゲームがドライバを見て、細かいオプションを制御できるようになります。ドライバのバグを回避しつつゲームの互換性をゲーム側が制御できるという点では、Windowsより優れたものにできるかもしれません。」
- 2026-10-27 ユーザー「ゲーム最適化の細かいオプションの一覧もUAPIで取得できるようにしましょう。」→ option の一覧（名前・説明・既定値など）を UAPI で取れるようにする。Vulkan の core にはこの仕組みが無い（Q1 の説明: 近い物は VK_EXT_layer_settings（layer 向けの設定の受け渡し）、VkApplicationInfo の app・engine の名前による driver 側の profile、VK_KHR_driver_properties、VK_AMD_rasterization_order のような個別の拡張）ので、Vulkan への見せ方は zedBSD の独自の拡張を設計で考える。
- 2026-10-27 ユーザー「それでは、VulkanのAPIではなくて、環境変数をlibvulkanが読んで、UAPIを叩く形にしましょう。これにより、ゲームがゲーム最適化のON/OFFと個別ゲーム最適化オプションのON/OFFを制御できます。また、vulkanの情報を出力する標準のコマンドってありますか？そこにゲーム最適化オプションの一覧を出せばいいと思います。」→ 指定は環境変数（libvulkan が読んで UAPI を叩く。Vulkan の独自の拡張は作らない）。一覧の表示は Khronos の標準のコマンド `vulkaninfo`（Vulkan-Tools）に出す。2026-10-27 の時点で zedBSD の tree に vulkaninfo は無い（`grep -rli vulkaninfo userland` が 0 件）ので、userland/packages に移植する（tarball と patch、ライセンスは Apache-2.0 の見込みで監査が要る）。
- WS291 との順（2026-10-27 ユーザー「WS306ですが、AMD RDNAドライバの最適化をWS291でやりきったあとに、要素の最適化をオプションに移して実装し直す作業という認識なので、両方のWSに記録してください。」）→ WS306 は [WS291](../ws291/ws.md) で AMD RDNA の最適化をやりきった後に始める。WS291 で入れた最適化の要素（NGG の culling など）を option に移して実装し直す作業。
- 同じ日の task・mesh shader: VK_EXT_mesh_shader（task と mesh は同じ拡張の 2 つの feature）で出す。ゲーム最適化とは関係なく、使える GPU では常に出す（Q1 の説明にユーザーが「はい」の流れ。詳細は WS226 で）。

- 2026-10-29 ユーザーの回答（クリック、Q1 の中継、設計 [design.md](design.md) §9 の問い）: Q306-1「VULKAN_ZEDBSD_GAME (推し)」、Q306-2「+name/-name (推し)」、Q306-3「無視して数を返す (推し)」、Q306-4「libvulkan の C の関数 (推し)」。Q306-5 は Q1 の問いの文「変数が無い時は OFF…推しで進めます。違う場合は Other で」にユーザーが Other を選ばなかった。
  → 解釈（P5）: 環境変数は `VULKAN_ZEDBSD_GAME`（0/1）と `VULKAN_ZEDBSD_GAME_OPTIONS`、option の文字列は「,」区切りの `+name`／`-name`、表に無い名前は無視して数を返す（文法の誤りだけ拒否）、一覧は libvulkan の zedBSD の C の関数（Vulkan の拡張ではない）を patch した vulkaninfo とゲームが呼ぶ、変数が無い時は OFF。

## Q1 の解釈（未確定、設計の Phase で決める）

- RDNA: ゲーム最適化が ON の session の pipeline だけを culling 入りの NGG で compile する（三角形の少ない描画では culling を飛ばす判断は残す）。OFF の時は passthrough の NGG か従来の経路（desktop の frame-ops を測って決める）。
- 名前と版は Vulkan の `VkPhysicalDeviceProperties`（deviceName・driverVersion）と `VkPhysicalDeviceDriverProperties`（driverName・driverInfo）にも写す。
- 有効にする方法（決定済み、上の 2026-10-27 の原文）: 環境変数を libvulkan が読み、session の flag と option の文字列を UAPI で kernel に渡す。環境変数の名前と option の文字列の書式は p001 で決める。
- option の文字列は kernel で長さと書式を検査し、知らない option は無視か拒否（設計で決める）。

## Phase

設計は [design.md](design.md)（第 3 版、§13 が Phase の表の本体）。ユーザーに聞く点は [user-questions.md](user-questions.md)（Q306-7・Q306-9、Q306-8 は取り下げ）。
標準廉価実装手順は、条件 1（外部の標準仕様）を満たす部分が無いので WS306 では使わない（design §13）。

| Phase | 内容 | 依存 | 状態 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: UAPI・latch・driver の口・識別・libvulkan の変数と関数・vulkaninfo の移植と license の監査の手順・Phase の分け方、design-reviewer 2 回 | — | in-progress（2026-10-31 P5 q1746-i01: 第 3 版と第 2 回のレビューの反映を commit、Q1 の判定待ち） |
| p002 | `include/uapi/gpu-game.h`・`include/drivers/gpu/gpu-game.h`・`gpu-game.c`（解読と表の検査）と host の単体試験 | p001 | planning |
| p003 | 核: gpu.c の 3 つの ioctl・latch・登録の検査・interface の版 10・venus の識別、host の模型、guest の試験、QEMU | p002 | planning |
| p004 | 全 driver の識別（gpu_name・版、COMMAND と同じ所で GAME の bit） | p003 | planning |
| p005 | libvulkan（環境変数・SET・2 つの関数・`vulkan_zedbsd.h`・exports.map） | p003 | planning |
| p006 | vulkaninfo の下見（vulkan-headers・vulkan-tools の package、license の監査、patch なしの実行。libcxx の build は Q1 の許可） | p002 | planning |
| p007 | amdrdna の option（`ngg`・`ngg_cull`、WS291 の要素の分類）、実機の測り | p003、p004、WS291 の RDNA の完了、Q306-7 | planning |
| p008 | vulkaninfo の patch と image への組み込み | p005、p006 | planning |
| p009 | 文書（枠組み） | p005、p008 | planning |
| p010 | 文書（amdrdna の option） | p007、p009 | planning |
| p011 | 規約の全文の見直し | 全部 | planning |

## Resume point

- 2026-10-27: 新設。使用量が戻ったら p001 の設計から。
- 2026-10-31 P5（q1746-i01）: design.md を main 9946e22c0 に照らして第 3 版に書き直し、design-reviewer の第 2 回（Blocker 0・Major 7・Minor 13）を全部反映した。次は Q1 の p001 の判定と Q306-7・Q306-9 の回答。推しのままなら p002 から（p007 だけ WS291 の後）。
- 2026-10-28 確かめ: 2026-10-28 ユーザー「P2はRDNAマシンで作業します。RDNAの高速化をお願いします。まずNGGなどの高速化を取り入れてLinuxと同等以上の描画速度を達成して、そのあとで、ゲーム用最適化をオプションに分けていく段取りになっているので、確認してください。」 → 上の順のまま（WS291 で Linux 以上 → WS306）。

## ユーザーの回答（2026-10-29、Q306-6）

- 原文: 「デスクトップではすべて入れません。ゲームはレンダリングの内容が大きく変わらないものを中心に推しでデフォルトを決めます。bootの行というのはなんでしょうか。カーネルではなく/bin/waylandですか？」
- Q1 の解釈（確かめ中）: desktop（ゲーム最適化が OFF の session）には描画の最適化の option を入れない。ゲーム（ON）の option の既定は、描画の内容が大きく変わらない物を中心に推しで決める。「boot の行」は loader の cfg の kernel の引数（例 amdrdna.vk.ngg、amdrdna の driver が読む）だとユーザーに説明した。

## ユーザーの回答（2026-10-29、Q306-6 の確かめ）

- 問い（Q1）: WS291 の案 4・5（描画の内容を変えない投入の経路の改善）を desktop を含む全部に効かせてよいか。
- 原文: 「まずはデフォルト有効で実装して、NGGはゲーム用に作り替えます。つまりWSに依存関係があります。」
- Q1 の解釈: 案 4・5 は既定で全部に有効（option にしない）。NGG（と culling）はゲーム用（ゲーム最適化が ON の session の option）に作り替える。この作り替えは WS291 の成果の上で行うので、WS306 の NGG の段は WS291 に依存する（WS291 の NVIDIA の段の前後の順は Q1 が決める）。

- 2026-10-31 ユーザーから、WS306 は検討済みなので進め、決定事項が不明ならまた聞く（cockpit.md に書けば答える）、という趣旨の指示を受けた（q1746、P5 が WS333 p001 の後に p001 の設計）。
