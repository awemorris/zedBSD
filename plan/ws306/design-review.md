# WS306 p001 の設計の敵対的レビュー（design-reviewer、2026-10-29）

対象: [design.md](design.md) 第 1 版（worktree `agent/p5-design`。レビューの間に HEAD は f6aae6221 → dd607b017 と動いた。行の照合は f6aae6221 の tree で行った）。
読んだ物: AGENTS.md、plan/guardrail.md、plan/ws306/ws.md、plan/ws291/ws.md、include/uapi/gpu*.h・ioctl.h・backlight.h、include/drivers/gpu/gpu.h、
src/drivers/gpu/gpu.c（session・ops の検査・GET_INFO・dispatch・open の権限・native_submitted の 3 か所・stop の判定）、
src/drivers/gpu/amdrdna/{vk-node.c,node.c,amdrdna-node.h,amdrdna-hw.h}・vk/{amdrdna-vk.h,vk-pipeline.c,vk-execute.c,vk-device.c,vk-layout.c}、
nvrtx/vk/vk-device.c、各 driver の `driver_name` と `DRV_GPU_INTERFACE_VERSION` の所、venus/venus.c の ops、
userland/desktop/libvulkan/{context.c,instance.c,wsi-display.c,dispatch.c,exports.map,Makefile}、include/libc/vulkan/、
plan/ws009/phase009/phase.md、plan/ws309/design.md（game mode の語）、plan/ws014/tests/gpu-uapi-layout.c（有無だけ）。

## 確かめて合っていた事

- 'G' の番号: 0〜45 の 46 個、重なり・抜け無し（全 header の `_IO*('G', n` を数えた）。46〜48 は空き。他の WS の plan にも 46 以降と 131072 の予約は無い（main の plan/ を grep）。
- capability: 使用は 1・2・4・8・16・32・64・128・256・512・1024・2048・4096・8192・16384・32768・65536。131072（bit 17）は空き。`gpu_info.capabilities` は uint32_t で入る。
- 新しい 3 つの struct の大きさ: info 160・option 176・set 40 byte。host の x86_64 と `-m32` で同じ（scratchpad で sizeof・offsetof を出した）。全部の field が自然な境界にあり暗黙の padding が無いので、m68k（int の整列 2）でも同じはず（m68k の compiler では未確認）。`_IOWR` の size の 13 bit（8191）に収まる。
- 名前の文法 `[a-z][a-z0-9_]{0,30}` は 31 文字 + NUL = `GPU_GAME_NAME_MAX` 32 と合う。
- §1 の行: session の struct（gpu.c 219〜262、`native_submitted` 229）、open の権限（1390〜1406）、知らない request の EOPNOTSUPP（2197〜2200）、DISPLAY_EVENTS の EOPNOTSUPP（2335）、context.c の open（104）と GET_INFO（115）、instance.c の `apiVersion = VK_API_VERSION_1_0`（1073）、vk-node.c 149・439〜449、amdrdna-vk.h 200〜204、wsi-display.c の別の open と GET_INFO（1738 付近）。合っていた。
- `amdrdna_vk_geometry_mode(owner)` の owner は node の session（vk-node.c 441〜448）で、session ごとにする口があるのは事実。読むのは compile（vk-pipeline.c 691）と executor の quad の program（vk-execute.c 610）の 2 か所。quad の program は session ごと（`struct amdrdna_vk_execute`）。node 全体の NGG の状態は main の amdrdna には見当たらなかった。
- `vulkan_context_open` は instance.c 721 の `instance_add_context` だけから呼ばれ、その後の `instance_create_remote`（Venus の vkCreateInstance、`GPU_COMMAND` 系）で `native_submitted` が立つ。だから「GET_INFO の後・最初の投入の前」に SET を入れる場所は context_open の中で正しい。
- HAL の API に触れない（`drv_gpu_ops` は kernel の内部で HAL ではない）。承認は要らない。

## Blocker

### B1. `ngg_cull` の定義が boot の行と混ざって、どの設定でも名前どおりに効かない

