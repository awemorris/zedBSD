<!-- awesome-plan project=zedbsd record=ws341-design -->

# WS341 設計: 仮想 GPU（program が繋ぐ仮想の display）

- 作成: 2026-11-15 P8（q2112-i01、ws341-p001）。基点 main 44bb7cdaac。
- 状態: 案（第 2 版、design-reviewer の指摘 R1〜R25 を反映。[design-review.md](design-review.md)）。[user-questions.md](user-questions.md) の問い 1〜8 の答えで確定する。
- 読む順: §0 要旨 → §2 全体の形 → §5 UAPI → §6 流れ → §13 Phase の表。

## 0. 要旨

- kernel に仮想 GPU の driver（以下 vgpu）を足す。vgpu は **物を持たない display controller** で、二つの口を持つ:
  1. 制御の口 `/dev/vgpu`（新しい UAPI `include/uapi/vgpu.h`）: program が仮想の monitor を繋ぐ・外す・大きさを変える、frame を受け取る・返す。
  2. 表示の口 `/dev/gpuN`（今の GPU の UAPI のまま）: compositor（libvulkan）から見て、scanout だけを持つ display node（`GPU_DEVICE_DISPLAY` だけの node、bcm2711・mali・powervr の display node と同じ種類）。
- 推しの形（問い 1）: vgpu の display node は **起動時に 1 つだけ登録** し、仮想の connector を 4 つ持つ。program の create は空いた connector に仮想の monitor を「挿す」、close（落ちた時を含む）は「抜く」。compositor から見ると monitor の抜き差しそのもので、WS353 の hotplug の fence・compositor の hotplug の thread・WS113 の heads がそのまま使える。ws.md の決定の「作る度に /dev/gpuN を増やす」形は、node の到着の通知が無い事・libvulkan の node の一覧が instance ごとに固定な事・抜いた node が compositor が閉じるまで残る事から、追加の作業と危険が大きい（§3.1）。
- scanout の image は compositor が system の GPU（render node）で作る（ユーザーの決定 4）。compositor は image を resource の形で export し、vgpu の node に `GPU_IMPORT_SCANOUT` で import し、`GPU_DISPLAY_PRESENT_BLOB` で出す。vgpu は image の **capability（kernel の handle）を持つだけ** で、物理の page を読まない。program は frame ごとにその capability を fd で受け取り、自分の render node の open に import して Vulkan で読む（同じ node の import なので、全ての render driver が既に持つ道）。
- このために driver の枠組み（`include/drivers/gpu/gpu-scanout.h`、HAL ではない）に、page を要らない scanout import の op を 1 つ足す（§9）。HAL の API（`include/hal/hal.h`）は変えない。
- frame の受け渡し: compositor の present は、program が前の frame を持っていなければ直ちに採られる（adoption）。program が持っている間は次の frame の採用を待たせるが、上限（8 ms 固定）で打ち切る。さらに libvulkan の present の worker を display の swapchain ごとに分け、vgpu の待ちが物理の画面の present の列を止めないようにする（p006、§8）。compositor が program の遅れ・停止で止まらない事を優先する（§6.4）。
- 仮想の印: 出力の flag `GPU_DISPLAY_REMOTE`（新、「frame を monitor でなくこの machine の program が受け取る」。`GPU_DISPLAY_VIRTUAL_CLOCK` とは別の意味）と、表示の名前の key の種類 `virtual`（`zedbsd-port-v1:virtual:vgpu:N`）。compositor は Vulkan だけを使う規則なので、名前の key で見分ける（WS113 の D-ID A2、私有の Vulkan 拡張は不採用の決定に合わせる）。決定 (3) の「flag が見える」を名前の key で満たす事は問い 8 で確かめる。
- 試験は QEMU（Venus）で: kernel の口だけの試験（program 自身が compositor 役）と、Keiland の上で frame を file に落とす試験。Vulkan Video の encode は libvulkan にまだ encode が無いので、encoder の WS（WS324 など）の後の Phase（問い 5）。

## 1. 乖離の確かめ（ws.md 2026-11-01 の前提と今の main 44bb7cdaac）

| ws.md の前提 | 今の main | 扱い |
| --- | --- | --- |
| compositor が display の変化を拾う道は未定 | WS353 p002〜p004 で kernel の `GPU_DISPLAY_FENCE`（`GPU_DISPLAY_FENCE_HOTPLUG`・`REFRESH`、driver の push `drv_gpu_display_changed`）、libvulkan の hotplug の fence、compositor の hotplug の thread が入った（cleared、main に merge 済み）。fence は libvulkan が renderer に結んだ **全ての display node** に登録する（`vulkan_wsi_display_node_hotplug`）。 | vgpu は `GPU_CAP_DISPLAY_EVENTS` を持ち、connector の抜き差しで `drv_gpu_display_changed` を呼ぶ。compositor 側の変更は要らない見込み（§8）。 |
| 「/dev/gpuN を作る」 | libvulkan の display node の一覧（`wsi-display-nodes.c` の inventory）は instance を作った時に `/dev` を一度読んで固定し、「node の数は変わらない」前提で hotplug の登録を数える。後から出来た node は compositor の再起動まで見えない。node の到着を知らせる仕組みも無い。 | 問い 1。推しは起動時に 1 つの仮想 display node（§3.1）。 |
| image の共有は WS243 の NVDEC の export・import（`GPU_CAP_ALLOCATION_SHARE`）と同じ道 | allocation の形（`GPU_ALLOCATION_*`）は node を越えない（`EXDEV`）。display へ出す道は resource の形（`GPU_RESOURCE_EXPORT`・`GPU_RESOURCE_IMPORT` と `GPU_IMPORT_SCANOUT`）。 | resource の形を使う（§7）。 |
| 他の GPU の image を display が読む道はある | 別の node からの scanout import（`gpu_scanout_import`）は source の driver が物理の page を出す（`get_scanout_backing`）事を要る。出すのは powervr・videocore・mali の render node だけで、venus・i915・amdrdna・nvrtx・ghci は出さない。 | vgpu は page を読まないので、page を要らない import の op を枠組みに足す（§9）。 |
| encode は libmedia の encoder の口（WS318） | WS318 は canceled（2026-11-15）。libvulkan に Vulkan Video の encode は無い（decode だけ）。software の H.264 の encoder は WS329（incomplete、p003 は sources.mk の承認待ち）。 | この WS の encode の受け入れは後の Phase（問い 5）。p001 の範囲は「frame ごとに知らせを受け、image に access できる」口まで。 |
| compositor は普通の display として扱う | compositor は anchor の出力 1 つ＋他の display を head（mirror・extended）として出す（WS113、`heads.c`）。extended の head は今は壁紙だけで、窓は移せない（ws113-p007 は未）。 | 仮想の display は head として出る。遠隔の画面で窓を使うには mirror か、窓の移動（WS113 の後の Phase）が要る。問い 4 で product の扱いを聞く。 |
| — | seat の manager（sessiond）は `/dev/gpu0`〜`/dev/gpu3` だけを seat の user に渡す。それより後の番号の node は root・video・0660 のまま。 | vgpu の display node が gpu4 以降になると、video group でない seat の user の compositor は開けない。sessiond を「番号でなく全ての GPU node」に直す小さな Phase を置く（§10、p007）。 |
| — | docs の乖離（この WS の範囲外、Q1 へ報告）: `docs/reference/gpu-display.md` に `GPU_DISPLAY_FENCE`（54）・`GPU_DISPLAY_FENCE_CANCEL`（55）の記述が無い。`docs/reference/gpu.md` の「Who may open」は video group を認めると書き、同じ節の末尾は「root と owner だけを認める」と書く（自己矛盾。code の `gpu_open` を読んで直す必要）。 | 報告だけ。 |

