<!-- awesome-plan project=zedbsd record=ws035-compositing-design -->

# zdesktop の合成（compositing）設計

Parent: [WS035](ws.md) / Phase: [ws035-p051](phase051/phase.md)
Status: **承認済み**（2026-09-25 ユーザー「承認します」。§10 の 1〜8 をそのまま）。2026-09-24 に提出、同日のユーザーのレビュー 2 回を反映（§0）

2026-09-24 のユーザー指示「デスクトップのコンポジットは、設計をください。…設計を出してくれれば、微調整して OK しようと思います」
による設計。コードは変えていない。§10 に承認をもらいたい点を並べた。

## 0. ユーザーのレビュー（2026-09-24）と反映

> zdesktopは2つのモードを持ちます。1つめのモードは、あるウィンドウが全画面表示の時であり、このときそのままscanoutします。
> もう1つはウィンドウモードで、この場合はウィンドウのバッファはデスクトップ上のウィンドウを下から順にVulkanでレンダリングして出力します。
> つまり、デスクトップの背景の上で、すりガラスエフェクトでウィンドウを出力するようなことも、可能です。
> つまり、全画面でゲーム用の高速なモードと、コンポジットウィンドウマネージャと、2つのモードですね。SteamOSみたいな感じです。
> wl_shmでCPU Copyとは具体的になんでしょうか？いらない気がするのですが。

反映:

- **2 つのモードを設計の骨格にした**（D0）。旧版の D5 は「合成の例外として、条件がそろった frame だけ直接 scanout」だったが、
  **全画面モード**（窓の全画面の状態で入る、そのまま scanout、ゲーム向け）と**ウィンドウモード**（背景の上に窓を下から順に
  Vulkan で描く合成 window manager）の 2 つを明示した切替にした。
- ウィンドウモードは**背景→窓を下から順**に描く。描く途中の画像を次の窓の描画が読めるので、窓の背後をぼかす
  **すりガラス**などの効果が載る構造にした（D1、D10）。
- **`wl_shm` を外した**（D2）。「CPU copy」の中身は、client が普通のメモリ（`shm_open` の匿名メモリ）に描いた画素を、
  GPU が読めるメモリへ zdesktop が CPU で memcpy（staging buffer）し、GPU の copy で窓の VkImage へ移す、という
  commit ごとの 2 段の copy のことだった。普通のメモリは GPU が sampling できないので、`wl_shm` を受ける限りこの copy は避けられない。
  zdesktop の client はすべて Vulkan（libwayland の WSI）で GPU の画像を渡すので、今は要らない。
  cursor の画像も zdesktop が自分で持つ（D8）。toolkit（GTK・Qt の software 描画）が `wl_shm` を要るかは p028 で改めて決める。

### 2 回目のレビュー（2026-09-24）

> wl_shmは主要な描画経路ではないが、サポートするならCPUコピーという意味なんですね？では、それもサポートしてください。
> ただし、主要な経路であるVulkanバッファでの高速性に対する最適化を行ってください。
> D1が理解できません。swapchainを使うのがよいのでは？全画面モードはswapchainを破棄するか停止して、直接scanoutすればいいと思います。
> 切り替えに多少時間がかかってもOKです。

反映:

- **`wl_shm` を持つ**（D2）。主要な経路ではない補助の経路で、commit ごとに CPU の copy がある。その copy は damage の範囲に限り、
  その窓だけの負担にする。**主要な経路（Vulkan の GPU 画像）は copy 0 のまま、import の使い回しなどで速さを優先する**（D2 の最適化の表）。
  cursor の theme のような `wl_shm` の画像も `set_cursor` で受ける（D8）。