- 根拠: design §6「ngg_cull（既定 ON）: ON の session の pipeline を `AMDRDNA_VK_GEOMETRY_CULLING` で、OFF は boot の行の値（vk-node.c:149）」。boot の行は 0（legacy、`VK_NODE_GEOMETRY_DEFAULT 0U`、vk-node.c 50）・1（passthrough の NGG）・2（culling）の 3 値（amdrdna-node.h 156〜159、amdrdna-vk.h 197〜203）。WS291 の機械は `ngg=2` で動いている（plan/ws291/ws.md 79）。
- 起こる事:
  - boot 0（main の既定）で `+ngg_cull` → mode 2。culling だけでなく NGG そのもの（`primitive_shader`）も入る。`-ngg_cull` → mode 0 で NGG ごと消える。「culling を切る」ではなく「NGG を切る」になる。
  - boot 2（WS291 の測りの設定）で `-ngg_cull` → boot の値 2 のままで culling が続く。ゲームが driver の不具合を避けるために切っても切れない。ws.md の目的（「ドライバのバグを回避しつつ…ゲーム側が制御」）が壊れる。
  - ユーザーの最初の原文は「ゲーム最適化がかかっているときだけ、NGGをmesh shader以外でも有効にする」で、culling ではなく NGG そのものを指す。design は NGG の passthrough（mode 1）を option にしていない。OFF の時は「boot の行の値」なので、boot が 1・2 なら OFF でも NGG が入り、原文に反する。
- 直し方: (1) amdrdna の option を NGG の段に合わせて 2 つにする。例 `ngg`（非 mesh の頂点を primitive shader に）と `ngg_cull`（`ngg` を要る）。(2) 規則を 1 つ書く。「ゲーム最適化が ON の session では option の bit が全部を決め、boot の行は見ない。OFF の session では boot の行が決める」。boot の行は「OFF の session の既定と、診断のための強制」に限る。(3) WS291 が終わった時の既定（boot の行の既定の値）で OFF の挙動を決める（M5）。(4) 試験に boot 0・1・2 × ゲーム最適化 OFF・ON × `±ngg`・`±ngg_cull` の表を入れ、compile（vk-pipeline.c）と executor（vk-execute.c）の mode が全部の組で揃うことを確かめる。

## Major

### M1. 「投入の後の変更の拒否」が競合に弱く、`native_submitted` の意味を借りている

- 根拠: `native_submitted` を立てる所は 3 つ。`gpu_command_ioctl`（gpu.c 3373、普通の admission の中）、`gpu_job_admit`（5891、`GPU_JOB_COMMIT` から。gpu.c 2016〜2023 で admission の外）、`gpu_submit_ioctl`（6090。`GPU_COMMAND_SUBMIT_SYNC` は gpu.c 2071〜2078 で admission の外の `gpu_dependency_ioctl` から）。どれも `gpu_registry_lock` の下で書く。design §3 は「`native_submitted` が立った後の SET は EBUSY」「核が SET の成功で `game_apply` を呼ぶ」だけ。
- 起こる事: 同じ fd の 2 つの thread で、A が `GPU_GAME_SET` の検査（0 を見た）と `game_apply` の間にいる時、B が admission の外の経路（JOB_COMMIT・SUBMIT_SYNC）で backend に入れる。backend が compile か実行で mask を読んでいる間に `game_apply` が mask を書き換える。compile（vk-pipeline.c 691）と executor（vk-execute.c 610）が違う mode を見ると、legacy の頂点 program を NGG の状態で走らせる組ができ、GPU の hang（device 全体の回復）や誤った描画になりうる（推測。hang するかは未確認）。libvulkan は 1 thread で順に呼ぶので普段は起きないが、kernel は悪意の user に対して守る必要がある。また `native_submitted` は「stop の証明が要らない」の印（gpu.c 228 の comment、7551 の判定）で、WS250 の回復の作業がこの意味を変えると、ゲーム最適化の lock も黙って変わる。`gpu_ops_validate`（gpu.c 1003〜）・「No callback runs under a core spinlock」（gpu.h 108）のため、`game_apply` を lock の下で呼ぶ直し方は取れない。
- 直し方: ゲーム最適化に専用の latch を持つ。例: session に `game_state`（OPEN・FREEZING・FROZEN）。`gpu_ioctl` の入口（admission の分かれ道より前）で、GET_INFO・GET_CAPSET・DEVICE_QUERY・GAME_* 以外の request が来たら registry lock の下で OPEN → FREEZING にし、その thread が lock の外で `game_apply(enabled, active)` を 1 回呼んで FROZEN にして waitq を起こす。FREEZING を見た他の thread は FROZEN まで待つ。SET は OPEN の間だけ core の field を書く（`game_apply` は SET では呼ばない）。これで「backend が mask を読む前に 1 回だけ渡り、その後は変わらない」が全部の経路で成り立ち、`native_submitted` と独立する。`GPU_GAME_LOCKED` は FROZEN を返す。試験に「SET と JOB_COMMIT・SUBMIT_SYNC を別の thread で同時に」を入れる（M8）。

### M2. 名前と版の Vulkan への写し方が決まっておらず、Venus では作れない