## 2. 全体の形

```
 program（RDP の server、遠隔の display の送り手、capture の道具）
   │ open /dev/vgpu → VGPU_CREATE（大きさ・refresh）       ┌──────────────── kernel ────────────────┐
   │ poll / VGPU_FRAME_ACQUIRE → fd（image の capability）  │ vgpu driver                              │
   │ VGPU_FRAME_RELEASE                                    │  ├ 制御の口 /dev/vgpu（open ごとに 1 connector）│
   │ close → 抜く                                          │  └ display node /dev/gpuN（connector 4 つ）   │
   │                                                      │       drv_gpu_display_ops・scanout ops      │
   │ Vulkan: fd を import（自分の render node の open）       │       drv_gpu_display_changed（抜き差し）     │
   ▼                                                      └──────────────────────────────────────────┘
 render node /dev/gpuR（system の GPU）  ◀── image の storage はここ（compositor が作り export）
   ▲
 compositor（Keiland、libvulkan だけ）: hotplug の fence → 列挙 → head を開く → swapchain（共有の route）
   → image を export・vgpu node に GPU_IMPORT_SCANOUT で import → GPU_DISPLAY_PRESENT_BLOB
```

- vgpu は render の機能も storage も持たない。`GPU_GET_INFO` の capability は `GPU_CAP_DISPLAY`・`GPU_CAP_DISPLAY_EVENTS`・`GPU_CAP_DISPLAY_CONTROL` だけ（`GPU_CAP_RESOURCE`・`BLOB`・`SHARE`・`FENCE` は無い）。`GPU_DEVICE_QUERY` の `roles` は `GPU_DEVICE_DISPLAY`、`companion_id` は 0（libvulkan は既定の renderer＝最小の device_id の renderer に結ぶ。§14 の限り）。
- `GPU_DISPLAY_CONSTRAINTS` は route `GPU_SCANOUT_SHARED | GPU_SCANOUT_FOREIGN`（`GPU_SCANOUT_COPY` は無い: vgpu は自分の memory を持たない、ユーザーの決定 4）、format は BGRA8888・RGBA8888、stride・offset の alignment 4、placement 0、`max_dma_address` 0。
- image の layout は linear の行だけ（vgpu は `drv_gpu_layout_ops` を持たない。core の import は linear でない image を `EXDEV` で断り、libvulkan は COPY の無い display に linear の image を使う既存の道を取る）。理由: program が CPU でも GPU でも素直に読め、encoder の入力への変換も単純。compositor の最後の書き込みが linear になる損は、仮想の出力の数と大きさでは小さい。

## 3. 決め方の根拠

### 3.1 仮想の display node を 1 つにする（推し）か、作る度に node を増やすか

本質: 仮想 GPU は「program が繋ぐ monitor」であって、描く力（GPU）ではない。描くのは system の GPU で、vgpu が足すのは「出力先」だけ。一般の OS でも、後から挿す display（USB の display adapter など）は「device の到着」と「connector の抜き差し」の二段で扱う。zedBSD の今の道具立てでは:

| 観点 | A: node 1 つ＋connector（推し） | B: create ごとに /dev/gpuN |
| --- | --- | --- |
| compositor が新しい表示を知る | 既存の hotplug の fence（node の topology の変化）でそのまま。 | node の到着を知らせる仕組みが無い。kernel に「GPU node の集合の変化」の fence か `/dev/system` の事象、libvulkan の inventory を可変にする改修（今は instance の生存の間は不変が前提）、hotplug の登録の数の前提の変更が要る。 |
| program が閉じた時 | connector の抜き。generation と topology の番号が進み、compositor は列挙し直して head を閉じる（monitor を抜いたのと同じ）。 | node の withdraw。libvulkan の discovery の open が instance の終わりまで node を持つので、`drv_gpu_unregister` は `EBUSY` のまま。node の番号と wrapper は compositor の再起動まで残り、create を繰り返すと増え続ける。 |
| 権限 | node 1 つを seat に渡せば済む。 | 作る度に owner を決める。sessiond は番号 0〜3 しか渡さない。 |
| 失敗の分離 | 1 node の全 connector が同じ device。vgpu は hardware を持たないので device の失敗は起きにくい。 | node ごとに分かれる。 |
| 起動時の副作用 | connector 0 本挿しの node が 1 つ常に居る（下の注）。 | 使わない時は何も無い。 |

注（A の起動時の登録、レビュー R5 で改めた）: node の番号は登録の順なので、vgpu が hardware の GPU より先に登録すると `/dev/gpu0` を取り、sessiond の「`/dev/gpu0` があれば表示がある」判定を誤らせうる。vgpu は `hw.gpu.attaching` を数えない（render node の来ない machine では止める合図が無く、sessiond が最大 120 秒待ってしまう）。代わりに core に「render の役を持つ node が登録された」時の kernel の中の知らせ（p003 で足す、登録の道の中で同期に呼ぶ）を置き、vgpu はそれを受けて、その driver の attach が終わる（attaching が減る）前に自分の display node を登録する。sessiond と greeter の Vulkan が node の一覧を固定する時には vgpu の node が既に居る。render node の来ない machine（GPU の無い server）では何も起きず、起動の遅れも無い（p005 の試験に GPU の無い image の起動の時間を入れる）。