- **D1 を swapchain に改めた**。旧版の D1 は「zdesktop が出力画像 3 枚を自分で確保し、`GPU_DISPLAY_PRESENT` で自分で出す。swapchain は使わない」だった。
  理由は、旧版が「frame ごとに合成と直接 scanout を切り替える」形だったためで、swapchain を使うと表示の lease を libvulkan が持ち、
  zdesktop が frame の途中で client の画像を出せない。モードの切替を窓の状態で決める形（D0）になり、切替に時間がかかってもよいので、
  この理由は無くなった。ウィンドウモードは**標準の `VK_KHR_display` の swapchain** で出し、全画面モードに入るときは swapchain と
  display の surface を破棄して lease を返し、zdesktop が表示を持って client の画像を直接 scanout する（D0、D1）。

## 1. 目的と範囲

今の `zwl` は、client が渡した画像を**そのまま scanout に出す**（全画面 1 枚、[p011 の調査](phase011/phase.md)）。
窓を任意の位置と大きさで重ねて出すには、zdesktop が自分の出力画像を持ち、各 client の画像をそこへ描いて（合成して）
その 1 枚を scanout する必要がある。この文書はその仕組みを決める。

範囲:

- 出力画像の持ち方と scanout への出し方
- 全画面モードとウィンドウモードの 2 つと、その切替
- client の画像の受け取り方（GPU の画像）と同期・寿命
- 描き方（何で合成するか）、効果（すりガラス等）を載せる構造、描き直す範囲（damage）、frame の間隔
- 窓の座標系、`xdg_toplevel` の configure、装飾と当たり判定の境界
- cursor の描き方、`/dev/graphics` との関係

範囲外（別 Phase）: 窓管理の操作そのもの（p011）、装飾の見た目（p025）、X11（p012）、タスクバー（p013）、
タイル表示（p014）、toolkit 向けの protocol の残り（p028）、複数の画面、GPU の無い機種での合成（§9）。

## 2. 前提（今あるもの）

| もの | 状態 |
| --- | --- |
| `/lib/libvulkan.so` | Vulkan 1.0 の全 137 core と direct-display WSI（WS030）。shader を使う描画は vkdemo・mview が実証 |
| GPU 画像の共有 | `VK_KHR_external_memory_fd` の OPAQUE_FD（同じ deviceUUID/driverUUID の範囲）、linear・buffer・optimal（WS014 p007） |
| 完了の通知 | `KERNEL_HANDLE_FENCE` の fd を `poll(POLLIN)` で待てる。`VK_KHR_external_fence_fd`（WS014 p007） |
| scanout | `GPU_DISPLAY_PRESENT`（`BLOB`）で、共有した linear 画像をそのまま出す。`include/uapi/gpu-display.h` に cursor の plane の interface は**無い** |
| zwl の protocol | `wl_compositor` 4、`xdg_wm_base` 1、`zed_gpu_buffer_v1` 1（fd と画像の記述）、`wl_output` 2、`wl_seat` 5。**`wl_shm` は無い** |
| event loop | `poll()` 1 本の single thread（epoll は使わない。ws034-p050 で POSIX の範囲を確かめる） |

## 3. 決定の提案

### D0. 2 つのモード: 全画面モードとウィンドウモード

zdesktop は表示の出し方を 2 つ持ち、状態で切り替える（SteamOS の gamescope のような構成）。

| | 全画面モード | ウィンドウモード |
| --- | --- | --- |
| 入る条件 | ある窓が全画面の状態（`xdg_toplevel.set_fullscreen`、または利用者の操作）で、最前面にある | それ以外（既定） |
| 出し方 | その窓の client の画像を**そのまま** `GPU_DISPLAY_PRESENT`（`BLOB`）。合成しない、copy 0 | 背景の上に窓を下から順に Vulkan で描き、`VK_KHR_display` の swapchain で出す（D1） |
| 表示の持ち主 | zdesktop が `GPU_DISPLAY_CLAIM` で持つ | libvulkan の display の surface が持つ |
| 用途 | ゲーム・動画・vkdemo など、速さと遅延が大事なもの | 合成 window manager（重なり、装飾、効果、タイル表示） |
| frame の間隔 | client の present に合わせる（今の zwl の直接表示と同じ） | 変化があったときだけ、swapchain の FIFO で表示 1 回につき最大 1 回（D4） |
| cursor | 描かない（下の「全画面モードの cursor」） | 合成する（D8） |
| 装飾・タスクバー | 出さない | 出す |

