<!-- awesome-plan project=zedbsd record=ws306 -->

# WS306 GPU の driver の枠組みの「ゲーム最適化」の UAPI と driver の識別

- Status: planning（2026-10-27 新設。WS291 の RDNA の最適化の完了の後に始める）
- Master: [plan/master.md](../master.md)。Primary Milestone: MG006。関係: [WS291](../ws291/ws.md)（RDNA の NGG の culling を、この flag の時だけ入れる）、[WS226](../ws226/ws.md)（Vulkan 1.3 相当と拡張の capability。VK_EXT_mesh_shader の task・mesh の feature はそちら）。

## 目標

`/dev/gpu`（GPU の driver の枠組み）に、session（process）ごとの「ゲーム最適化」の UAPI を足す。ON/OFF に加えて、細かい option の ON/OFF を明示する文字列も渡せる。GPU の名前と driver の版をアプリが取れるようにし、ゲームが driver を見て option を選び、driver の不具合を避けながら互換性をゲームの側で制御できるようにする。全 GPU の driver（amdrdna・NVIDIA・PowerVR など）が同じ UAPI で自分の最適化を切り替える。

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

| Phase | 内容 | 依存 | 状態 |
| --- | --- | --- | --- |
| p001 | 設計: UAPI（session の flag・option の文字列・option の一覧・名前と版の取得）、libvulkan が読む環境変数の名前と書式、知らない option の扱い、各 driver の option の表（amdrdna の NGG culling から）、vulkaninfo への一覧の出し方、design-reviewer | — | in-progress（[design.md](design.md) 第 1 版 2026-10-29 P5、問いは回答済み、design-reviewer 待ち） |
| p002〜 | 実装: UAPI と gpu の枠組み、libvulkan、amdrdna の option（NGG culling）、vulkaninfo の移植（userland/packages、ライセンスの監査）。p001 で分ける | p001 | planning |

## Resume point

- 2026-10-27: 新設。使用量が戻ったら p001 の設計から。
- 2026-10-28 確かめ: 2026-10-28 ユーザー「P2はRDNAマシンで作業します。RDNAの高速化をお願いします。まずNGGなどの高速化を取り入れてLinuxと同等以上の描画速度を達成して、そのあとで、ゲーム用最適化をオプションに分けていく段取りになっているので、確認してください。」 → 上の順のまま（WS291 で Linux 以上 → WS306）。

## ユーザーの回答（2026-10-29、Q306-6）

- 原文: 「デスクトップではすべて入れません。ゲームはレンダリングの内容が大きく変わらないものを中心に推しでデフォルトを決めます。bootの行というのはなんでしょうか。カーネルではなく/bin/waylandですか？」
- Q1 の解釈（確かめ中）: desktop（ゲーム最適化が OFF の session）には描画の最適化の option を入れない。ゲーム（ON）の option の既定は、描画の内容が大きく変わらない物を中心に推しで決める。「boot の行」は loader の cfg の kernel の引数（例 amdrdna.vk.ngg、amdrdna の driver が読む）だとユーザーに説明した。

## ユーザーの回答（2026-10-29、Q306-6 の確かめ）

- 問い（Q1）: WS291 の案 4・5（描画の内容を変えない投入の経路の改善）を desktop を含む全部に効かせてよいか。
- 原文: 「まずはデフォルト有効で実装して、NGGはゲーム用に作り替えます。つまりWSに依存関係があります。」
- Q1 の解釈: 案 4・5 は既定で全部に有効（option にしない）。NGG（と culling）はゲーム用（ゲーム最適化が ON の session の option）に作り替える。この作り替えは WS291 の成果の上で行うので、WS306 の NGG の段は WS291 に依存する（WS291 の NVIDIA の段の前後の順は Q1 が決める）。

- 2026-10-31 ユーザーから、WS306 は検討済みなので進め、決定事項が不明ならまた聞く（cockpit.md に書けば答える）、という趣旨の指示を受けた（q1746、P5 が WS333 p001 の後に p001 の設計）。