B を選ぶ場合の追加の作業は §16 に挙げる。

### 3.2 vgpu が image の capability を持ち、program に fd で渡す

- 目的は「frame ごとに知らせを受け、その image に access できる」（ユーザーの決定、2026-11-01 22:50）。access を Vulkan Video でも CPU（差分の符号化）でも使える形にする。
- image は system の GPU の memory にある。その GPU で読むのが最も速く、copy を避ける zedBSD の方針にも合う。Vulkan の external memory（`VK_KHR_external_memory_fd`、opaque fd）は libvulkan に既にあり、resource の形の fd を同じ render node に import できる（`memory.c` の `memory_import_fd`）。
- vgpu が物理の page から読む形（page の CPU の map を program に渡す）は、page を出す render driver が SoC の 3 つしか無く、x86 の GPU（i915・amdrdna・nvrtx）と venus で使えない。また GPU の cache・圧縮・device memory（VRAM）の読みを vgpu が知る事になり、driver の責務を越える。
- capability は allocation 全体への書きの権利も含む（`gpu-sharing.md`）。program は compositor の出す image を書き換えられる。program は画面の内容を全て受け取る立場（画面の capture と同じ）なので、書きの権利の追加は危険の大きさを本質的には変えない。誰が `/dev/vgpu` を開けるかが本当の境界（§10、問い 2）。

### 3.3 frame の採用を program の遅れに結び付けるが、上限で切る

- libvulkan は display の present の ioctl が成功で返った時点で、前の front の image の hold を外し、swapchain に返す（`wsi-display.c` の present の後の `previous` の扱い）。つまり **present が返る事 ＝ 前の image がもう読まれない事** でなければならない。
- program が frame k を読んでいる間に compositor の present（k+1）を返すと、compositor は k に描き始め、program は壊れた frame を encode する。よって vgpu は、program が k を持つ間は k+1 の採用（と present の return）を待たせる。
- 一方 compositor の present を長く止めると、compositor の present の thread と、libvulkan の process 全体の `display_mutex`（present の ioctl の間 lock を持つ）が止まり、物理の display の表示まで遅れる。program の停止・遅さで desktop が止まるのは避ける（ユーザーの決定: program が落ちても compositor は落ちない、の延長）。
- 決め: 待たせる上限を **8 ms 固定**（`VGPU_HOLD_MAX_NS`、`VGPU_INFO` で返す）とし、過ぎたら program の hold を取り上げて（revoke）次の frame を採る。program は release の答えで取り上げを知る（その frame は読みの途中で書き換わった可能性がある）。program への推奨の使い方は「acquire → 自分の image へ GPU で copy か色の変換（Vulkan Video の encoder の入力は普通 YUV なので変換は要る）→ その完了で release → encode」（§7）。4K の BGRA の GPU の copy は数 ms で終わる。
- 上限だけでは足りない（レビュー R1）: compositor は anchor → head の順に同じ queue へ present を出し、libvulkan の ordinary の lane の worker は 1 本で順序を守り、present の ioctl の間 process 全体の `display_mutex` を持つ（`wsi-swapchain.c` の lane、`wsi-display.c` の present）。vgpu の head の待ち（最大 8 ms）が次の anchor の present を遅らせ、物理の画面が vblank を逃しうる。よって p006 で libvulkan の display の present を swapchain（node）ごとの worker に分け、present の ioctl を `display_mutex` の外で行う（lock は記録の更新だけ）。受け入れに「program が hold し続けても anchor の frame の間隔が変わらない」を入れる。
- program が acquire していない時は、present は直ちに採られる（virtual の tick を待たない）。compositor の歩調は物理の display と refresh の fence が決め、vgpu の present は compositor の present の thread を待たせない。

## 4. kernel: vgpu driver

### 4.1 置き場所と build

- source: `src/drivers/gpu/vgpu/`（`vgpu.c`: 登録・制御の口、`display.c`: display ops、`frame.c`: frame の受け渡しと clock）。driver の `drv_` の global symbol の方針に従う。
- build: `mk/kernel.mk` に `CONFIG_DRIVER_VGPU` の block と、x86_64・arm64・riscv64 の config に `CONFIG_DRIVER_VGPU=y` が要る。Makefile・`mk/` は Claude が変えない規則なので、差分の案を [proposed/kernel-mk.diff](proposed/kernel-mk.diff) に置き、ユーザーの承認を問い 6 で求める。
- 制御の口の device 番号は既存の固定番号と重ならない値を実装の Phase で選ぶ。`/dev/vgpu` の作成は kernel の初期化の呼び口（input-inject と同じ場所）から、devfs の mode と group の選択に `vgpu` の名前を足す（GPU の node と同じ 0660・video。今の devfs は gpu<数字> 以外を 0666・wheel にする）。
- open の権限（レビュー R3）: devfs の mode だけに頼らず、vgpu の open が `gpu_open` と同じ 3 つの規則（root、node の owner、mode が group に読み書きを与える時の group の member）を確かめる。
- userland の試験の program と道具の Makefile（`userland/tests/vgpu-*`、`vgpu-capture`）も build system の一部で、Claude は変えない。p005・p009 の前に差分の案を出して承認を受ける（問い 6）。

### 4.2 状態

- device（1 つ）: display node の `drv_gpu_device`、connector の配列（4）、topology の番号（1 から増える、`events` op が返す）、lock（mutex。`events` op 用に IRQ-safe な番号の snapshot を別に持つ）。
- connector: 状態（空き・繋がり・抜けた後の lease の残り）、持ち主の制御の open、`display_id`（connector ごとに固定、1〜4）、`generation`（抜き差し・大きさの変更ごとに進む。kernel の生存の間は戻らない）、mode（幅・高さ・refresh・物理の mm）、lease（compositor の open と値）、power の状態、virtual clock の起点、frame の列（下）、program に渡した image の集合と epoch、待ち（program の acquire、compositor の adoption、refresh の wait）。
- frame の列（connector ごと）:
  - `current`: 採られた最新の frame（image の参照・sequence・採用の時刻）。compositor の image の hold はここ（display に「表示中」）。
  - `pending`: 採用を待つ frame（program が `current` を持っている時だけ存在しうる）。新しい present は古い pending を置き換え、置き換えられた frame は置き換えた frame の採用と同時に採られた扱い（ABI の定義どおり）。
  - `held`: program が acquire して release していない frame の sequence（0 か `current` の sequence）。
  - `delivered`: program が最後に acquire した sequence（次の acquire は、これより新しい `current` を待つ）。