- 根拠: design §5「`gpu_name` を `deviceName` に、`driver_version_number` を `driverVersion` に写す（…どちらを優先するかは実装の段で決める、Venus では host の値を保つ）」、§6「venus は host の名前」。実際は、native の driver は kernel の executor が Venus の応答として `VkPhysicalDeviceProperties` を作っている: amdrdna/vk/vk-device.c 264〜284（`"zedBSD amdrdna (AMD RDNA1)"`、`DEVICE_DRIVER_VERSION 1U`）、nvrtx/vk/vk-device.c 262〜282（`"zedBSD nvrtx (NVIDIA Turing)"`）、powervr・ghci・i915（vk-instance.c）・bcm2711（vulkan-query.c）も同じ形。libvulkan は応答を `physical_load`（instance.c 1043〜1075）で受けるだけ。
- 起こる事:
  - libvulkan が上書きすると、真実の出所が 2 つ（executor の応答と GPU_GAME_INFO）になり、片方だけ直すと食い違う。
  - venus の kernel の driver は command の stream を host に渡すだけで、host の応答（`deviceName`）を読まない。だから `GPU_GAME_INFO.gpu_name` に「host の名前」を入れる手段が無い（venus.c 621 は `"venus"` を書くだけ）。libvulkan が「venus なら上書きしない」と判断する印も UAPI に無い（`driver_name` の文字列比較になる）。
  - `driverVersion` の符号化は vendor ごとに違う（NVIDIA は 10.8.8.6 bit、AMD は VK_MAKE_VERSION の形。vulkaninfo も vendorID で読み方を変える。外部の慣習で、この review では出典を確かめていない）。`vendorID` が NVIDIA（nvrtx/vk/vk-device.c 279）のまま zedBSD の版を VK_MAKE_VERSION で入れると、vendorID と driverVersion で回避策を選ぶゲームや tool が「古い NVIDIA の製品の driver」と読み違える（推測）。
  - `deviceName` を製品名だけ（例 "AMD Radeon RX 6600"）にすると、今の名前にある「zedBSD amdrdna」が Vulkan から消える。driverName が無い（`VK_KHR_driver_properties` を出していない）ので、ゲームは Vulkan だけでは zedBSD の driver だと分からなくなる。
- 直し方: (1) libvulkan では上書きしない。各 driver が 1 つの識別の関数（`game_identity` と同じ値）を持ち、executor の `vk-device.c` もそれを読んで `deviceName`・`driverVersion` を返す。venus は何もしない（host の値がそのまま出る。§12 の危険も無くなる）。(2) `driverVersion` の符号化を決める。案: vendor の符号化と衝突しないように「zedBSD の driver では driverVersion の意味は `driver_name` とあわせて読む」と文書に書き、製品の driver に見えない値（例 0 から始める）を使う。(3) `deviceName` に driver の名前を残すか（例 "AMD Radeon RX 6600 (zedBSD amdrdna)"）を決める。消すなら M3 の C の関数で driver の名前が取れることを条件にする。(4) venus の `GPU_GAME_INFO` は「gpu_name は空、driver_name は venus」と決め、M3 の関数はその時 Vulkan の deviceName を返すと書く。

### M3. ゲームが driver を見てから option を選ぶ手順と、そのための libvulkan の関数が足りない

- 根拠: ユーザーの原文「ゲームがドライバを見て、細かいオプションを制御できるようになります」（ws.md）。design §5 の C の関数は option の一覧だけ。`driver_name`・`driver_version`（文字列）・`enabled`・`active` をゲームが取る道は無い（Vulkan には deviceName と数の driverVersion だけ）。環境変数は `vulkan_context_open` の時、つまり vkCreateInstance の時に読まれ、その instance の session は直後の `instance_create_remote` で凍る。
- 起こる事:
  - ゲームが driver を見て option を選ぶには、(a) 探りの instance を作る → 名前と一覧を読む → 消す → `setenv` → 本番の instance を作る、の 2 段が要る。design にこの手順が無く、試験にも無い。多 thread のゲームでの `setenv` は POSIX で安全でない。
  - 関数の名前の案 `vkZedbsdGetGameOptionsEXP` は Khronos の `vk` の名前空間に入る。逆に `vk` で始めない名前にすると、`exports.map`（`global: vk*; local: *;`、userland/desktop/libvulkan/exports.map）で export されない。design はどちらにも触れていない。
  - 公開の `struct zedbsd_game_option` は libvulkan の ABI になる。拡張の欄（`sType`／`size` に当たる物）が無いと後で変えられない。