- **入る**: 全画面の窓が最前面になり、その client の画像が scanout できる形（linear、scanout できる format、出力と同じ大きさ、GPU の画像）のとき。
  手順: 描画を止める → `vkDeviceWaitIdle` → swapchain と display の surface を破棄（libvulkan が `GPU_DISPLAY_RELEASE`）→
  zdesktop が `GPU_DISPLAY_CLAIM` で表示を持つ → client の画像を `GPU_RESOURCE_IMPORT`（scanout）して `GPU_DISPLAY_PRESENT`。
- scanout できない形の全画面の窓（optimal の画像、`wl_shm` の画像、大きさ違い）は、全画面モードに入らずウィンドウモードのまま、
  その窓 1 枚を全画面の大きさに描く（装飾・タスクバーは出さない。copy は GPU の 1 回）。
- **出る**: 全画面の解除、別の窓へ focus を移す操作（Alt+Tab・Windows+Tab の p014）、窓の close、scanout できない画像の commit。
  手順: `GPU_DISPLAY_RELEASE` → display の surface と swapchain を作り直す → 次の frame からウィンドウモードで描く。
- 切替には swapchain の破棄・作成の時間がかかってよい（ユーザーのレビュー）。切替の間は client の frame callback を止め、切替後に再開する。
- **切替の隙間**: release から claim までの間、[graphics-design.md](graphics-design.md) §5.1 の調停で文字 console が一瞬出る恐れがある。
  p052 で実機・QEMU の画面の読み取りで確かめる。出るなら、同じ process の claim が lease を引き継ぐ形（release と claim の間に console へ戻さない）を
  GPU の display の interface に足す（HAL ではなく GPU の UAPI の変更。設計を p052 で出す）。
- **全画面モードの cursor**: hardware の cursor plane が無いので、cursor を出すには合成が要り、全画面モードの意味（copy 0）と両立しない。
  全画面モードでは cursor を描かず、pointer の event は client へ送る（ゲームは自分で cursor を描くか、相対移動を使う）。
  cursor が要る全画面の窓は、利用者がウィンドウモードの最大化で使う。cursor の plane を driver が持ったら変える（§9）。
- 通知などを全画面モードの上に重ねたくなったら、その間だけウィンドウモードで描く（今は通知が無いので後回し）。

### D1. ウィンドウモードは Vulkan で合成し、`VK_KHR_display` の swapchain で出す

- zdesktop は **Vulkan の client** になる（`/lib/libvulkan.so`）。GPU に 2D 合成の専用経路は足さない。
  Vulkan で足り、driver ごとに 2D 経路を持つより保守が少ない。
- 出力は**標準の swapchain**（`VK_KHR_display` の display plane の surface、`VK_KHR_swapchain`、FIFO、画像 3 枚）。
  vkdemo と同じ標準の経路で、表示の間隔・画像の順番・display の event（topology の変化）を libvulkan に任せる。
  swapchain の画像は libvulkan が作成時に 1 回だけ選ぶ経路（WS014 p007: 共有の linear 画像を BLOB で scanout）で出る。
- 全画面モード（D0）では swapchain と surface を破棄し、zdesktop 自身が表示を持つ。frame ごとの切替はしない。
- 描く順は **背景（壁紙）→ 窓を z-order の下から順に（装飾・影を含む）→ タスクバー → cursor**。
  1 frame は 1 本の command buffer で、出力画像へ順に描き重ねる（painter's algorithm）。
- 描き方は **textured quad**（頂点 4 つ、vertex shader で位置・大きさ、fragment shader で sampling と alpha）。
  `vkCmdBlitImage` だけでは拡大縮小つきの半透明（タイル表示 p014、影、cursor）が描けないため、最初から quad にする。
  不透明な窓は blend を切った pipeline、装飾・cursor は alpha blend の pipeline の 2 本から始める。