### 4.3 display ops（`drv_gpu_display_ops`）

| op | vgpu の答え |
| --- | --- |
| `query` | connector 4 つを常に列挙（`count` = 4）。`generation` は起動時から全 connector で 1 以上（core は 0 の generation を `EIO` にし、libvulkan は node の一つの失敗で列挙全体を失う）。繋がっている connector は flag `CONNECTED`・`VIRTUAL`（新）・`VIRTUAL_CLOCK`・`FIFO`・`BLOB`・`POWER_CONTROL`・`REFRESH_COUNTER`、表示中なら `ACTIVE`、電源を切られていれば `POWERED_OFF`。空きは `CONNECTED` 無し。`plane_count` 1、format 2 つ、`max_frame_bytes` は linear の image が取りうる大きさの上限（stride の padding を含む値を driver の上限として返し、これで present を断らない）、current・preferred・max の大きさは作った大きさ、物理の mm、refresh。名前は `zedbsd-port-v1:virtual:vgpu:<connector>`。 |
| `mode` | mode は作った 1 つだけ。VALIDATE は同じ幅・高さ・refresh（0 なら作った refresh）だけを認める。空きの connector は `ENXIO`、古い generation は `ESTALE`。 |
| `claim` | 繋がっている connector の plane 0 だけ。lease は connector ごとに 1 つ（`EBUSY`）。lease は generation に結ぶ。 |
| `release` | lease を外し、`current`・`pending` の image の参照を落とす。program の `held` はその場で取り上げ（release で知らせる）。抜けた後の connector は release で空きに戻る。 |
| image の寿命 | core の `gpu_close` は全ての `resource_destroy` を backend の close（lease の終わり）より前に呼び、libvulkan も表示中の image の resource を先に消す。よって vgpu の image の object は refcount を持ち、`current`・`pending`・`held` の記録と program への fd の発行はそれぞれ自分の参照（object と、capability の `handle_get`）を持つ。`resource_destroy` は resource の参照だけを落とし、記録の参照は採用の置き換え・lease の release・close で落とす。参照を落とすのは眠れる thread の文脈だけ（venus の share の release は mutex を取る）。 |
| `present` | ungated（`DRV_GPU_DISPLAY_UNGATED`）。BLOB の image（vgpu が import した resource）だけ。大きさが mode と違えば `EINVAL`。抜けた connector は `ENXIO`、古い generation は `ESTALE`、電源断は受けて採る（表示しない＝program に渡さない。§6.6）。frame を `pending` に置き、`held` が無ければ直ちに `current` に採る。sequence を返す。 |
| `adoption_wait` | その sequence が採られるまで待つ。hold の上限を過ぎたらこの thread の中で revoke して採り、0 を返す（`ETIMEDOUT`・`EIO` は返さない。libvulkan はそれを device の喪失にする）。返す失敗は抜けの `ENXIO` と generation の `ESTALE` だけ（`EBUSY` は libvulkan で NATIVE_WINDOW_IN_USE になるので使わない）。 |
| `wait` | 採られた最大の sequence と採用の時刻（virtual clock の時刻）。`timeout_ns` は 1 秒まで、signal で `EINTR`。 |
| `events` | IRQ-safe な topology の番号の snapshot。 |
| `power` | ON・OFF・SUSPEND を記録し、program に state の変化（POLLPRI）を知らせる。OFF の間は frame を program に渡さない。 |
| `refresh` | virtual clock: 起点（create・resize・power ON の時刻）から refresh の周期で数える境界の番号。`timeout_ns` の間、次の境界まで眠る。`refresh_arm` は持たず、refresh の fence は core の refresh watcher（driver の refresh を呼ぶ thread、登録のある間だけ動く）に任せる。 |
| cursor・commit・overlay | 無し（`GPU_DISPLAY_HAS_CURSOR`・`HAS_OVERLAYS` を立てない）。compositor は cursor を image に描く。遠隔の画面には cursor が絵に入る方が素直。 |

`drv_gpu_scanout_ops`: `query_device`（roles = DISPLAY）、`constraints`（§2）、新しい `import_handle`（§9）。`import_image`（page の道）は持たない。

### 4.4 制御の口 `/dev/vgpu` の open

- open ごとに最大 1 connector（input-inject の「open ごとに 1 device、閉じれば消える」と同じ形）。1 つの program が複数の画面を出す時は複数回開く。
- close（process の終わり・落ちた時の kernel による close を含む）: 繋いでいれば抜く（§6.5）。持っていた frame を release し、渡した image の参照を落とす。

## 5. UAPI

### 5.1 新しい header `include/uapi/vgpu.h`

案は [proposed/vgpu.h](proposed/vgpu.h)。ioctl の group は `'V'`（`include/uapi` の他の header の group と重ならない）。全ての top-level の構造は `version`（`VGPU_ABI_VERSION` = 1）と `size` で始まり、ILP32 と LP64 で同じ layout、user の pointer を含まない。GPU の UAPI と同じく、kernel は copyin → 検査 → 実行 → 成功の後だけ copyout。