- 直し方: (1) §5 に「探りの instance → 読む → 消す → 変数を変える → 本番の instance」の手順を書き、QEMU の試験に入れる。(2) 関数を 2 つにする: 識別（gpu_name・driver_name・driver_version・driver_version_number・enabled・active）と一覧。名前は `vk` の外（例 `zedbsd_vulkan_*`）にし、`exports.map` に明示で足す（ABI の変更として phase に書く）。(3) 公開の struct の先頭に `size`（または version）を置く。(4) header の置き場所 `include/libc/vulkan/` には Khronos の派生の file（Apache-2.0、`API-PROVENANCE.md`）が並ぶ。zedBSD の Zlib の header であることを file の license の行で分ける。

### M4. 安全の規則が、session をまたいで共有する資源を見ていない

- 根拠: design §6「option は自分の session の速さと互換だけを変える。memory の隔離・取り消しと復帰（WS250）・他の session の結果を弱める option を作らない」。§4 の例 `+fast_clear`。gpu.h の share・ALLOCATION_SHARE（gpu-allocation.h）・scanout の import で、ゲームの swapchain の画像は compositor（ゲーム最適化 OFF の別の session）と display が読む。
- 起こる事: 画像の memory の形（圧縮・fast clear の metadata・tiling）を変える option を ON の session が使うと、OFF の compositor・display が import した時に読めない、または壊れて見える。今の規則の文は「自分の session の結果」だけを見ているので、この種の option を通してしまう。§4 の例そのものがこの種。
- 直し方: 規則に足す: 「export・import・scanout できる資源の形（format・tiling・圧縮・metadata・同期）を変えない。変える必要がある option は、その資源を export しない時だけ効く、と driver が判定する」。§4 の例を `fast_clear` から別の名前に変えるか、この条件を例に書く。p002 の受け入れに「option の表の各行に、資源の形を変えない事の確認を書く」を入れる。

### M5. WS291 の最適化のうちどれを option にするか、OFF の時の速さがどうなるかが決まっていない（ユーザーの判断が要る）

- 根拠: ws.md の Q1 の解釈「OFF の時は passthrough の NGG か従来の経路（desktop の frame-ops を測って決める）」。design §6・§11 p005 は「WS291 の要素の移し替え」だけで、基準と測りが無い。WS291 は in-progress（plan/ws291/ws.md 47、p006 で PTE の fragment・WC・UCLK・A・C・B1 などが入っている）。Q306-5 の文「OFF（今と同じ挙動）」の「今」は、WS291 が culling を既定にした後では「culling あり」になる。
- 起こる事: (a) 全部を option にして既定 OFF にすると、WS291 で Linux 以上にした desktop と一般のアプリが元の速さに戻る。WS291 の受け入れの数（scene-6500）も、`VULKAN_ZEDBSD_GAME=1` を付けないと再現しない。(b) PTE の fragment・WC の map・UCLK のように、互換に危険の無い最適化まで option にすると、ゲームでない描画を理由なく遅くする。(c) 「OFF ＝今と同じ」と「culling は ON の時だけ」が、WS291 の後で両立しない。
- 直し方: §6 に option にする基準を書く（例: 正しさ・互換の危険がある物、負荷によって損得が変わる物だけ。常に得で危険の無い物は option にしない）。WS291 の最適化の要素を 1 行ずつこの基準で分けた表を、WS291 が終わった時に作る（p005 の最初）。OFF の既定の挙動（legacy か passthrough か、ユーザーの原文は「NGG はゲーム最適化の時だけ」）は、desktop の frame-ops と scene の fps を測った上で Q1 経由でユーザーに確かめる。design §6 の amdrdna の節を「WS291 の後に改訂する暫定」と書く。

### M6. 版の上げ方が無いので、ゲームが「直った版」を見分けられない

- 根拠: §3 の `driver_version`・`driver_version_number`。今は `DEVICE_DRIVER_VERSION 1U` の定数（amdrdna/vk/vk-device.c 53、nvrtx 51）。zedBSD は kernel と driver が 1 つの build。libvulkan は userland で別に変わる。
- 起こる事: 誰も上げない定数では、ゲームが「この版より前は不具合があるので option を切る」という使い方（ユーザーの目的）ができない。不具合が libvulkan にある時は kernel の driver の版では区別できない。
- 直し方: 版の決め方を design に書く。案: driver ごとの版の数を、挙動の変わる commit（option の追加・既定の変更・不具合の修正）で上げる規則と、その記録の場所（driver の source の中の 1 か所の定義、変更の履歴は docs/ の reference）。`driver_version` の文字列には build の識別（OS の版か commit）を入れる。libvulkan の版も M3 の識別の関数で返す。