- 窓ごとに「描き方」を持てる形にする（既定は不透明の quad）。すりガラスのように**それまでに描いた画像を読む効果**は、
  その窓を描く直前に render pass を区切り、出力画像のその窓の範囲を読んで加工してから窓を重ねる（D10）。
  下から順に描くので、窓の背後にあるもの（背景と下の窓）はその時点で出力画像にそろっている。

### D2. client の画像: GPU の画像（主要、copy 0）と `wl_shm`（補助、CPU の copy あり）

**GPU の画像**（`zed_gpu_buffer_v1`、libwayland の WSI が使う主要な経路）:

- ウィンドウモードでは Vulkan で sampling するので、fd を **`vkAllocateMemory`（`VkImportMemoryFdInfoKHR`、OPAQUE_FD）＋
  `vkCreateImage` / `vkBindImageMemory`** で import する。画像の記述（幅・高さ・format・tiling・stride）は protocol の record にある。
- 全画面モードでは同じ fd を `GPU_RESOURCE_IMPORT`（scanout）して `GPU_DISPLAY_PRESENT` する。
- どちらのモードでも画素の copy は無い。

主要な経路の速さのための最適化（p052 で入れる）:

| 最適化 | 内容 |
| --- | --- |
| import は buffer ごとに 1 回 | `wl_buffer` を作るとき（fd を受けたとき）に VkImage・VkImageView・descriptor set まで作り、commit のたびには作らない。WSI の swapchain は画像 2〜3 枚を回すので、定常状態では commit に割り当ても ioctl も無い |
| 全画面モードの import も 1 回 | 全画面モードに入った時点で、その client の buffer をまとめて scanout 用に import し、以後の present は `GPU_DISPLAY_PRESENT` だけ |
| 1 frame 1 submit | 全部の窓を 1 本の command buffer・1 つの render pass で描く（すりガラスの窓だけ pass を区切る、D10）。pipeline・sampler・descriptor の layout は起動時に作る |
| CPU で待たない | client の acquire fence・zdesktop の sampling の fence はどちらも fd にして poll に入れる（D3）。event loop は止まらない |
| 描かない frame は描かない | 変化が無ければ swapchain に present しない（D4）。窓が 1 つ動かない間の GPU の負担は 0 |
| release を早く返す | sampling の fence が signal されたらすぐ `wl_buffer.release` を送り、client の swapchain が詰まらないようにする |

**`wl_shm`**（toolkit の software 描画、cursor の theme、小さな client のための補助の経路）:

- `wl_shm` 1（format は ARGB8888・XRGB8888）と `wl_shm_pool` を持つ。client の fd は `shm_open` 直後に `shm_unlink` した匿名のもの
  （audiod の決定と同じ方法）。zdesktop は pool を作ったときに 1 回だけ mmap する（`wl_shm_pool.resize` で mmap し直す）。
- commit のたびに、**damage の範囲の行だけ**を CPU で copy する。copy 先は窓ごとに持ち続ける、GPU が読める staging buffer
  （HOST_VISIBLE、persistent map）。その frame の command buffer の先頭で `vkCmdCopyBufferToImage`（damage の範囲だけ）して窓の VkImage へ移す。
- CPU の copy が終わった時点で `wl_buffer.release` を送る（client はすぐ次を描ける。GPU の完了を待たない）。
- sampling できる linear・HOST_VISIBLE の画像が使えるなら、staging を省いてそこへ直接 copy し、copy を 1 回にする（p053 で測って選ぶ）。
- `wl_shm` の窓は全画面モードに入らない（D0。scanout できない）。copy の負担はその窓だけに閉じ、GPU の画像の窓には影響しない。

### D3. 同期と寿命