| request | 番号 | 向き | open | 内容 |
| --- | ---: | --- | --- | --- |
| `VGPU_INFO` | 0 | `_IOWR` | R | display node の `device_id`、node の番号（情報）、connector の数、空きの数、hold の上限（ns）、大きさ・refresh の上限と下限。 |
| `VGPU_CREATE` | 1 | `_IOWR` | W | 仮想の monitor を挿す。in: `connector`（番号か `VGPU_CONNECTOR_ANY`）、幅・高さ（64〜4096。venus の image の export は 4096 を超えると断る。他の renderer の上限が大きくても、compositor の swapchain が作れずに head が limited になる事を避けるため v1 は 4096 で揃える）、`refresh_millihz`（1000〜240000、0 は 60000）、物理の mm（0 は不明）、`flags` 0。out: `connector`、`display_id`、`generation`、display node の `device_id`。既に繋いだ open は `EBUSY`、空き無し `ENOSPC`、指名の connector が使用中か前の lease が残る `EBUSY`。 |
| `VGPU_DESTROY` | 2 | `_IOW` | W | 抜く（close と同じ。fd を残して作り直す時用）。 |
| `VGPU_RESIZE` | 3 | `_IOWR` | W | 大きさ・refresh・物理の mm を変える（遠隔の client の窓の大きさの変化）。generation と topology の番号が進み、compositor には monitor の差し替えに見える。out: 新しい `generation`。 |
| `VGPU_STATE` | 4 | `_IOWR` | R | 状態の snapshot: 繋がり・lease の有無・電源・`generation`・採られた最新の sequence・program に渡した最新の sequence・image の epoch。待たない。 |
| `VGPU_FRAME_ACQUIRE` | 5 | `_IOWR` | W | 前に受け取ったより新しい採られた frame を受け取り hold する。`timeout_ns`（0 は見るだけで `EAGAIN`、上限は 1 秒、signal で `EINTR`）。out: `sequence`、`skipped`（受け取らずに過ぎた frame の数）、採用の時刻、`image_id`、`image_epoch`、`state`、image の記述（`struct gpu_image_descriptor`: 幅・高さ・stride・offset・format・allocation の大きさ・exporter の `device_id`・`usage`・`memory_type`）、`fd`。`fd` は、この open がその `image_id` をまだ受け取っていない時（epoch が変わった後を含む）か `VGPU_FRAME_FD` を指定した時だけ新しい descriptor、それ以外は -1。flag `VGPU_HANDLE_CLOEXEC`・`CLOFORK`。前の frame を release していない時と、取り上げられた frame の release（応答）をまだしていない時は `EBUSY`。 |
| `VGPU_FRAME_RELEASE` | 6 | `_IOWR` | W | hold した frame を返す。in: `sequence`。out: `result`（`VGPU_RELEASE_REVOKED`: 上限を過ぎて取り上げられていた＝読みの間に書き換わった可能性）。 |

poll（`/dev/vgpu` の fd）: `POLLIN`＝受け取っていない新しい採られた frame がある、`POLLPRI`＝状態（lease・電源・generation・epoch）が変わった（`VGPU_STATE` で読む）、`POLLHUP`＝vgpu の display node が失われた（起きない想定、防御）。program は専用の thread で poll か `VGPU_FRAME_ACQUIRE` の待ちを使う（ユーザーの決定、前の案の「専用の thread で次の frame を待つ」）。

image の epoch と fd の再送: vgpu は open ごとに「渡した `image_id` の集合」を持つ。compositor がその image の vgpu 側の import を消した時（swapchain の作り直し・head を閉じた）、epoch を進めて集合を空にし `POLLPRI` で知らせる。program は epoch が変わったら自分の import（`VkDeviceMemory`・`VkImage`）を全て捨てる。次の acquire で新しい fd を受け取る。fd を frame ごとに作らないので、毎 frame の import の費用が無い。

### 5.2 `include/uapi/gpu-display.h` への追加

- `GPU_DISPLAY_REMOTE 4096U`: 「出力の frame は monitor でなく、この machine の program が受け取る（`/dev/vgpu` で挿した出力）」。`GPU_DISPLAY_VIRTUAL_CLOCK`（venus も立てる、時計が仮想である事。docs の「no scanout of its own」の説明もこちら）とは別の意味。venus の出力（QEMU の窓）は利用者に見える monitor なので立てない。
- docs（`docs/reference/gpu-display.md` の flag の表、`gpu-drivers.md` の node の表に vgpu の行、新しい `docs/reference/vgpu.md`）を同じ Phase で書く。

GPU の node の request は増やさない。

## 6. 流れ

### 6.1 挿す（create）

1. program: `open("/dev/vgpu", O_RDWR)` → `VGPU_CREATE`（例 1920×1080、60 Hz、物理 527×296 mm）。
2. vgpu: 空いた connector を取り、mode を記録、`generation` を進め、topology の番号を進めて `drv_gpu_display_changed`。
3. compositor: hotplug の thread が fence の signal で起き、新しい fence を登録してから main に知らせる → main が列挙 → 新しい display（名前の key の種類 `virtual`）を head として開く（extended なら displays.conf の位置か最右、mirror なら desktop の複写）。

### 6.2 compositor の swapchain と present

1. libvulkan: 表示の constraints に COPY が無いので共有の route（linear の image、format の確かめ）を選ぶ。swapchain の image を renderer の node で shareable・cross-device の blob に作り、resource の形で export。
2. vgpu の node に `GPU_RESOURCE_IMPORT`（`GPU_IMPORT_SCANOUT`）→ core は別の node の capability と見て vgpu の `import_handle` を呼ぶ（§9）→ vgpu は capability の参照を持つ resource を作る（`resource_id` 0、`image_id` を振る）。
3. claim → present（BLOB）→ vgpu は §6.4 の規則で採り、present を返す。

### 6.3 program の受け取り

1. program の frame の thread: poll（`POLLIN`）か `VGPU_FRAME_ACQUIRE`。
2. 受け取った `fd` があれば、自分の Vulkan の device（同じ render node、`device_id` で選ぶ）で `vkAllocateMemory` ＋ `VkImportMemoryFdInfoKHR`、記述どおりの `VkImage`（linear、format、extent）を作り bind。`image_id` で cache。
3. GPU で自分の image へ copy か変換（encoder の入力の YUV への変換など）→ その完了を待つ → `VGPU_FRAME_RELEASE`。
4. encode（Vulkan Video、CPU の差分の符号化など、program の自由）→ 送る・file に書く。

### 6.4 採用の規則（compositor と program の間）

- present（k+1）の時、program が何も hold していなければ k+1 は直ちに `current`（採用）。前の `current`（k）の image は compositor に返る（present の return）。
- program が k を hold している時、k+1 は `pending`。program の release で直ちに採用。hold が上限を過ぎたら vgpu は hold を取り上げて採用し、program の release に `REVOKED` を返す。
- program が受け取らない間に採られた frame は `skipped` に数える（遅い program は最新だけを受け取る＝latest-wins。問い 3）。
- 採用の時刻は採った瞬間の時刻（virtual clock の境界に丸めない）。`GPU_DISPLAY_WAIT` の `present_time_ns` もこれ。refresh の境界は `GPU_DISPLAY_REFRESH` と refresh の fence で別に出す。

### 6.5 抜く（program の close・落ち・destroy）