### M7. p007 の文書の依存が誤り（他の WS の file、まだ無い file）

- 根拠: §11 p007「DOC-54 の `docs/reference/gpu.md` への追記、WS009」、依存 p003。`docs/reference/gpu.md` はまだ無い（docs/reference/ の一覧に無い）。作るのは ws009-p009（planned、22 LW、実行の Queue は未割当。plan/ws009/phase009/phase.md の Status）。AGENTS.md §4 で他の WS の file は読むだけ。
- 起こる事: p007 は対象の file が無いので実行できない。WS009 の担当と同じ file を同時に変えて merge で衝突する。
- 直し方: p007 の依存に ws009-p009（p009b）の完了を足し、Q1 に file の所有の調整を頼むと書く。または WS306 の UAPI の節を ws009-p009 の範囲に入れてもらう（ws009-p009 は「WS306・WS309 がまだ足しうる UAPI なので experimental」と書いている）。

### M8. 試験の計画で見つからない誤り

- 根拠: §10 の表。
- 起こる事と直し方（足す試験）:
  - M1 の競合: host の模型で、SET と JOB_COMMIT・SUBMIT_SYNC・GPU_COMMAND を別の thread から同時に。`game_apply` がちょうど 1 回、backend の最初の callback より前に呼ばれること。
  - B1 の表（boot 0・1・2 × ON/OFF × option）と、compile・executor の mode の一致。判定は driver の log ではなく、`GPU_GAME_INFO.active` と executor が数える pipeline の形の counter（新しく出すなら試験の道具として）で。実機の dmesg を SSH で読むのは許されるが、QEMU の console log では判定しない（AGENTS.md §9）。
  - venus（QEMU）で `deviceName`・`driverVersion` が host の値のまま変わらないこと（M2 の回帰。今の表に無い）。
  - fork・dup: 同じ open file description を共有するので、子の SET は親の投入の後なら EBUSY。2 つの VkInstance は別の session で別の設定になること。
  - display の session（wsi-display.c の別の open）に SET が飛ばないこと。
  - UAPI の大きさ: gpu.c 121〜123 と同じ `_Static_assert` を 3 つの struct に。host の `-m32` と x86_64 の比較（plan/ws014/tests/gpu-uapi-layout.c は WS014 の物なので、足すなら Q1 に頼むか WS306 の試験に写す）。
  - 解読の負の例: NUL を含む文字列、`bytes` = 513、末尾の「,」、「+-x」、空の token、`enable = 0` で壊れた文字列（EINVAL にならない）、`index >= option_count`、reserved が 0 でない。
  - M5 の測り: OFF の既定で desktop の frame-ops と scene の fps が WS291 の最後の数から落ちないこと（落ちるならユーザーの判断）。

## Minor