- **client の描画の完了**: GPU の画像は、commit のときに client の描画がまだ終わっていないことがある。
  `zed_gpu_buffer_v1` に **acquire fence（`KERNEL_HANDLE_FENCE` の fd）を commit と一緒に渡す request** を足す
  （Linux の `linux-explicit-synchronization` 相当を最小限で）。zdesktop はその fd を poll に入れ、
  signal されてから、その commit を「使える状態」にする。**CPU で wait はしない**（event loop が止まる）。
  fence を渡さない client は今までどおり、commit の時点で描画が終わっている前提（今の WSI の動き）。
  （p054 で実装: version 2 の `set_acquire_fence(surface, fd, generation_hi, generation_lo)`、1 つの commit に 4 つまで。
  WSI の present は元から worker が完了を待つ形で、`vkQueuePresentKHR` 自体は速くならない。[phase054](phase054/phase.md)）
- **zdesktop の sampling の完了**: 出力 frame ごとの submit に VkFence を付け、`vkGetFenceFdKHR` で fd にして poll に入れる
  （swapchain の acquire・present の同期は標準の semaphore）。
  signal されたら、その frame が sampling していた client の画像のうち、もう新しい commit で置き換わったものへ
  `wl_buffer.release` を送る。
- **寿命**: client が `wl_buffer.destroy` しても、使用中の frame の fence が signal されるまで zdesktop の
  VkImage・VkDeviceMemory は残す（参照数）。client の切断も同じ。

### D4. 描き直す範囲と frame の間隔

- **変化が無ければ描かない**。描くきっかけは commit、窓の移動・z-order の変化、cursor の移動、装飾の状態の変化。
- 1 回目の実装は**全画面を描き直す**（swapchain の画像を毎回全部）。単純で、正しさを先に固める。
- 2 回目で **damage**: surface の `damage_buffer`、窓の移動前後の矩形、cursor の前後の矩形を合わせ、
  swapchain の画像ごとの「前に描いてからの経過」（buffer age。画像の index ごとに zdesktop が数える）の分だけ union を取り、scissor で限る。
  `wl_shm` の upload の範囲も同じ damage で限る（D2）。
- 描くのは**表示 1 回につき最大 1 回**。swapchain の FIFO の acquire が次の表示を待つ。全画面モードは `GPU_DISPLAY_PRESENT` の完了（今の `zwl_schedule` の仕組み）で間隔を取る。
  `wl_surface.frame` の callback は、その surface を含む frame が出たときに送る（今と同じ意味）。

### D5. （D0 へ統合）

旧版の「全画面 1 枚のときの直接 scanout を残す」は、D0 の全画面モードになった。旧版は「見える surface が 1 つで不透明、
cursor が隠れている」などの条件を frame ごとに調べて自動で切り替えていたが、窓の全画面の状態で明示的に切り替える形に改めた。
これで cursor が出ているだけで合成へ落ちることが無くなり、ゲームの frame の間隔が安定する。

### D6. 座標系と configure

- 全体の座標は**出力の pixel**（出力 1 つ、`wl_output` の scale 1）。窓は出力上の位置 `(x, y)` を持ち、大きさは
  commit された buffer の大きさ（`attach` の dx・dy を反映）。
- `xdg_toplevel.configure`:
  - 通常: 最初は `0×0`（client が決める）。以後、利用者が大きさを変えたときだけ新しい大きさを送る。
  - 最大化: 出力からタスクバー（p013）の高さを除いた大きさ、state `maximized`。
  - 全画面: 出力の大きさ、state `fullscreen`。最前面なら全画面モード（D0）。
  - resize の grab 中は state `resizing` で、pointer が動くたびに新しい大きさを送る。client が ack して commit した大きさで描く
    （ack 前の古い大きさの buffer は、古い大きさのまま描く）。
  - focus のある窓に state `activated`。
- 新しい窓の位置は、出力の中央から少しずつずらす（cascade）。
- subsurface・popup（`xdg_popup`）は p028。

### D7. 装飾と当たり判定は zdesktop が持つ（server-side decoration）

- 題名の帯・枠・閉じる／最大化／最小化の button は zdesktop が描く（見た目は p025、文字は p027 で libtruetype）。
  `xdg-decoration` は p028 で足し、既定を server-side にする。