1. vgpu: connector を「抜けた」にする（`CONNECTED` を消す）、`generation`・topology の番号を進めて `drv_gpu_display_changed`。program の hold・渡した image を落とす。待っている adoption・wait・refresh の待ちを `ENXIO`・`ESTALE` で起こす。
2. compositor: その head の present・wait は `ENXIO`・`ESTALE` で失敗（libvulkan は `VK_ERROR_SURFACE_LOST_KHR`・`OUT_OF_DATE`）。hotplug の thread の知らせで列挙し直し、display が消えた head を閉じる（swapchain を消し、lease を release、vgpu の node の import を消す）。これは今の monitor の抜きの道と同じ（WS353 p004 の QEMU 試験で venus の head の抜き差しを確かめた道）。
3. connector は compositor の release で空きに戻る（lease の残る間は同じ connector への指名の create は `EBUSY`、`ANY` は他を取る）。compositor が release しないまま落ちた時は、compositor の open の close で lease が終わり空く。

### 6.6 その他の変化

- 大きさの変更（`VGPU_RESIZE`）: 抜き差しと同じく generation が進み、compositor は head を新しい大きさで開き直す。program は epoch の変化と新しい記述を受ける。
- 電源（compositor の消灯）: `POWERED_OFF`、program に `POLLPRI`。OFF の間は present を採るが program に渡さない（program は「画面が消えた」を遠隔に伝えられる）。
- compositor が落ちた・lease を外した: program に `POLLPRI`（lease 無し）。program は待ち続ける（compositor の再起動で再び claim される）。

## 7. frame の受け取りと image への access

- 道は Vulkan だけにする（kernel が CPU の map を program に出す道は作らない）。理由: image が VRAM・圧縮・cache の属性を持ちうる事を知るのは render driver と libvulkan だけ。program は同じ GPU の Vulkan で copy・変換し、CPU で読みたい program（VNC のような差分の符号化）は host-visible の buffer へ `vkCmdCopyImageToBuffer` して map する（1 回の GPU の copy。差分の検出は CPU で前の frame と比べる）。
- 変わった領域（damage）は渡さない。今の present の ABI に damage が無く（`gpu-display.md`「A partial update or damage rectangle does not exist」）、compositor の present の側を変える範囲が大きい。要る時は後の WS で present に damage を足す（`GPU_DISPLAY_COMMIT` の拡張か）。
- Vulkan Video で encode する program: acquire → compute で BGRA → NV12（自分の image）→ release → `VK_KHR_video_encode_h264`・`h265` の encode。libvulkan にはまだ encode が無い（encoder の WS の作業）。この WS の口は encode の手段を前提にしない（ユーザーの決定）。
- 同じ GPU の前提: compositor の image は compositor の renderer の memory。program はその renderer の node を開く（記述の `device_id` で選ぶ）。他の GPU の encoder（例 RTX の NVENC で、描画は i915）を使う場合の GPU の間の copy は範囲外（ユーザーの決定: 同じ GPU の device で開く前提）。

## 8. compositor から見た扱い

- 新しい display の発見・消失: WS353 の hotplug の fence が vgpu の node にも登録される（libvulkan は renderer に結んだ全ての display node に登録）。vgpu の node が起動時から居れば（§3.1 A）compositor の変更は要らない見込み。
- 仮想の印: compositor は名前の key の種類 `virtual` で見分ける（`zedbsd-port-v1:virtual:vgpu:N`）。使い道: anchor（machine 自身の display）に選ばない、displays.conf の位置を connector ごとに覚える、Settings で「仮想」と出す。key の扱いは compositor の `displays.c`・`output-switch.c` の key の解釈に足す小さな変更（p008）。`GPU_DISPLAY_REMOTE` は Vulkan だけの compositor には届かないが、UAPI の program（試験・道具）には要る（問い 8）。
- headless（物理の画面の無い RDP の server）: 今の復旧の道は失った output を「元の display → machine 自身の display → 他の接続中のどれか」へ移し（`output-switch.c`）、head は anchor が開いている時だけ開く。p008 で、最初の選択・復旧・Settings の 3 か所で「物理の display がある時は仮想の display を anchor にしない」とする。物理の display が 1 つも無い時に仮想の display を anchor にしてよいかは問い 7。
- head の動き: 仮想の display は head（mirror か extended）。extended の head は今は壁紙だけで窓を置けない（ws113-p007 が未）。遠隔の画面で作業するには mirror か、窓の移動の実装が要る（問い 4）。
- 遅い program の影響: §6.4 の上限で vgpu の head の present が待つのは最大 8 ms。§3.3 のとおり p006 で display の present の worker を swapchain ごとに分け、present の ioctl を `display_mutex` の外に出し、anchor の present がそれを待たない形にする。
- head の image の usage（レビュー R2）: head の swapchain の usage は COLOR_ATTACHMENT と TRANSFER_DST で、direct chain の linear の image は TRANSFER_SRC を足すだけで SAMPLED を持たない。libvulkan の image の capability の import（dedicated の確かめ）は SAMPLED の無い記述を断るので、program は import できない。p006 で、共有の route の linear の image に SAMPLED を足す（compositor の usage に足すだけで描画は変わらない）か、dedicated の確かめを TRANSFER_SRC でも通す。program は記述の `usage` と同じ usage で自分の VkImage を作る（docs に書く）。
- cursor: vgpu は cursor plane を持たないので compositor は cursor を image に描く。

## 9. 枠組みの変更（HAL は変えない）

HAL の API（`include/hal/hal.h`）: 変えない。vgpu は hardware に触らず、kernel の cdev・timer・lock・handle の既存の API だけを使う。

driver の枠組み（`include/drivers/gpu/gpu-scanout.h`・`src/drivers/gpu/gpu.c`、HAL ではなく承認の規則の対象外だが、全 GPU driver の契約なので Q1 に報告して進める）:

- `struct drv_gpu_scanout_ops` に optional の op を足す:
  `int (*import_handle)(void *private, void *session, const struct gpu_image_descriptor *image, struct kernel_handle *source, void **object);`
  意味: 別の node の resource の形の capability を、page を読まずに受ける。driver は表示の hardware を持たず image を読まない事を宣言する。成功で driver は自分の参照（`handle_get`）を持つ resource を返し、`resource_destroy` で落とす。core は今の foreign の import と同じく source の capability を resource の寿命の間 pin する。