- m1. `drv_gpu_ops` の追加の形。venus は ops を位置で初期化している（venus.c 357〜380）ので、新しい field は struct の最後に足す。`DRV_GPU_INTERFACE_VERSION`（9）を上げるかを書く（上げると全 driver の build が同じ commit で要る）。`gpu_ops_validate`（gpu.c 1029〜1035）の許す bit に `GPU_CAP_GAME` を足し、bit があれば `game_identity` を必須、`game_option_count > 0` なら `game_apply` と表を必須、表の各名前が §4 の文法・重複なし・NUL 終端・数 ≤ 64、description が NUL 終端、を登録の時に検査する、と書く。
- m2. struct の規則: 3 つの struct の `version` の値（`GPU_ABI_VERSION` か）、入力でない field と `reserved` は 0 を求める（既存の `gpu_resource_map`・`gpu_device_info` と同じ「Only version and size are inputs」の扱い）、`GPU_GAME_OPTION` の index の範囲外は EINVAL、SET を 2 度呼んだ時は既定から作り直す（積み重ねない）、文字列の中の NUL は文法の誤り。
- m3. 対象の node: `GPU_CAP_GAME` は render の node だけにする、と書く。display だけの node（powervr/display/display.c 562 `POWERVR_DISPLAY_DRIVER`、bcm2711/display/display-device.c 479 `bcm2711-display`）を §6 の表に入れるか外すかを明記。§1 の `driver_name` の表に i915（session.c 270）・powervr（render.c 1051）・venus（venus.c 621）が抜けている。
- m4. 権限: §3「write の権限は要らない」。read だけの open は投入できないので害は無いが、SET は session の挙動を変える操作なので、他の変更の ioctl（`session->writable`、gpu.c 2552 など）と揃えて writable を求めた方が分かりやすい。
- m5. 文法の将来: 1 つの変数が全 GPU に渡るので、2 つの driver が同じ名前の option を持つと片方だけを切れない。今は使わなくてよいが、`.` か `:` を driver の名前の区切りとして予約する（今は文法の誤り）と書いておくと後で文法を壊さずに足せる。option の間の依存（B1 の `ngg_cull` は `ngg` を要る）の表し方（`GPU_GAME_OPTION` の flag か、driver が黙って落とすか）も決める。
- m6. libvulkan の細目: `VULKAN_ZEDBSD_GAME` が 0・1 以外（"yes"、"2"、空）の時、`_OPTIONS` だけがある時（OFF のまま）の扱いを書く。unknown は黙る（Q306-3）としても、名前が全部の device で unknown だった時（綴りの誤り）は stderr に 1 行出すと、ゲームの作者が気づける（ユーザーの決定「無視して数を返す」とは両立する、kernel の挙動は変えない）。変数は compositor・helper にも継がれる（§12）。WS309 の「game mode」（compositor の direct scanout）と語が近いので、文書で区別する。
- m7. `VkPhysicalDeviceDriverProperties` を WS226 の後に回す理由: `VK_KHR_driver_properties` は 1.0 でも `VK_KHR_get_physical_device_properties2`（libvulkan は `VULKAN_INSTANCE_PROPERTIES2` を持つ、instance.c 658）の上で出せる。出さない理由は `driverID` に zedBSD の登録された値が無い事のはず（推測）。理由を §0 に書く。
- m8. Phase の依存: p003 の QEMU の試験は venus の `GPU_GAME_INFO` を要るが、venus の識別は p004。p003 の依存に p004（の venus の分）を足すか、venus の識別を p002 に入れる。
- m9. vulkaninfo の移植の危険（どれも推測、移植の段の最初に確かめる）: vulkaninfo は C++17 と Vulkan-Headers の版の一致を要る（tree は `VK_HEADER_VERSION 269`、include/libc/vulkan/vulkan_core.h 39）。実行時に `libvulkan.so.1` を dlopen する版がある（zedBSD は `/lib/libvulkan.so`、Makefile 24）。instance・device が 1.0 の時の振る舞い、`--json`・`--summary` の出力にも patch が要る。8 LW の前に、patch なしの vulkaninfo が QEMU で動くかの 1〜2 LW の下見を入れる。
- m10. p004 の 4 LW: amdrdna の表は chip の名前だけ（`struct amdrdna_product { device, generation, chip }`、amdrdna-hw.h 990〜994）で、"AMD Radeon RX 6600" のような製品名は無い。製品名の表を作るなら、名前の出典（pci.ids など）の license と、guardrail の「Values referenced:」の block ごとの出典の書き方が要る。各 driver の実機の確認（i915・powervr・bcm2711・nvrtx）が試験の表に無く、build だけの確認になる。そう書く。
- m11. pipeline cache: 今の amdrdna の pipeline cache は object を作るだけに見える（vk-layout.c 95〜99、data の中身は未確認）。後で cache の data を出す時は、`active` の mask を `pipelineCacheUUID` か cache の key に入れる、と §6 に一行。
- m12. 語の食い違い: ws.md「session（process）ごと」と design「open ごと」。実際は open file description ごと（gpu.c 213 の comment「shared by dup/fork until its final close」）で、1 process の 2 つの VkInstance は別の session。ws.md の語を design に合わせる（ws.md は担当が直してよい範囲）。
- m13. 照合した tree: design は main 61630bb0b を照合と書くが、worktree の HEAD はレビューの間に f6aae6221 → dd607b017 と進んだ。§1 の行はこのレビューで f6aae6221 の tree で合っていた。次の版で照合した SHA を更新する。

## 見積もり（§11、計 41 LW、足し算は合っている）

- p002 の 8 LW は M1 の latch・m1 の登録の検査・m2 の規則を含めると +2〜3 LW。
- p005 の 10 LW は M5 の表（WS291 の要素の分類）と B1 の 2 つの option、測りの往復で増える見込み。WS291 の終わりの形が分かるまで ±50% の幅に収まらない恐れがある。
- p006 の 8 LW は m9 の下見の後に決め直す。
- p007 は M7 の依存で、ws009-p009 の後に回る。

## 出典の規則（guardrail）

- design の中に、source に入れてはいけない管理情報を source へ書く指示は無い。§4 の RADV・DXVK の書式は慣習の比較だけで、code・symbol の写しは無い。vulkaninfo の patch は Apache-2.0 の file への変更になるので、package の監査の記録に patch の license を書く（§7 に一言）。製品名の表を作るなら m10。