- 当たり判定: 装飾の領域（題名の帯＝移動、枠と角＝resize、button）は zdesktop が処理し、client の領域だけを
  `wl_pointer` で client へ送る。`xdg_toplevel.move`・`resize` の request（client-side decoration の client から）も受ける。
- 装飾は窓ごとの小さな画像にして quad で描く（D1 の alpha の pipeline）。

### D8. cursor

- cursor の plane が無いので、**ウィンドウモードでは cursor を合成する**（最後に alpha の quad）。全画面モードでは描かない（D0）。
- 既定の cursor の画像は **zdesktop が自分で持つ**（矢印、移動、resize の向き、文字入力の I 字など数種類。p025 の見た目と合わせる）。
  装飾の上・窓の外ではこれを出す。
- client が `wl_pointer.set_cursor` で渡す surface は、`wl_shm` の画像（toolkit の cursor の theme はこれ）も GPU の画像も受ける。
  cursor は小さいので `wl_shm` の copy の負担は無視できる。buffer の無い surface は cursor を隠す（protocol の意味どおり）。
- cursor だけが動いた frame は、D4 の 2 回目の damage で cursor の前後の矩形だけを描き直す。1 回目の実装では全画面を描き直す
  （1080p で quad 数個なので、GPU には軽いと見込む。p052 の実装で測る）。

### D9. `/dev/graphics` との関係

[graphics-design.md](graphics-design.md) の §5.1 の調停のまま変えない。zdesktop が `GPU_DISPLAY_CLAIM` で表示を持つ間、
文字 console は出ない。zdesktop が終了・異常終了すると表示が console に戻る（WS014 で確認済みの動き）。

### D10. 効果（すりガラス等）を載せる構造

ウィンドウモードは背景から下から順に描くので、窓を描く時点でその背後の画像が出力画像にある（D1）。これを使う効果を、
窓ごとの「描き方」として後から足せるようにする。最初の実装（p052）では効果を作らず、構造だけを用意する。

- **すりガラス（背後のぼかし）**: その窓の範囲（＋ぼかしの半径）の出力画像を別の画像へ copy → 縮小 → 横と縦のぼかし
  （compute か fragment の 2 pass）→ 拡大して窓の範囲へ描く → その上に窓を alpha で重ねる。窓は半透明（ARGB）で描く。
  窓ごとに render pass を区切る必要があるので、効果のある窓の数だけ pass が増える。
- **影・角の丸め**: 窓の quad の外側に影の quad、fragment shader で角を切る。背後を読まないので pass を区切らない。
- **タイル表示の縮小**（p014）: 窓の quad を縮小して並べるだけ（D1 の quad で足りる）。
- 効果は窓の属性（zdesktop の設定、または将来の protocol）で決め、client は関与しない。damage（D4 の 2 回目）では、
  すりガラスの窓の背後が変わったらその窓の範囲も描き直す（ぼかしの半径の分だけ広げる）。

## 4. 部品の分け方（file）

`userland/base/zwl` を `userland/base/zdesktop`（`/bin/zdesktop`）へ改名したうえで（p011）:

| file | 役割 |
| --- | --- |
| `compose.c` | Vulkan の初期化、display の surface と swapchain（作成・破棄）、pipeline 2 本、frame を描く（quad の列を作って submit） |
| `import.c` | `zed_gpu_buffer_v1` の OPAQUE_FD の import（buffer ごとに 1 回）と寿命、acquire fence |
| `shm.c` | `wl_shm`・`wl_shm_pool`、damage の範囲の copy と upload |
| `scene.c` | 窓の列（z-order）、位置・大きさ、damage、モード（D0）の判定と切替 |
| `display.c` | 全画面モードの表示（claim・import・present・release）、モードの切替の手順、frame の間隔 |
| `cursor.c` | zdesktop の cursor の画像と、`set_cursor` の GPU の surface |
| `effect.c`（p057） | すりガラスなど背後を読む効果の pass |
| 既存（`protocol.c`・`seat.c`・`input.c`・`wire.c`・`objects.c`） | 当たり判定・configure の追加以外はそのまま |