- core（`gpu.c`）の変更点（レビュー R11 で数え直した）: (1) `gpu_import_ioctl` の入口の確かめ（SHARE も scanout の import も無いと `EOPNOTSUPP`）に `import_handle` を含める、(2) 同じ関数の foreign の分岐で、destination が `import_handle` を持てば `gpu_scanout_import`（source の page を要る）を通らずにそれを呼ぶ、(3) `gpu_ops_validate` の 3 か所（resource_destroy の要否、display-only の backend の許可、scanout ops の確かめ）、(4) `gpu_destroy_ioctl` の受け入れ、(5) `gpu_constraints_ioctl` の `GPU_SCANOUT_FOREIGN` の検査、(6) `import_image` と `import_handle` の両方を持つ backend は断る、(7) render の node の登録の知らせ（§3.1 の注）。`DRV_GPU_INTERFACE_VERSION` を 10 にする（scanout ops 自体には size の版が無いので、版で古い driver を見分けるのは interface の版だけ）。
- capability を program の process に fd として置く: `gpu.c` の `gpu_export_install` と同じ手順（`filedesc_reserve_many` → copyout → `filedesc_commit_objects`）。vgpu の中に置くか、core に `drv_gpu_handle_install` として出すかは p003 で決める（推しは core の共通の関数。export と同じ「copyout の後に descriptor を確定」の約束を一か所に保つため）。

UAPI: §5（新しい `vgpu.h`、`gpu-display.h` の flag 1 つ）。

## 10. 権限と security

- `/dev/vgpu` を開ける人が「画面の内容を受け取り、compositor の image を書ける」人。program は frame の capability を **自分の render node の open に import** しなければ読めないので、render node を開ける事も要る（レビュー R4）。今の sessiond はログインの間、`/dev/gpu0`〜`/dev/gpu3`（render node を含む）を seat の user の 0600 に渡し、`gpu_open` は mode が group に読み書きを与えない時は group を認めないので、その間 video group の service は render node を開けない。
- 推し（問い 2）: `/dev/vgpu` は GPU の node と同じ扱いにする: 平時は root・video・0660、graphical session の間は sessiond が他の GPU の node と一緒に seat の user に渡す。画面を共有する program は session の中で seat の user として動かす（compositor と同じ user）。login の無い machine の RDP の server は root か video group で動く（session が無い間は node が video の 0660 のまま）。
- vgpu の display node（`/dev/gpuN`）は普通の GPU の node と同じく devfs が root・video・0660 で作り、sessiond が seat の user に渡す。今の sessiond は `/dev/gpu0`〜`/dev/gpu3` だけを渡すので、全ての GPU node（数を数えて）を渡す形に直す（p007）。
- program が受け取る fd は CLOEXEC を推す（既定は呼び手の flag どおり、GPU の export と同じ）。
- 制御の open は 1 process が開ける数を制限しない（connector の数 4 が全体の上限）。

## 11. 異常系

| 事象 | 扱い |
| --- | --- |
| program が落ちる・kill -9 | kernel の close → §6.5 の抜き。compositor は monitor を抜いた時と同じ。 |
| program が acquire したまま止まる | 上限（8 ms）で adoption_wait の thread が revoke、compositor は進む。connector は「取り上げ済み・未応答」の状態を持ち、program の次の `VGPU_FRAME_RELEASE`（その sequence）が `REVOKED` を受け取るまで `VGPU_FRAME_ACQUIRE` は `EBUSY`（古い sequence の release が新しい hold を外す誤りを防ぐ）。止まり続ける program は frame を受け取れないだけ。 |
| program が acquire しない | present は直ちに採られる。compositor は待たない。 |
| compositor が落ちる | compositor の open の close で lease が終わり、vgpu は image を落とす。program は `POLLPRI`（lease 無し）。 |
| compositor が image を消した（swapchain の作り直し） | epoch を進め、program の hold が消された image ならその場で取り上げる（`REVOKED`）。 |
| resize の途中の present | 古い generation は `ESTALE`。 |
| 4 つ全てが使用中 | create は `ENOSPC`。 |
| node の登録の前（render node が来ていない） | `/dev/vgpu` の create は `ENXIO`（`VGPU_INFO` は connector 0）。 |

## 12. 試験の形

AGENTS.md §9 に従い、QEMU の console・serial の log で判定しない。guest の操作は SSH（`plan/tools/guest/guest.sh`）、判定は試験の program の出力の file と exit code。

- host: `include/uapi/vgpu.h` の layout の試験（ILP32・LP64 の offset と size、`plan/ws341/tests/host-vgpu-layout.c`）。
- QEMU（Venus）`plan/ws341/tests/qemu-vgpu-loop.sh`（1 本の script: image の build → 起動 → 試験 → 回収 → 要約）。compositor 役と受け手役を **別の process** にする（受け手の kill -9 が compositor 役を巻き込まないため、レビュー R13）。compositor 役は `VK_KHR_display` で vgpu の display に swapchain を作り present する Vulkan の program（`userland/tests/vkdemo` の display の道と同じ形）で、libvulkan の本物の共有の route の image を通す（R2 の usage を拾うため。raw の ioctl で venus の blob を作る道は host の blob の id が要り使わない）。受け手は `vgpu-loop`:
  1. `VGPU_CREATE` → vgpu の node の topology の変化（`GPU_DISPLAY_EVENTS` と `GPU_DISPLAY_FENCE` の HOTPLUG の signal）、query の flag・名前・mode、空きの connector の generation が 0 でない。
  2. compositor 役が既知の色の frame を 300 回 present、受け手は acquire → fd を venus の node に import → GPU で host-visible の buffer へ copy → 色と大きさを照らす。`skipped`・sequence の単調。
  3. 受け手が hold したまま止まる → present が 8 ms 程度で返る、release が `REVOKED`、未応答の間の acquire が `EBUSY`。
  4. 受け手を kill -9 → topology の変化、compositor 役の present が `ENXIO`（Vulkan の SURFACE_LOST）で compositor 役が落ちない、lease の release の後に connector が空く、同じ connector の作り直し。compositor 役を kill -9 → connector の lease が空き、受け手に `POLLPRI`。
  5. resize、power、refresh の fence（境界の間隔が周期の整数倍）。
- QEMU（Venus）の起動の時間: GPU の無い構成（venus 無し）の image で、vgpu 入りの kernel の起動から login prompt までの時間が vgpu 無しと変わらない（`plan/tools/boot-test.sh`）。
- QEMU（Venus）`plan/ws341/tests/qemu-vgpu-desktop.sh`: Keiland を起動し、道具 `vgpu-capture`（p009）で 1280×720 の仮想 display を挿す → compositor が head を開く（`vgpu-capture` が受け取る frame が壁紙の色を含み、大きさが合う）→ N frame を file（Y4M。WS329 があれば H.264 の elementary stream）に保存 → `vgpu-capture` を kill -9 → compositor の pid が変わらず、`plan/tools/boot-test.sh` の画面の確かめで desktop が出ている。 さらに、受け手が hold し続ける・止まる間、anchor（venus の主の display）の present の間隔（compositor の `KWL PERF` の frame の間隔を guest の file から）が変わらない事（レビュー R1）。
- 実機（Vulkan Video の encode）: 問い 5 の後の Phase。