---

# 第 2 回: design.md 第 3 版の敵対的レビュー（design-reviewer、2026-10-31）

対象: agent/p5-ws306 c0e511840（親 main 9946e22c0）の design.md 第 3 版。照合した tree も同じ。file は変えていない（sizeof と警告の確かめは scratchpad）。
結果: Blocker 0・Major 7・Minor 13。反映は [design.md](design.md) §18（全部を反映、見送り 0）。

## Major

- **M1 `apply` の契約が実際の callback の順と合わない**: 許可の表の `GPU_DEVICE_QUERY` は `ops->scanout->query_device(…, session->backend)`（gpu.c 5031）を、`GPU_GAME_INFO` は `identity` を呼ぶ。poll は凍らせないが `gpu_events_snapshot` → `ops->display->events`（2452 付近）を呼ぶ（COMMAND と DISPLAY_EVENTS を同じ node に持つ amdrdna・i915・nvrtx・ghci・venus で起きる）。凍らせないまま close した session では `drain`・`resource_destroy`・`close` が走り `apply` は無い。`apply` は admission の外で、同じ session の他の callback と並ぶ（gpu.h 109〜111 の前提の外）。→ 契約を「仕事を出す・compile する callback の前に多くても 1 回、他は前・無しにもありうる、並行しうる、open は OFF で初期化、offline では飛ばす」に書き直す。
- **M2 試験が latch と option の核心を確かめられない**: QEMU の venus は option 0 個で `active` は常に 0、OPTION は常に EINVAL。競合の試験は 1 つの fd で 1000 回なので 2 回目からは凍った後の速い道しか試さない。「最初の callback の前に 1 回」は guest の出力から観測できない。「kernel の warning が無い」の判定の方法が無い（AGENTS.md §9）。→ 偽の backend を gpu.c につないだ host の模型（WS014 の `plan/ws014/tests/gpu-framework.c` が前例、runner は無い）、各回新しい open の競合、libvulkan の `active` の変化は host の差し替えか amdrdna の実機、dmesg は serial.py・SSH の対話、性質と試験の対応の表。
- **M3 `requires` は C++20 の予約語**: UAPI・kernel・libvulkan の struct の field 名。vulkaninfo（C++）とゲーム（C++20）で compile できない。→ `requirements` に。`vulkan_zedbsd.h` に `extern "C"`・`VKAPI_ATTR`/`VKAPI_CALL`、struct の tag と関数の名前を重ねない、ACTIVE の flag を header に定義。
- **M4 一覧の配列と要素ごとの `size`**: 呼び手が古い struct の配列を渡すと新しい libvulkan の歩幅で配列の外に書く。→ 1 個ずつ取る関数か、stride の引数か、`options[0].size` を歩幅にすると明記。
- **M5 Q306-8 の前提の誤り**: zedBSD の libc の `getenv`（`userland/base/libc/posix.c` 355〜）・`setenv`（409〜）は `environment_lock()`（`pthread.c` 177）の下で、getenv は thread ごとの複写を返す。「setenv は thread のある program で安全でない」は zedBSD では成り立たない（移植性だけの問題）。→ 問いと §9.4 を直してから問う。
- **M6 標準廉価実装手順を条件の外で使う案に問いが無い**: 手順 §1 は条件を全部満たす時だけ使い、満たさなければ普通の Phase と書く（ユーザーの決定）。design §13 は条件 1 を満たさないと認めながら推し、段取り（p002 と p003 で 8 LW）が実装より大きい。→ ユーザーに問うか、推しをやめて普通の Phase に入れる。
- **M7 GAME の bit を足す所**: COMMAND は executor を bind する関数で後から足す（amdrdna `runtime.c` 204、nvrtx `runtime.c` 235、powervr `render-runtime.c` 97、mali `sched.c` 395、bcm2711 `render-runtime.c` 71、ghci `vk/vk-node.c` 129。初期値に COMMAND の無い powervr `render.c` 181、mali `render.c` 130、bcm2711 `render-device.c` 26）。GAME を静的な初期化に置くと executor の無い構成で登録の検査が EINVAL になり node ごと消える。→ 全 driver で COMMAND と同じ所で足すと書き、executor の無い構成の登録を試験する。

## Minor