## 5. 実装の順序（Phase の提案）

承認後、p011 の前に次の Phase を置くことを提案する。

| 案 | 内容 | 受入 |
| --- | --- | --- |
| p052 | 2 つのモードの核: Vulkan の初期化、`VK_KHR_display` の swapchain、背景と quad の pipeline、ウィンドウモードの全画面の描き直し、全画面モードとの切替（D0。swapchain の破棄・作り直しと claim、切替の隙間の確認）、GPU 画像の OPAQUE_FD import（buffer ごとに 1 回）、sampling の fence と release、効果を載せる窓ごとの描き方の枠（D10、効果そのものは無し） | QEMU（Venus）で wltest 2 つを背景の上の別の位置に同時に表示、画面の読み取りで背景と 2 つの画像の位置が合う。1 つを全画面にすると全画面モードに入り直接 scanout になる（trace で copy 0）、解除でウィンドウモードに戻る |
| p053 | `wl_shm`（damage の範囲の copy と upload、早い release）と cursor（zdesktop の cursor 画像、ウィンドウモードでの合成、`set_cursor` の shm・GPU の surface、全画面モードで描かない） | shm の client の画像と cursor が正しく出る。GPU の画像の窓の frame 時間が shm の窓の有無で変わらない（測定）。全画面モードで cursor が出ず copy 0 のまま |
| p054 | acquire fence の request（D3） | 描画中に commit しても、描き終わった画像だけが出る |
| p011 | 改名と窓管理（focus、移動、resize、z-order、最小化・最大化）、装飾の当たり判定（見た目は仮） | 既存の受入条件 |
| p055 | damage（D4 の 2 回目） | cursor の移動で描く面積が cursor の周りだけになる（測定） |
| p057 | 効果: すりガラス（背後のぼかし）、影（D10） | 背景の上の半透明の窓の背後がぼけて出る（画面の読み取りで、ぼかした背景と一致）。全画面モードには影響しない |

## 6. 性能の見込み

- 1920×1080 の出力を全部描き直すと、1 frame で約 8 MB の書き込みと窓の分の読み出し。Venus（host の GPU）でも i915 でも
  60 Hz に十分と見込む。p052 で frame の時間を測り、足りなければ p055 の damage を先にする。
- GPU の画像の窓は copy 0（ウィンドウモードは sampling、全画面モードは scanout）。定常状態の commit に割り当ても import も無い（D2）。
- CPU の copy は `wl_shm` の窓の damage の範囲だけ。1080p の窓全体が毎 frame 変わると約 8 MB/frame の memcpy になるが、その窓だけの負担。
- モードの切替は swapchain の破棄・作成を含むので数十〜数百 ms かかりうる（ユーザーが許容）。p052 で測る。
- すりガラスは効果のある窓ごとに縮小・ぼかしの pass が増える。縮小してからぼかすので、1080p の窓 1 枚で出力 1 枚の描き直しより軽いと見込む。p057 で測る。

## 7. 失敗のときの扱い

- client の画像の import に失敗: その buffer を protocol error にし（今と同じ）、他の窓は影響を受けない。
- VkDevice の喪失（GPU の reset）: 出力を作り直す。作り直せなければ zdesktop は終了し、表示は console に戻る。
- acquire fence が signal されない client: その commit を出さないだけで、他の窓の描画は止めない（窓ごとに独立）。

## 8. 自己レビュー（敵対的な見直し）

設計の Agent を使わない運用なので、自分で反対側から見直した。