## 13. 実装の Phase の表（案）

大きさは S（半日以内）・M（1 日）・L（数日）。

| Phase | 中身 | 依存 | 大きさ |
| --- | --- | --- | --- |
| p002 | UAPI: `include/uapi/vgpu.h`、`gpu-display.h` の `GPU_DISPLAY_REMOTE`、docs（`docs/reference/vgpu.md`、`gpu-display.md`・`gpu-drivers.md` の追記）、host の layout 試験 | p001 の承認（問い 1・8） | S |
| p003 | GPU の枠組み: `import_handle` の op と core の 7 か所（§9）、render node の登録の知らせ、capability を fd にする共通の関数、`DRV_GPU_INTERFACE_VERSION` 10。3 機種の build、既存の display 試験（display-scanout・gpu-share）の回帰 | p002 | M |
| p004a | vgpu driver の土台: 登録（render node の知らせ）、`/dev/vgpu` の cdev と devfs の mode・group、open の権限、connector と抜き差し、display ops の列挙・mode・claim・release・events・constraints・query_device | p003、問い 6（mk の承認） | M |
| p004b | vgpu driver の frame: present・adoption_wait・wait、hold と revoke の状態、image の参照の寿命、frame の acquire・release・poll・epoch、virtual clock と refresh・power | p004a | L |
| p005 | 試験: 受け手 `vgpu-loop` と compositor 役の Vulkan の program、`qemu-vgpu-loop.sh`、GPU の無い image の起動の時間。§12 | p004b・p006、問い 6（userland の Makefile の承認） | M |
| p006 | libvulkan: display の present を swapchain ごとの worker に分け、present の ioctl を `display_mutex` の外に（R1）、共有の route の linear の image に SAMPLED（か dedicated の確かめを直す、R2）、venus（と i915・amdrdna・nvrtx）の renderer と別の display-only node の共有の route の確かめ | p004a | L |
| p007 | sessiond: `/dev/vgpu` と、番号 0〜3 に限らない全ての GPU node を seat の user に渡す（問い 2） | 問い 2 | S |
| p008 | compositor: 名前の key の種類 `virtual`（最初の選択・復旧・Settings の 3 か所、問い 7 の headless の扱い、displays.conf の connector ごとの位置）。host の output-switch の試験の追加 | p004a、問い 7 | S |
| p009 | 道具 `vgpu-capture`（frame を Y4M、WS329 があれば H.264 の file へ）と `qemu-vgpu-desktop.sh`（anchor の frame の間隔を含む） | p005・p006・p007・p008 | M |
| p010 | Vulkan Video の encode の道の確かめ（BGRA → NV12 の変換、H.264・H.265 の encode を file へ）、実機 | 問い 5、encoder の WS（WS324 など）の libvulkan の encode | M |
| p011 | 規約の全文の見直し | p002〜p010 | S |

## 14. 危険と未確認

- 未確認: libvulkan の共有の route（COPY の無い display-only node への linear の image の export と import）は SoC の pair でしか使われていない。venus・x86 の renderer で動くかは p006 で確かめる。venus の Windows の fork の host でも、HOST_SCANOUT の capset を返す host なら `copy_display` は偽になり共有の route を取る（`libvulkan/context.c`）。返さない host では取らず、そこでは仮想の display は使えない。
- 確かめた: cross-device の flag は libvulkan が DMABUF の外部の型の image に付ける（`memory.c`、`wsi-image.c`）。amdrdna・nvrtx が direct chain を取るか、linear の format を持つかは未確認（p006）。
- 多 GPU: vgpu の node は `companion_id` 0 で、libvulkan は最小の device_id の renderer に結ぶ。compositor が別の renderer を使う machine では仮想の display が見えない。要れば後で起動の引数で companion を選べるようにする。
- libvulkan の present の worker（ordinary の lane は anchor と head で 1 本）と `display_mutex`（present の ioctl の間 process 全体）が vgpu の待ち（最大 8 ms）を anchor に伝える（R1）。p006 で直すまで、vgpu の head は物理の画面の frame を落としうる。p006 は L で、libvulkan の swapchain の作りに触れる。
- hold の上限を過ぎた frame は壊れている可能性がある。program が release の `REVOKED` を見て捨てる約束を docs に書く。
- capability は allocation 全体の書きの権利を含む（§3.2）。
- WS353 の p016（QEMU の回帰と 5330 の idle）は in-progress。hotplug の道の回帰の結果はそちらに依る。

## 15. ユーザーへの問い

[user-questions.md](user-questions.md) の 1〜8（node の形、`/dev/vgpu` と render node の権限、遅い program の扱い、仮想の display の使い方と既定の build、Vulkan Video の encode の Phase の置き場所、build system の差分の承認、headless の anchor、仮想の印を名前の key で示す事）。

## 16. 捨てた案と理由

- **B: create ごとに /dev/gpuN**（ws.md の言い方）: §3.1。選ぶ場合の追加: kernel の「GPU node の集合の変化」の知らせ（例 全 GPU node の `GPU_DISPLAY_FENCE` に新しい event の種類、か `/dev/system` の事象）、libvulkan の inventory を可変に（node の追加・withdraw の時の discovery の open の close、hotplug の登録の数の前提）、node の番号の再利用が compositor の close まで遅れる事の受け入れ、sessiond の渡し。p004a・p004b と p006 がさらに大きくなり、p006 の前に compositor の hotplug の試験の作り直しが要る。
- **vgpu が自分の memory（COPY の route）を持つ**: ユーザーの決定 4 と、copy を避ける方針に反する。
- **vgpu が物理の page を CPU で map して program に渡す**: page を出す render driver が限られ、VRAM・cache の属性を vgpu が知る事になる（§3.2）。
- **user mode の program 自身が display の op に答える（kernel は中継だけ）**: compositor の present・wait・refresh が program の応答に直に結び付き、program の停止で compositor が止まる。kernel が採用の規則と上限を持つ方が安全。
- **damage の領域を渡す**: present の ABI に無い（§7）。
- **frame ごとに新しい fd**: 毎 frame の import の費用（vkAllocateMemory の import と kernel の import）。`image_id` と epoch で cache する。