- m1 venus の位置の初期化は `-Wall -Wextra -Werror`（Makefile 1098〜1101）の `-Wmissing-field-initializers` で止まる。「driver の source の変更は要らない」は誤り（venus.c に 1 行要る）。
- m2 NGG の mode を読むのは 2 か所（`vk/vk-execute.c` 624 の quad の program も）。`amdrdna_vk_geometry_mode` の中で session の値を返し、試験に quad の program の mode を足す。
- m3 mutex を壊す関数は無く、`mutex_init` の失敗は NULL の時だけ（`src/kern/lock.c` 207〜）。§6・§11 の手順の文を消し、rank を明記、SET の copyin と解読を `game_lock` の外で。
- m4 INFO・OPTION を admission の下か外かが無い。SET の `address` は `gpu_user_range`（gpu.c 3072〜）で検査する。
- m5 `enable = 0` と `bytes > 512`・`reserved != 0` の検査の順、「依存は 1 段」と「繰り返す」の食い違い、符号の無い `name` は Q306-2 への追加、名前の境は 31 と 32（33 は境の外）、`:`・`.` を EINVAL にすると将来の書式で古い kernel が文字列全体を拒否する。
- m6 p006 の依存「p005 の venus」と p004 の「venus の identity」の食い違い、文書が WS291 待ちの p007 に依存、vulkaninfo は libcxx が要り（harfbuzz の `DEPENDS`）その build は Q1 の許可が要る、`ZEDBSD_EXTERNAL_SOURCE` は 1 つの名前に 1 つの archive（external.mk 291〜）、tag の archive の SHA-256 が変わる事例（推測）。
- m7 `deviceName` を揃えると利用者に見える名前が変わる（bcm2711・mali・ghci・amdrdna。powervr は同じで `plan/tools/powervr/p009/frame-host.c` 934 の照合は保たれる）。変える一覧を Q1 に確かめるか、今の文字列を保つ。
- m8 Q306-9 に gpu.c での WS291 の job の案・WS250 との衝突の費用を書く。WS291 が欄を足すのは `drv_gpu_job_ops` で、版を上げるかは WS291 の文書に無い。
- m9 試験の image を `plan/ws306/tests/` の config.mk で作り、guest の program を入れる構成を書く。
- m10 ws.md の p001 の行・Phase の表・目標の「session（process）ごと」・原文の引用、`phase001/phase.md` が無い、pipeline cache の注意が §8.2・p007 に無い。
- m11 session・device に誤りがある時の凍らせの扱い。
- m12 build の一覧に ghci・venus の構成が無い。
- m13 `VkPhysicalDeviceDriverProperties` の引き継ぎと、WS226 が header を替える時に `vulkan_zedbsd.h` と `API-PROVENANCE.md` の行を残す事を、Q1 が WS226 に記録する。

## 確かめて合っていた事（要点）

- 'G' 0〜45 は重なり・抜け無し、46〜48 と 131072 は空き、WS291 の予約は 49・50 と 262144・524288（job-submit-design.md 6・31〜32・90〜91 行）。
- struct の大きさ: info 160・option 184・set 40・`zedbsd_vulkan_game_identity` 352 byte（x86_64・`-m32`）、暗黙の padding 無し。
- design の引く gpu.c・gpu.h の行は全部合っていた。admission の外の経路の一覧も合っていた。
- latch の骨組み: file の操作は open・close・ioctl・poll・mmap（1414〜1420）、ioctl は全部 `gpu_ioctl_body` を通る、mmap は backend の callback を呼ばない（3719〜）、lock の順に循環は無い、`busy_owner` の再入は速い道で影響しない、rank は実行時に検査されない、release と acquire で `apply` の書き込みはその後の callback に見える。
- libvulkan: SET の置き場所は正しい（GET_INFO・GET_CAPSET の後、次は `instance_create_remote` の blob・command）、`vulkan_context_open` の呼び手は 721 だけ、display の session は `wsi-display.c` 1720・`wsi-display-nodes.c` 473 の別の open、`VkPhysicalDevice_T` は 1 つの context の fd に属する（internal.h 121〜124、Venus では 1 つの context に複数の physical device）、instance を作る間に thread は無い。
- amdrdna で NGG を session ごとに選ぶのは安全（program ごとの `primitive_shader`、pm4 の geometry の flush は両向き、session をまたぐ shader の cache は無い、culling を飛ばす判断は compile に残る gfx10-compile.c 740〜743）。
- COMMAND を持つのは render の node だけ。HAL の API の変更は要らない。source に管理情報を書く指示・外部の code の写しは無い。

## 第 1 回の指摘の解消（第 2 回の判定）

解消: B1・M3〜M7、m1（venus の文を除く）・m2〜m7・m9・m10・m13。一部: M1・M2・M8・B1 の (4)・m8・m11（それぞれ第 2 回の M1・m7・M2・m2・m6・m10 で直した）。未解消: m12（ws.md の語、第 3 版の反映で直した）。