| 疑い | 検討 | 結論 |
| --- | --- | --- |
| OPAQUE_FD の import は同じ GPU でしか使えない | zdesktop と client は同じ描画 node を使う。別の GPU の client は今も未対応（WS014 p007 の範囲） | 問題なし。別 GPU は範囲外と明記 |
| optimal tiling の client の画像を sampling できるか | p007 で optimal の共有を受入済み（strict の renderer の組） | 可。linear でない画像は全画面モードでも直接 scanout せず、1 枚の quad で描く（D0） |
| モードの切替で画面がちらつく | 切替は frame 単位で表示の持ち主は変わらない。全画面の窓を 1 枚描いた画像と client の画像は同じ内容 | 問題なし |
| 全画面モードで cursor が要る app（全画面の文書閲覧など） | 全画面モードは cursor を描かない。その app はウィンドウモードの最大化で使う | 制約として明記（D0）。cursor plane ができたら解消 |
| すりガラスで窓ごとに render pass を区切ると遅い | 効果のある窓だけ区切る。区切らない窓は 1 pass にまとめる | p057 で測って判断 |
| cursor を合成すると、pointer を動かすだけで全画面を描き直す | 1 回目は全画面。測って重ければ p055 を前倒し | 測定で判断 |
| 表示の lease を libvulkan と zdesktop の両方が持とうとする | ウィンドウモードは libvulkan の surface だけが持ち、全画面モードに入る前に surface を破棄して返す（D0） | 衝突しない。切替の隙間の console は p052 で確認 |
| swapchain の画像の scanout に余分な copy が入る | libvulkan は swapchain の作成時に共有の linear 画像を直接 scanout する経路を選ぶ（WS014 p007）。入る場合もウィンドウモードの 1 回だけ | 許容。p052 で trace を見る |
| `wl_shm` の copy が GPU の画像の窓を遅くする | copy は CPU で event loop の中、upload は damage の範囲だけ。GPU の画像の窓の import・sampling には触れない | p053 で frame 時間を測る |
| event loop が fence の wait で止まる | fence は fd にして poll に入れる。CPU の wait はしない（D3） | 止まらない |
| 3 枚の出力画像のメモリ | 1080p で 3 × 8 MB = 24 MB | 許容 |

## 9. 後回しにするもの

- 複数の画面、画面ごとの scale。
- GPU の無い機種（pc98、firmware の framebuffer だけの PC）での合成。CPU で合成する別の backend が要る。
  今は zdesktop を GPU のある機種に限る。
- 色空間・HDR、回転、画面の分数 scale。
- hardware の cursor plane（driver が plane を持つようになったら D0 の全画面モードの cursor と D8 を変える）。
- 全画面モードの上に通知などを重ねること（通知の仕組みができたとき）。

## 10. 承認をもらいたい点

1. **D0**: 全画面モード（最前面の全画面の窓をそのまま scanout）とウィンドウモード（Vulkan の合成）の 2 つ。
   全画面モードでは cursor を描かない点、scanout できない画像の全画面の窓はウィンドウモードのまま 1 枚に描く点。
   切替の隙間に console が一瞬出るなら、同じ process の claim が lease を引き継ぐ interface を GPU の UAPI に足す点（p052 で設計）。
2. **D1**: ウィンドウモードは `VK_KHR_display` の swapchain で出し、全画面モードでは swapchain と surface を破棄する（2 回目のレビューを反映）。
3. **D2**: GPU の画像を主要な経路として copy 0・import 1 回などの最適化を入れ、`wl_shm` は damage の範囲の CPU の copy で補助として持つ（2 回目のレビューを反映）。
4. **D3**: `zed_gpu_buffer_v1` に acquire fence を渡す request を足す（最小限の explicit sync）。
5. **D7**: 装飾は server-side を既定にする。
6. **D8・D10**: 既定の cursor は zdesktop が持ち、`set_cursor` は shm・GPU の surface を受ける。効果（すりガラス・影）は構造だけ p052 で用意し、p057 で作る。
7. **§5**: p052 → p053（`wl_shm`・cursor）→ p054 → p011 → p055 → p057 の順序。
8. 窓の初期位置（cascade）、最大化でタスクバーの分を除くことなど、細かい既定（D6）。
