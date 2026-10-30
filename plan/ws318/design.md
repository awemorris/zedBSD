<!-- awesome-plan project=zedbsd record=ws318-design -->

# WS318 設計: libmedia の作り直し（Vulkan の device の共有・codec の問い合わせと指定・encoder・camera・libavcodec の内部の fallback）

- 親: [ws.md](ws.md)。作成: ws318-p001（P5、q1752-i01、2026-10-31）。調べた tree: main 843847185（worktree `p5-ws318`）。
- 正本: ユーザーの決定は ws.md の原文 1〜6 と設計の合意。この文書はその解釈と実装の設計。公開の interface の案は [proposed/media.h](proposed/media.h)・[proposed/media-vulkan.h](proposed/media-vulkan.h)（各関数の comment が仕様。p002 で source へ写す）。
- ユーザーの判断を待つ点は [user-questions.md](user-questions.md)（Q318-1〜9）。敵対的レビューと反映は [design-review.md](design-review.md)、要点は §16。
- HAL（`include/hal/hal.h`）は変えない（§10）。

## 1. 目的・範囲・範囲の外

目的（ws.md）: libmedia を Keiland の app の唯一の media の入り口にする。hardware は Vulkan Video の 1 本、無ければ libmedia の独自の software、無ければ libmedia の中で libavcodec を dlopen。decoder・encoder（H.264・H.265・AAC-LC）・camera を、app と共有する Vulkan の device の上で、fd を API に見せずに扱う。基準の機械は i915（5330）。

範囲:

1. 公開の API の作り直し: context（device の無し・libmedia の private・app と共有）、codec の問い合わせと指定（decode と encode）、decoder、frame（参照の数の picture、CPU か VkImage）、encoder、camera、engine の callback。
2. backend の表: decode は Vulkan Video → 独自の software（H.264 は WS326、AAC-LC）→ libavcodec。encode は Vulkan Video → 独自の software（AAC-LC。H.264 は WS329、H.265 は WS330 が後で足す）→ libavcodec。
3. libavcodec の decode の add-in を `userland/desktop/media-app/` から libmedia の中へ移し、encode の add-in を足す。
4. 形式を前提にしない: decoder・encoder・camera の形式を実行の時に問い、合わなければ GPU で変換（compute と transfer）。
5. Vulkan Video の encode の backend（H.264・H.265）と、H.265 の Vulkan Video の decode の backend（H.265 の bitstream の解析を含む）。driver が無いので試験は偽の Vulkan Video（§13.2）。実機は WS319 以降（Q318-1）。
6. AAC-LC の encoder を encoder の口に載せ、足りない所（窓の切り替えと短い窓・TNS、Q318-5）を独自に。
7. camera の口（`/dev/camN` の zedBSD の source、`CAM_MEMORY_IMPORT` を i915 の coherent な memory に広げる kernel の変更を含む）。
8. app の書き換え: Video Player・Music・libbrowser の engine・media-probe を libmedia だけに。`media-app/` を無くす。
9. libmedia の thread を名前の付いた thread（WS309 の方針）に、起こしを callback に。

範囲の外（理由）:

- 個別の GPU・VPU の Vulkan Video の driver（WS319〜WS325、この WS の completed の後。ユーザーの決定 6）。libvulkan の encode・H.265 の命令と構造体の wire の転送も driver の側（§10.3）。
- H.264・H.265 の software の encoder（WS329・WS330）、H.265 の software の decoder（WS330）。この WS は口（encoder の backend の ops）を用意する。
- file への書き出し（MP4 の muxer）。mediafile は reader だけ（Q318-7）。
- 10 bit（P010、HEVC Main 10）・interlace・4:2:2 以上の出力（Q318-8）。
- Browser の `<video>` の GPU の道（今は engine の CPU の画素）。libbrowser の描画は WS074・WS328 の領分。
- Photos は libmedia を使っていない（動画を扱わない）ので変えない。Linux・FreeBSD の Keiland の build に libmedia は入っていない（`keiland-linux.mk` に無い）ので、zedBSD の build だけを対象にする。

## 2. 今の姿（main 843847185 で読んだ事実）

| 物 | 事実 | 場所 |
| --- | --- | --- |
| 公開の header | `media.h`（engine）、`media-decoder.h`（decoder・frame・`struct media_gpu_picture`）、`media-sbc.h`、`media-aac-encoder.h`、`mediafile.h`。`exports.map` が export を限る | `userland/desktop/libmedia/` |
| decode の表 | `decoder_backends[] = { media_vkvideo_ops, media_aac_ops }`。h264sw の backend（`media_h264sw_ops`）は骨格で表に未登録（WS326 p023 で `{vkvideo, h264sw, aac}`、遅れた拒否の切り替えも p023） | `decoder.c`、`h264sw-backend.[ch]` |
| libavcodec | app の側の add-in。`media-app/decoder.c` が native を先に開き、DEVICE・PROFILE・FORMAT・BUSY と最初の packet の ENOTSUP・ENODEV・EBUSY で `app_avcodec_ops` へ。`avcodec.c` は dlopen（`libavcodec.so.61/63` 等）、`kl_once`（libkeiland）を使う。`bitstream.c` は libmedia の物と接頭辞の他は同じ | `userland/desktop/media-app/` |
| 使う app | Video Player・Music（`media-app/*.c` を自分の source に入れて build）、libbrowser の `page/media.c`（engine と `media_engine_wake_fd` を loader の watch に）、`media-probe`（`media-private.h`・`picture.h` を include して CPU の picture を hash） | 各 Makefile |
| Vulkan | `vkvideo-runtime.c` が decoder ごとに `dlopen("libvulkan.so.1")`、instance と device を自分で作る。GPU の picture は decode の image の memory を `OPAQUE_FD` で export し app が import（`struct media_gpu_picture.fd`）。picture は decode の fence の後に渡す | `vkvideo-runtime.c`、`videoplayer/present.c` |
| GPU の picture の条件 | decode の出力の image に SAMPLED の usage と plane の view（MUTABLE・EXTENDED_USAGE）が出せる device だけ（Pi 4・RTX）。出せない device は readback（transfer → buffer → CPU） | `vkvideo-runtime.c` 2586 行付近 |
| i915 の decode | 出力の形式は NV12 の 1 つ、OPTIMAL（Tile Y）、usage は `DECODE_DST | DECODE_DPB | TRANSFER_SRC`。**SAMPLED が無い**。queue family は graphics 1 本・video 1 本（各 queueCount 1） | `src/drivers/gpu/i915/vk/vk-video.c` 879 行、`vk-instance.c` 588・599 行 |
| libvulkan | device の拡張に sync2・video_queue・video_decode_queue・decode_h264・external_memory(_fd) はある。timeline semaphore・sampler YCbCr conversion は無い | `userland/desktop/libvulkan/instance.c` |
| Vulkan の header | Khronos 1.3.269 から選んだ物。`vk_video/` は H.264 の std と decode だけ。encode は 1.3.269 では EXT（provisional）の構造体の番号だけで、KHR の encode・H.264/H.265 の encode の std・H.265 の std は無い | `include/libc/vulkan/`、`API-PROVENANCE.md` |
| libavcodec の package | LGPL の build、`--disable-encoders`（encoder は全部無い）。FFmpeg の native の LGPL の encoder に H.264・H.265 は無い（libx264・libx265 は GPL の外部の library） | `userland/packages/multimedia/libavcodec/Makefile` |
| camera | `include/uapi/camera.h`（CAM_FORMAT_YUYV・NV12・MJPEG ほか、`CAM_MEMORY_DEVICE`・`CAM_MEMORY_IMPORT`、`struct cam_buffer.fd`）。`cam.c` の import は backing が CONTIGUOUS・RAM・UNCACHED の時だけ（それ以外 `EXDEV`）、`get_allocation_backing` を持つ GPU は bcm2711 だけ。i915 の gem の object は 1 つの連続の run（`backing->run.paddr`） | `src/drivers/camera/cam.c` 1040〜1130 行、`src/drivers/gpu/i915/resource.c` |
| thread | engine は `pthread_create` の 1 本（名前無し）。libc に `pthread_setname_np` は無い（WS309 design §2） | `engine.c` |
| shader | Video Player は GLSL を `regenerate.py` で SPIR-V の配列の header にする（host の glslangValidator） | `videoplayer/shaders/` |
| host の試験 | `plan/tools/media/run-host-codec.sh` が libmedia の decoder と media-app を host で build（c89、ASan・UBSan、host の libavcodec 61 を dlopen） | `plan/tools/media/` |
| host の Vulkan | lavapipe（Mesa 25.0.7、Vulkan 1.4、compute・transfer あり、Vulkan Video は無い）、glslangValidator・spirv-val がある。validation layer は入っていない | host |

## 3. 決定（判断の根拠と、選ばなかった案）

| # | 決定 | 根拠 | 選ばなかった案と理由 |
| --- | --- | --- | --- |
| D1 | 公開の header は `media.h`（Vulkan の型を持たない全部: context・問い合わせ・decoder・frame・encoder・camera・engine）と `media-vulkan.h`（Vulkan の型の部分）の 2 つ。`mediafile.h`・`media-sbc.h`・`media-aac-encoder.h`（A2DP の低い層）はそのまま。`media-decoder.h` は最後に `media.h` へ統合 | master-design-policy §2.6（少数の、まとまった header）。Vulkan を使わない app（Music・engine の使い手）と host の試験が Vulkan の header 無しで compile できる。Windows などで Vulkan の型の部分だけ別に実装できる（ユーザーの決定 2） | 1 つにまとめる（全 app が vulkan.h を要る）、機能ごとに 6 つに分ける（§2.6 の「細切れ」） |
| D2 | context を新しく持つ。device の mode は NONE・PRIVATE・SHARED。旧い global（`decoder_gpu_hold`）は context と decoder の config へ | 共有の device は 1 度渡して decoder・encoder・camera が使う。PRIVATE は今の動き（libmedia の device と readback）を browser の engine などに残す | 関数ごとに device を渡す（寿命と queue の lock が散る）、global の 1 つの device（複数の app の device を持てない） |
| D3 | libmedia は app の `vkGetInstanceProcAddr` だけで Vulkan に届く。SHARED では libvulkan を dlopen しない | 同じ dispatch を使う。試験で proc addr を差し替えて偽の Vulkan Video を挟める（§13.2） | libmedia が自分で dlopen（app と違う loader の状態になりうる） |
| D4 | 要る物の問い合わせは 2 段: `media_vulkan_instance_requirements` と `media_vulkan_device_requirements`（拡張・sync2・queue の要求・hardware の codec）。app が自分の要求と合わせて作る | Vulkan Video は device の作成の時に queue family と拡張が要る。物理 device の選びに hardware の codec の有無を使える | libmedia が device を作り app に貸す（ユーザーの合意は app の device の共有） |
| D5 | queue: decode・encode の queue は libmedia だけが使う。compute の queue は余りがあれば libmedia 専用、無ければ app の queue を lock の callback 付きで共有 | i915 は graphics family の queue が 1 本（§2）。VkQueue への submit は外部の同期が要る | libmedia の変換を app の command buffer に記録させる（encode・camera の道に app の command buffer が無い） |
| D6 | frame（`struct media_frame`）を decoder・camera・encoder の共通の型にし、参照の数で生かす。CPU と VULKAN と wrap（app の image）の 3 種 | ユーザーの合意（camera の frame を表示と encode が同じ画像で使い、参照の数で返す）。名前は今の公開の `media_frame` を引き継ぎ、移行の差を小さくする | 新しい型 `media_picture`（内部の `struct media_picture`（picture.h）と衝突、WS326 の h264sw-backend が使う） |
| D7 | frame の同期: libmedia は書き込みの完了（fence）を自分の thread で待ってから frame を渡す。app は自分の GPU の読みが終わってから参照を落とす。image は CONCURRENT で作り queue family の移しをしない | libvulkan に timeline semaphore が無い（§2）。今の GPU の picture の契約と同じで、Video Player の作りを変えずに済む | 渡す時に binary semaphore（app が 1 回しか待てない、複数の reader で壊れる）、timeline（無い） |
| D8 | 形式の交渉: app が受ける形式を好みの順に並べ、libmedia は変換無しで出せる最初の物、無ければ GPU で変換できる最初の物を選ぶ。変換は compute の 1 pass ＋ 必要なら transfer の copy（§6） | ユーザーの決定 3（NV12 の前提を捨て i915 を基準に）。i915 の decode の出力は sample できないので copy が要る | 形式を NV12 に固定（決定 3 に反する）、YCbCr の sampler（libvulkan に無い） |
| D9 | decoder・encoder の呼び出しは呼び手の thread で同期（GPU を有限の時間だけ待つ）。libmedia の常駐の thread は engine と camera（と後の software の並列）だけ。全部名前を付け、本数を決め、signal を block して作る。起こしは notify の callback（edge で 1 回、lock を持たずに） | WS309 の原則（待つ物は名前の付いた thread、本数を決める、thread の間は queue と通知）。decoder を使う app（Video Player）は既に自分の media の thread で decode している | decoder を非同期にして libmedia の thread で decode（app の作りを全部変える、今の Video Player の音と絵の同期が崩れる）、fd の wake を残す（ユーザーの合意で外す） |
| D10 | backend の ops を 2 層に: 各 backend は自分の出力（CPU の plane か自分の VkImage）を作り、変換・upload・readback は decoder の層（`decoder.c`）と encoder の層（`encoder.c`）が `convert.c` で行う | 変換を backend ごとに持たない（独立の実装は上の契約だけを満たす。§2.6 の AHA）。software の backend（WS326・WS329・WS330）は CPU の plane だけを出せばよい | backend ごとに VkImage へ upload（同じ code が 4〜5 か所） |
| D11 | 遅れた拒否の切り替えは WS326 p023 の規則のまま、表の最後に libavcodec を足す。全部が断った時の問題の値の選び方を決める（MISSING を優先、§4.9） | app が「libavcodec を入れれば見られる」と言える | 最初の backend の値（今の decoder.c）だけ（MISSING が出ない） |
| D12 | encoder は開いた後に backend を変えない。open の時に config の全部（size・profile・level・rate control）を受けられる最初の backend | 1 本の stream の packet が別の encoder の物に変わると壊れる | decoder と同じ遅れた切り替え |
| D13 | libavcodec の add-in は libmedia の中で `pthread_once` を使う（`kl_once` を使わない）。libmedia は libc（pthread・dlopen・libm）だけに依る | libbrowser が libmedia を link し、libkeiland に依らない | libmedia を libkeiland に依らせる |
| D14 | camera の OS の部分は `camera-zedbsd.c` の 1 file（`<uapi/camera.h>` と ioctl はここだけ）、camera の層は source の ops を通す | OS の境界を 1 file に閉じる（guardrail の配置の考え）。後で Linux（V4L2）などを足せる | camera の層に ioctl を直に |
| D15 | camera の frame の再投入（`CAM_QUEUE_BUFFER`）は最後の `media_frame_unref` の中で行う（待たない ioctl 1 つ、camera の lock の中） | camera の thread は DEQUEUE で待っているので、そこへ頼むと全 buffer が持たれた時に timeout まで遅れる | camera の thread に依頼（遅れる） |
| D16 | H.265 の Vulkan Video の解析（StdVideoH265 の構造体へ）は libmedia の `h265*.c` に、software の decoder（WS330）の解析とは分ける | WS326 の design の決定（h264.c と h264sw-ps を分けた理由: Vulkan の header に依る構造体か、自分の単位か）と同じ。WS330 は後で共通化を設計できる | WS330 を待つ（WS318 が先に完了する予定に反する） |
| D17 | 標準廉価実装手順は仕様で振る舞いが決まる所（H.265 の解析・POC・RPS・DPB、AAC の窓の切り替えと TNS）だけに使う。device の共有・変換・camera・移行は普通の Phase（Opus） | spec-module-delegation §1 の適用の条件 | 全部を Sonnet（設計を探りながら作る所は §1 の外） |

## 4. API（要点。全文は proposed/ の header）

### 4.1 object と寿命

| object | 作る | 消す | 持つ物 | 寿命の条件 |
| --- | --- | --- | --- | --- |
| `media_context` | `media_context_create` | `media_context_destroy`（生きた子があれば EBUSY） | device の束縛（関数の表・queue・lock）、変換の pipeline、frame の pool、libavcodec の読み込みの状態（process で 1 回、context をまたいで共有）、log | SHARED の app の device より短い |
| `media_decoder` | `media_decoder_create` | `media_decoder_close` | backend の状態、track の写し、切り替えの状態、出力の pool | context より短い |
| `media_encoder` | `media_encoder_create` | `media_encoder_close` | backend の状態、入力の変換の image、出力の packet の buffer | 同上 |
| `media_camera` | `media_camera_open` | `media_camera_close` | device の fd（内部）、slot、thread | 同上 |
| `media_engine` | `media_engine_create` | `media_engine_close` | thread、file、decoder | context（NULL なら自分の NONE の context） |
| `media_frame` | decoder・camera・`media_frame_create`・`media_frame_wrap_vulkan` | 最後の `media_frame_unref` | 画素（pool の image・buffer・camera の slot・app の image）と作り手への参照 | 作り手（decoder・camera）の close の後も生きる。context より短い |

### 4.2 共有の device の手順（SHARED）

1. app: `media_vulkan_instance_requirements(vkGetInstanceProcAddr, &ir)` → 自分の拡張と合わせて `vkCreateInstance`（api_version は大きい方）。
2. app: 物理 device ごとに `media_vulkan_device_requirements(get, instance, physical, MEDIA_QUEUE_DECODE|ENCODE, &dr)`。表示できる device の中で `dr.decode_codecs` の多い物を選んでよい。
3. app: `dr.extensions` を自分の拡張に足し、`dr.synchronization2` なら `VkPhysicalDeviceSynchronization2Features` を鎖に、`dr.queues` の family と数を自分の queue の作成に足す（同じ family は数を合わせる。足りなければ自分の queue を共有にする）。`vkCreateDevice`。
4. app: `struct media_vulkan_device` を埋め（実際に enable した拡張、queue と役、共有の queue には lock、自分が frame を読む family）、`media_context_create({MEDIA_DEVICE_SHARED, &device, notify, data})`。
5. 以後 decoder・encoder・camera を context の上で開く。終わりは子を閉じ、frame を全部落とし、`media_context_destroy` の後に `vkDestroyDevice`。

libmedia が要る拡張（device が持つ物だけを返す）: `VK_KHR_synchronization2`、`VK_KHR_external_memory`・`VK_KHR_external_memory_fd`（camera の import、encode の結果の共有は無し）、`VK_KHR_video_queue`、`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264`・`VK_KHR_video_decode_h265`、`VK_KHR_video_encode_queue`・`VK_KHR_video_encode_h264`・`VK_KHR_video_encode_h265`（header の更新の後、§10.2）。multi-planar の形式（NV12）と plane の aspect は、今の runtime が libvulkan の上で既に使っている（readback の image → buffer）。image → image の plane の copy は未確認（§6.3）。
instance: `VK_KHR_get_physical_device_properties2`、`VK_KHR_external_memory_capabilities`。

### 4.3 codec の問い合わせと指定

- `media_codec_query(ctx, MEDIA_DECODE|MEDIA_ENCODE, codec, infos, capacity, &count)`: 試す順の実装の一覧。各行に id・kind・名前・状態（0 か MISSING・VERSION・DEVICE）・profile・level・最大の大きさ・rate control・B-frame・参照の数・変換無しの形式・音の rate と channel。
- 指定: decoder・encoder の config の `allow`（kind の bit）か `implementation`（id 1 つ）。`implementation` を指定して断られた時は次へ回さず断った値を返す（app が明示に選んだ物）。
- hardware の行は Vulkan の問い合わせ（`vkGetPhysicalDeviceVideoCapabilitiesKHR`・`vkGetPhysicalDeviceVideoFormatPropertiesKHR`、encode は `VkVideoEncodeCapabilitiesKHR` と codec の caps）を context の作成の後の最初の問い合わせで 1 回だけ取り、context に保つ。libavcodec の行は dlopen と `avcodec_find_{de,en}coder_by_name` で埋める（profile などは分かる範囲だけ、分からなければ 0）。

### 4.4 decoder

`media_decoder_create(ctx, track, config, &dec)` の他は今の関数のまま（send・receive・picture・sound・trim・frame_us・flush・close・name・backend）に `media_decoder_kind` を足す。`media_decoder_picture` は参照 1 つの frame を返す（呼び手が `media_frame_unref`）。frame の memory と形式は config に従い decoder の層が変換する（§6）。CPU の context の frame は CPU。

### 4.5 frame

- 共通: ref・unref・size・aspect・memory・format・colour（行列・range・primaries・transfer・chroma の位置）・time。
- CPU: `media_frame_planes`、`media_frame_scale`（CPU で描く使い手、browser の engine）。
- VULKAN: `media_frame_vulkan` が `struct media_vulkan_frame`（VkImage・identity・形式・作成の flag・usage・tiling・plane の view の形式・extent・見える窓・layout）。app は識別 `identity` で view を cache する（今の (identity, slot) の import の cache の代わり、fd の import は無くなる）。
- 作る: `media_frame_create`（app が CPU で書く・自分の pass で描く encoder の入力）、`media_frame_wrap_vulkan`（app の image を encoder に、書いた semaphore と解放の callback）、`media_frame_written`。

### 4.6 encoder

- `media_encoder_create(ctx, config, &enc)`。config: codec・allow/implementation・profile・level・大きさ・frame rate・rate control（DEFAULT・CQP・CBR・VBR と bitrate・max_bitrate・qp）・key frame の間隔・B-frame（0 だけ、Q318-9）・colour・音の rate と channel と bitrate・出力の枠（Annex B か RAW、AAC は ADTS か raw）。
- `media_encoder_input(enc, &memory, &format)`: 変換無しで読める入力（camera が選ぶ時に使う）。
- `media_encoder_send_frame(enc, frame, time, flags)`・`media_encoder_send_sound(enc, samples, frames, time)`・`media_encoder_receive(enc, &packet)`（packet の bytes は次の receive まで encoder の物）・`media_encoder_codec_data(enc, form, ...)`（parameter set・avcC・hvcC・AudioSpecificConfig）・`media_encoder_close`。
- AAC は既存の `media_aac_encoder_*`（A2DP の低い層、export は残す）を encoder の backend（`aac-encoder-backend.c`）に包む。

### 4.7 camera

- 列挙: `media_camera_count`・`media_camera_describe`・`media_camera_modes`（CAM_ENUM_FORMAT・ENUM_MODE・ENUM_INTERVAL から）。
- `media_camera_open(ctx, index, config, &cam)`: config の大きさと rate に近い mode を、(1) encoder の入力の形式と同じ、(2) app の受ける形式と同じ、(3) libmedia が変換できる、(4) MJPEG の順で選ぶ。memory: SHARED の context で import が使えれば `CAM_MEMORY_IMPORT`（libmedia が作った memory を export して kernel に渡す）、使えなければ（`EXDEV`・`ENOTSUP`）`CAM_MEMORY_DEVICE`（mmap）と GPU への upload。
- `media_camera_start`・`stop`・`take`（EAGAIN・ENODEV・EIO、落とした数）・`media_camera_mode`（選んだ mode と encoder へ直に行くか）・`close`。
- 通知は config の notify（MEDIA_EVENT_CAMERA）。

### 4.8 engine と callback

`media_engine_create(ctx, path, source, flags, notify, data, &engine)`。engine の thread が状態の変化・picture の到着で notify を 1 回呼び、app が `media_engine_status` を読むと再び呼べる状態に戻る。`media_engine_wake_fd` は無くなる。libbrowser は notify の中で自分の pipe に 1 byte 書き、今の `net_loader_watch` をその pipe に向ける（libbrowser の変更は `page/media.c` の中だけ）。

### 4.9 誤りの値

- errno: EINVAL（引数・順序）、ENOMEM、EAGAIN（先に receive）、ENOTSUP（その形・memory を扱えない）、EBUSY（context の destroy、camera の取り合い）、ENOENT・EACCES・ENODEV・EIO（camera）、ENOBUFS（codec data の大きさ）。
- `MEDIA_PROBLEM_*`: 今の 1001〜1006 に CONFIG（1007、encoder の設定）と LOST（1008、device の喪失）を足す。全部の実装が断った時の値は §3 D11: libavcodec を試すべきだったが無い → MISSING、次に「codec を知る最初の実装の断り」、どの実装も codec を知らない時だけ FORMAT。

## 5. 内部の構成

### 5.1 file（`userland/desktop/libmedia/`、新しい物に ★）

| file | 中身 | 由来 |
| --- | --- | --- |
| ★`context.c` | context・device の束縛・queue の lock・子の数・LOST | 新 |
| ★`vulkan-device.c` | requirements の問い合わせ、proc addr からの関数の表、PRIVATE の device の作成（`vkvideo-runtime.c` の instance・device の作成を移す） | 一部移す |
| ★`frame.c` | `media_frame` の種類（CPU・VULKAN・WRAP・CAMERA）、参照の数、pool | 新（`picture.c` の pool は CPU の frame の下で使い続ける） |
| ★`convert.c`・★`convert-shaders.h`・★`shaders/*.comp`・★`shaders/regenerate.py` | 形式の交渉、変換の graph、compute の pipeline、upload・readback・plane の copy | 新（作りは videoplayer の shaders に倣う） |
| ★`codec-query.c` | `media_codec_query`、hardware の caps の cache | 新 |
| `decoder.c` | decoder の層（WS326 p023 の切り替え）＋ context・config・出力の変換・libavcodec の行 | 変える |
| ★`encoder.c` | encoder の層: 選び・入力の変換・packet の枠（Annex B・長さ・ADTS） | 新 |
| ★`avcodec.c`・★`avcodec-layout.h` | libavcodec の add-in（decode を移し encode を足す） | `media-app/` から移す |
| ★`aac-encoder-backend.c` | 既存の AAC-LC の encoder を encoder の ops に | 新 |
| `aac-encode.c`（と新しい補助） | 窓の切り替え・短い窓・TNS（Q318-5） | 変える |
| `vkvideo.c`・`vkvideo-runtime.c` | context の device を使う、H.265 の道、export を内部へ | 変える |
| ★`vkvideo-encode.c`・★`vkvideo-encode-h264.c`・★`vkvideo-encode-h265.c` | Vulkan Video の encode の backend | 新 |
| ★`h265.c`・★`h265.h`・★`h265-dpb.c`・★`h265-dpb.h`（設計の Phase で module を確定） | H.265 の NAL・VPS・SPS・PPS・slice segment header を StdVideoH265 へ、POC・RPS・DPB | 新（標準廉価実装手順） |
| ★`camera.c`・★`camera-zedbsd.c`・★`camera-source.h` | camera の層と zedBSD の source | 新 |
| ★`thread.c` | 名前の付いた thread の台帳 | 新 |
| `engine.c` | context と notify | 変える |
| `media-private.h` | ops の表（decoder v2・encoder・camera source・frame の種類） | 変える |
| `media-decoder.h` | 移行の間は旧い宣言、最後に `media.h` へ統合して消す | 消す（Q1） |

### 5.2 decoder の backend の ops（v2、`media-private.h`）

今の `struct media_decoder_ops` を次に変える（p006、WS326 の完了の後）:

- `kind`、`query(ctx, codec, info)`（問い合わせの行を埋める）、`open(ctx, track, out_config, state)`（`out_config` は decoder の層が決めた backend の出力の希望: hardware なら VkImage で SAMPLED が要るか、CPU なら plane の形式）。
- `picture(state)` は backend の frame（`struct media_frame *`、frame.c の種類のどれか）を返す。picture_free・picture_size・picture_scale・picture_aspect・picture_gpu の 5 つは frame の ops へ移る。
- `send`・`receive`・`sound`・`flush`・`close`・`trim`・`frame_us`・`decoder_name` は今のまま。
- software の backend（h264sw・aac・avcodec）は CPU の frame（plane）を出す。decoder の層が config の memory が VULKAN なら upload（§6）。

### 5.3 encoder の backend の ops（新）

`kind`・`query`・`open(ctx, config, state)`（config を全部受けられなければ MEDIA_PROBLEM_CONFIG）・`input(state, &memory, &format)`・`send_frame(state, frame, time, flags)`（入力は `input` の形に変換済み）・`send_sound`・`receive(state, packet)`・`codec_data(state, form, ...)`・`close`。
表（`encoder.c`）: `{ vkvideo_encode_h264, vkvideo_encode_h265, aac_lc, avcodec }`。WS329・WS330 は software の行をこの表の hardware の後・avcodec の前に足す。

### 5.4 backend の順の表

| codec | decode | encode |
| --- | --- | --- |
| H.264 | vulkan-video → h264-software（WS326）→ libavcodec:h264 | vulkan-video → （h264 software、WS329）→ libavcodec（libx264・libopenh264 などがあれば） |
| H.265 | vulkan-video（この WS で解析を作る）→ （h265 software、WS330）→ libavcodec:hevc | vulkan-video → （h265 software、WS330）→ libavcodec（libx265 などがあれば） |
| AAC-LC | aac-lc（独自）→ libavcodec:aac | aac-lc（独自）→ libavcodec:aac |
| AV1・VP9・VP8・MPEG-4・Opus・MP3・Vorbis・Theora・MJPEG・PCM | libavcodec（今の `codec_names[]` のとおり） | — |

zedBSD の libavcodec の package は encoder が無いので、zedBSD では encode の libavcodec の行は MISSING ではなく「その codec の encoder が無い」（行の state は MEDIA_PROBLEM_FORMAT）になる（Q318-2）。

### 5.5 遅れた拒否の切り替え（decode）

WS326 p023 の規則（`h264sw-backend.h` の comment: open の DEVICE・PROFILE・BUSY は次へ、send の ENOTSUP・ENODEV・EBUSY は受け入れ 0 なら次を開き同じ packet から、受け入れ後なら旧 backend を drain して閉じ、key frame まで捨てて次へ）をそのまま使い、libavcodec を表の最後に置く。`implementation` を指定した decoder は切り替えない（その値を返す）。切り替えで出力の形式が変わっても decoder の層の変換が app の求める形式に揃える。

## 6. 形式と変換

### 6.1 交渉

1. decoder の層は backend の open の前に、backend が出せる出力（hardware: `vkGetPhysicalDeviceVideoFormatPropertiesKHR` の DECODE_DST の形式・tiling・usage、software: CPU の plane の形式）を得る。
2. app の `formats` を順に見て、(a) 変換無し（同じ形式で app が読める: VULKAN なら SAMPLED の usage があり multi-planar なら MUTABLE・EXTENDED_USAGE で plane の view が作れる）、(b) copy だけ（同じ形式、出力に TRANSFER_SRC）、(c) compute の変換で作れる、の順に最初に満たす物を選ぶ。`format_count` 0 は backend の形式のまま（VULKAN で sample できなければ (b) で sample できる image に写す）。
3. 選んだ道を decoder に保ち、backend が変わった時（§5.5）にやり直す。

### 6.2 変換の graph

| 元 | 先 | 道（GPU だけ、CPU は画素を触らない） |
| --- | --- | --- |
| hardware の decode の image（SAMPLED あり: Pi 4・RTX） | 同じ形式の VULKAN | 無し（decode の image をそのまま、今の GPU の picture の道） |
| hardware の decode の image（SAMPLED 無し: **i915**） | NV12 の VULKAN | plane ごとの `vkCmdCopyImage`（PLANE_0・PLANE_1 の aspect）で sample できる NV12 の image へ（transfer 1 回） |
| 同上 | BGRA8・RGBA8 | 上の copy の後に compute の YUV→RGB（2 pass。app が NV12 を受ければ 1 pass で済むので、Video Player は NV12 を先に並べる） |
| CPU の frame（software・libavcodec の decode、camera の mmap） | VULKAN（同じ形式） | host-visible の staging buffer へ書き（software の decoder が直に書けば複写無し、§6.4）、`vkCmdCopyBufferToImage`（plane ごと） |
| CPU の frame | 別の形式の VULKAN | staging buffer を storage buffer として compute で読み、出力の plane を storage buffer に書き、`vkCmdCopyBufferToImage` |
| camera の import の memory（YUYV・UYVY・NV12） | encoder の入力（NV12 の ENCODE_SRC） | 同じ形式で ENCODE_SRC が LINEAR を許し pitch が合えば、同じ memory に image を bind（pass 無し、ゼロコピー）。それ以外は compute（YUYV→NV12）＋ copy |
| VULKAN の任意 | CPU（PRIVATE の context、CPU の encoder） | compute か copy で host-visible の buffer へ、fence を待って map（readback） |
| app の RGBA の image | encoder の NV12 | compute の RGB→YUV（行列は config の colour） |

- compute の shader（GLSL → SPIR-V の配列、videoplayer と同じ `regenerate.py` の作り）: `yuv420-to-rgb`（NV12・I420、行列・range は push constant）、`packed422-to-nv12`、`packed422-to-rgb`、`rgb-to-nv12`、`i420-to-nv12`。読みは sampled image（SAMPLED のある元）か storage buffer、書きは storage buffer → `vkCmdCopyBufferToImage`（R8・R8G8 の storage image は `shaderStorageImageExtendedFormats` が要るので使わない）。
- 色: BT.601・709・2020 の行列、limited・full の range（ITU-T H.273 と各 BT の式。値は block ごとに「Values referenced:」で出典を書く）。

### 6.3 i915 を基準にした時の条件

- decode の出力に SAMPLED が無い（§2）。plane の copy（image → image、multi-planar の aspect）は Vulkan 1.1（か `VK_KHR_sampler_ycbcr_conversion`）の規則で、i915 の executor が実装しているかは未確認（今の readback は image → buffer）。p006 で 5330 で確かめ、出来なければ image → buffer → image（transfer 2 回）を代わりに使う（i915 の driver の直しは WS319 の範囲）。
- i915 は UMA（device-local と host-visible が同じ memory）なので、staging の buffer は device-local・host-visible の memory を選び、upload は copy 1 回で済む。
- i915 が decode の出力に SAMPLED を出すようになれば（WS319 の判断）、§6.2 の 1 行目の道になり copy も無くなる。

### 6.4 software の出力の複写

今の h264sw-backend は decoder の内部の frame を `media_picture` の pool へ複写する（WS326 design §2 の「出力」）。VULKAN の時はさらに staging への複写がある。p006 で decoder の層が staging の buffer を CPU の frame の pool として渡せる形（backend が `media_frame` の CPU の pool から取って書く）にし、複写を 1 回に減らす。backend の内部の frame から直に staging へ書く最適化は WS326 p024（速さ）か後の Phase（Q1 への注意 §15）。

## 7. thread・lock・同期

### 7.1 thread（全部 `thread.c` の台帳で名前・本数を持ち、signal を全部 block して作る。起動と終了を media_log に 1 行）

| 名前 | 種類 | 本数 | 仕事 | 待つ所 | 結果の渡し方 |
| --- | --- | --- | --- | --- | --- |
| media-engine | 常駐（engine の寿命） | engine ごとに 1 | file の read、decode、picture の ring | read、decode の中の GPU の fence、ring の空き | lock の下の状態 ＋ notify（edge） |
| media-camera | 常駐（start〜stop） | camera ごとに 1 | `CAM_DEQUEUE_BUFFER`（timeout 100 ms）、frame の作成、変換（GPU）、MJPEG の段 | DEQUEUE、変換の fence | 待ちの列（上限 = hold、超えたら古い物を落とし数える）＋ notify |
| （呼び手の thread） | — | — | decoder・encoder・frame の呼び出し | GPU の fence（`VKVIDEO_WAIT_NS` 5 s を上限） | 戻り値 |

- software の decoder・encoder の並列（slice・行）は使う WS（WS326 p024・WS329・WS330）が `thread.c` の pool を使って足す（この WS では作らない）。
- notify は lock を持たずに、待ちが空から非空になった時だけ呼ぶ（WS309 の main_woken と同じ）。app が take・status で空にすると再び呼べる。

### 7.2 lock の順

context の lock（子の数・LOST・pool）→ object の lock（camera の待ちの列と slot、engine の状態）→ queue の lock（libmedia の queue は libmedia の mutex、共有の queue は app の lock の callback）。notify と解放の callback は lock を全部外してから呼ぶ。frame の参照の数は atomic（`__atomic` の組み込み、libc の外の依存無し）。

### 7.3 GPU の同期

- libmedia の submit は全部 fence を付け、frame を渡す前・packet を返す前にその fence を待つ（呼び手の thread か camera の thread）。
- queue の間（video → compute）は binary semaphore。app との間は §3 D7（完了してから渡す、app は読み終えてから参照を落とす）。app の image を読む時は app の binary semaphore を待ちに入れる（wrap・written）。
- image は CONCURRENT（libmedia の family と app の reader の family）。layout: decode の image は DPB の layout、app に渡す image は `SHADER_READ_ONLY_OPTIMAL`（app は変えない）、encoder の入力は ENCODE_SRC の layout への遷移を libmedia が記録する。

## 8. 資源の所有と寿命（主な物）

| 資源 | 持ち主 | 消す時 |
| --- | --- | --- |
| VkDevice・VkQueue（SHARED） | app | context の destroy の後 |
| VkDevice（PRIVATE） | context | context の destroy |
| 変換の pipeline・descriptor・command pool | context | context の destroy |
| decode の DPB の image | decoder（今の `media_gpu_slots` を frame.c の image の束へ） | decoder の close と、その image を持つ最後の frame |
| 出力の sample できる image（copy・変換の先） | decoder の出力の pool | pool は decoder の close で dead、最後の frame で消える |
| camera の slot の memory（import は libmedia の VkDeviceMemory、device は kernel の mmap） | camera | camera の close と最後の frame（import の memory は kernel が handle を `CAM_REQUEST_BUFFERS` 0 まで持つので、close は stream を止め buffer を外してから memory を free） |
| export の fd（camera の import） | libmedia の内部の一時物 | `CAM_QUEUE_BUFFER` の最初の 1 回の後すぐ close（kernel は handle を自分で retain する） |
| encoder の入力の変換の image・bitstream の buffer・feedback の query pool | encoder | encoder の close |
| wrap の frame の image | app | 解放の callback の後に app が消す |

## 9. 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| VK_ERROR_DEVICE_LOST | context を LOST に、notify(MEDIA_EVENT_LOST) を 1 回。以後の呼び出しは MEDIA_PROBLEM_LOST（close・unref は通る）。app は子を閉じ frame を落として作り直す。decoder の software の backend へは切り替えない（出力の image が失われた device の物なので） |
| VK_ERROR_OUT_OF_DEVICE_MEMORY | その呼び出しを ENOMEM。decoder は次の backend に回さない（一時の不足で backend を変えない） |
| hardware の session の不足 | open で BUSY → 次の backend |
| libavcodec が無い・知らない版 | 行の state が MISSING・VERSION。decode・encode は次が無ければその値 |
| GPU の fence が 5 s で来ない | ETIMEDOUT を EIO として返し、decoder は旧い backend を閉じて次へ（今の vkvideo の扱いを保つ） |
| camera が抜けた | camera の thread が DEQUEUE の ENODEV を見て止まり、notify。take が ENODEV |
| camera の stream の誤り | frame の flag（CAM_FRAME_ERROR・INCOMPLETE）は落として数える。連続 30 回で EIO |
| import が `EXDEV` | 黙って `CAM_MEMORY_DEVICE` と upload に戻し、media_log に理由を 1 行 |

## 10. libmedia の外の変更

### 10.1 HAL

変えない。この WS の要る物は全部 userland（libmedia・app）と driver（camera・i915 の share）と libc の header で、HAL の契約（`include/hal/hal.h`）に関わる物が無い。

### 10.2 Vulkan の header（`include/libc/vulkan/`）— 承認の項目

KHR の encode（`VK_KHR_video_encode_queue`・`_h264`・`_h265`）と H.265 の std（decode・encode）・H.264 の encode の std は 1.3.269 の header に無い。Khronos の Vulkan-Headers の 1.3.274 以降の固定の tag から、`API-PROVENANCE.md` の手順（Noct の `maintain-api.noct`）で header と `vk_video/` を作り直す。target の build は共有の sysroot（`build/amd64/sysroot`）の header を読むので、sysroot の作り直しは Q1（toolchain・共有の build/ の規則）。enum の名前が EXT から KHR に変わるが、今の tree の使い手は decode だけ（libvulkan の `video.c`・libmedia・i915 の executor の H.264 decode）なので影響は build で確かめる。→ Q318-3。

### 10.3 libvulkan

libvulkan は今 H.264 の decode の命令と構造体だけを kernel の executor へ送る。encode と H.265 の命令（`vkCmdEncodeVideoKHR`・`vkGetEncodedVideoSessionParametersKHR`・H.265 の構造体の wire）は、それを実装する driver の WS（最初は WS319）で足す。この WS の libmedia は関数を proc addr で引き、無ければその行を DEVICE にする（壊れない）。

### 10.4 camera の import を i915 へ（kernel、driver の変更）

- `src/drivers/gpu/i915/resource.c`: share の ops に `get_allocation_backing`（gem の object の連続の run を 1 つの run として、`DRV_GPU_BACKING_CONTIGUOUS | DRV_GPU_BACKING_COHERENT`、UNCACHED 無し）。
- `src/drivers/camera/cam.c` の `cam_slot_import`: COHERENT（CPU の cache が GPU の読みに見える）なら cached の kernel の別名（direct map、`kern_pmem_to_kernel` の類）で書き、UNCACHED なら今のまま uncached の別名。どちらでもなければ `EXDEV`。
- 理由: i915 の memory は LLC で CPU と coherent。uncached を要るのは bcm2711（Pi 5）の都合。HAL の口（`kern_pmem_*`）は今のままで足りる（kernel の口を使うだけで HAL の宣言に触れない）。
- `include/drivers/gpu/gpu-scanout.h` の `DRV_GPU_BACKING_COHERENT` は既にある。

## 11. 他の WS との境（どちらが持つか）

| 物 | 持つ WS | 注意 |
| --- | --- | --- |
| `h264sw-*.c`（decoder の module） | WS326 | WS318 は触らない |
| `h264sw-backend.c` の最初の実装、`decoder.c` の表 `{vkvideo, h264sw, aac}` と遅れた拒否の切り替え | WS326 p023 | WS318 の p004 は p023 の merge の後に libavcodec の行を足す |
| decoder の ops の v2 への変更と、それに合わせた `h264sw-backend.c` の出力の直し（picture の pool → CPU の frame） | WS318 p006 | WS326 の completed の後（header の凍結 `BASE=fd1d6f88c` が外れた後）。p006 は h264sw-backend.[ch] を変えてよい |
| software の decoder の出力の複写の削減 | WS326 p024 は decoder の内部の速さ。出力の形は WS318 p006 | §15 の注意 1 |
| H.264 の software の encoder | WS329 | WS318 p012 の encoder の ops と表の位置（hardware の後、libavcodec の前） |
| H.265 の software の decoder と encoder | WS330 | WS318 の `h265*.c`（StdVideo の解析）との共通化は WS330 p001 が決める |
| 共通の名前（bits・NAL・CABAC など、WS326 p026） | WS326 | WS318 の `h265*.c` は WS326 p026 の後なら共通の bit reader を使ってよい（設計の Phase p016 で決める） |
| camera の枠組みと uvc、bcm2711 の import | WS282 | WS318 は i915 の import（§10.4）と libmedia の source。UVC の実機の動作は WS282 の再開が前提（Q318-6） |
| 個別の driver の Vulkan Video（decode・encode） | WS319〜WS325 | WS318 の vkfake の試験の項目を driver の WS の受け入れに渡す（§13.6） |
| AAC-LC の encoder の低い層（A2DP） | WS205 | 窓の切り替え・TNS を足しても `media_aac_*` の API は変えない（A2DP の呼び手は既定で今の動き。新しい道具は encoder の口から使う） |

## 12. 移行の手順と app の書き換えの範囲

順に、各段で tree が build でき今の app が動く形を保つ。

1. **p004（入り口を揃える、今の機能のまま）**: libavcodec の decode の add-in を libmedia の `avcodec.c` へ（`kl_once` → `pthread_once`、`app_*` → `media_*`、`bitstream.c` は libmedia の物を使う）、`decoder.c` の表の最後に `media_avcodec_ops`。Video Player・Music は `app_decoder_*` を `media_decoder_*` に（同じ形の関数）、Makefile から `media-app/*.c` を外す。`media-app/` の 6 file は Q1 に消す依頼。`plan/tools/media/run-host-codec.sh` を libmedia の avcodec に追従（Q1 の許可）。
2. **p005〜p008（新しい口を足す、旧い口は shim）**: context・frame・変換・thread を足す。旧い `media_decoder_open` は process の暗黙の context（PRIVATE、`media_decoder_gpu_pictures` の値）の上の shim、`media_frame_gpu` の fd の export は shim の中だけに残す。`media_engine_open` と `wake_fd` は shim（内部の pipe を notify で書く）。
3. **p009 Video Player**: `present.c` の device の作成に requirements を足し、`media_context_create(SHARED)`、`media_decoder_create(... MEDIA_MEMORY_VULKAN, {NV12, BGRA8})`、`media_frame_vulkan` で view を作る（fd の import と `VP_GPU_PRESENT` の分けを外す）。CPU の道（共有できない時）は `media_frame_planes` と既存の upload。
4. **p010 残りと旧い口の削除**: Music（NONE の context、音だけ）、media-probe（`media_frame_planes` で hash、`media-private.h`・`picture.h` の include を外す）、libbrowser の engine（`media_engine_create` と notify）。旧い口（`media_decoder_open`・`media_decoder_gpu_pictures`・`media_frame_gpu`・`media_frame_free`・`media_codec_load`・`media_codec_reason`・`media_engine_open`・`media_engine_wake_fd`・`struct media_gpu_picture`）と `media-decoder.h` を消し、`exports.map` を新しい口に。`plan/tools/rtx-video/` の media-probe の使い方が変わる所を追従（Q1 の許可）。
5. app の書き換えの範囲はこの 4 つ（Video Player・Music・libbrowser の engine・media-probe）。Photos・Phone・camvk（kernel の試験の道具なので `/dev/camN` を直に使い続ける）は変えない。

## 13. 試験の道

### 13.1 host の単体試験

- `plan/ws318/tests/run-unit.sh NAME...|all`（WS326 と同じ形: host の cc、`-std=c89`、ASan・UBSan、最後の行 `run-unit: PASS`）と `check-skeleton.sh`（header の基点の照合、cc と clang で warning 0）。p002 で作る。
- 対象: frame の参照の数と pool、形式の交渉（表の全組）、codec の問い合わせの順と指定、問題の値の選び（§4.9）、切り替え（偽の backend の表）、encoder の選び（CONFIG）、packet の枠（Annex B・長さ・ADTS・avcC・hvcC）、camera の層（偽の source の ops: 連続の frame・落とし・抜け・import の EXDEV）、thread の台帳、H.265 の各 module（標準廉価実装手順の試験）、AAC の窓の切り替え・TNS。

### 13.2 偽の Vulkan Video（vkfake、p003）

- host の lavapipe（実の Vulkan、CPU で動く）の上に、試験の `vkGetInstanceProcAddr` で video の queue family と `vkCmdDecodeVideoKHR`・`vkCmdEncodeVideoKHR`・session・parameter・video の caps と形式の問い合わせ・encode の feedback の query を差し込む。libmedia は app の proc addr だけを使う（D3）ので、libmedia を変えずに挟める。
- video の queue への submit は lavapipe の queue への submit に置き換える（wait と signal の semaphore・fence を保つ）。decode は「bitstream の hash から決まる模様」を DST の image に copy で書き、encode は SRC の image を読んで hash を bitstream の buffer に書き、feedback に大きさを返す。
- 記録: session・parameter の Std の構造体の値、DPB の slot の使い方、参照の list を log にし、試験が期待と比べる（H.264 の decode の今の道の回帰、H.265 の decode の解析の結果、encode の GOP と parameter）。
- caps と形式は設定で変える（i915 の形: NV12・OPTIMAL・SAMPLED 無し、Pi 4 の形: SAMPLED あり、encode の有り無し、rate control の種類）。i915 の形で §6.2 の copy の道を host で確かめる。
- lavapipe は compute と transfer が実なので、変換の shader の画素を CPU の参照の式（試験の中の独立の実装）と比べられる。

### 13.3 host の codec の試験

- `run-host-codec.sh`（p004 で追従）: libmedia の中の libavcodec の decode（host の FFmpeg 7、libavcodec 61）を今の試料で。
- p012: host の libavcodec 61 で encode（libx264・libx265・aac）→ libmedia の decode（h264sw・libavcodec・aac）で往復。PSNR と frame 数を見る。AAC は独自の encoder → 独自の decoder の SNR。
- H.265 の解析: ITU-T H.265.1 の HEVC の適合の bitstream（JCT-VC の conformance、Main の範囲）を WS326 と同じ形（パラメータの無い GET・SHA-256・cache は main の `build/sources/h265-conformance/`）で取り、全部を解析で誤り無く通し、Std の値を host の `ffmpeg -bsf:v trace_headers` の値（副の oracle）と比べる。取得の大きさは設計の Phase（p016）で見積もり、disk の空き（今 約 9 GB）に合わせて範囲を決める。

### 13.4 QEMU（Venus、amd64）

- Video Player: Venus は Vulkan Video が無いので h264sw（WS326）→ 共有の device の VkImage へ upload の道。`plan/tools/rtx-video/venus-videoplayer.sh`（WS243、image は `config-amd64-venus-video.mk`）を新しい道に合わせて使う（判定は今の script の方法、console の log では判定しない）。
- 起動の確かめは `boot-test.sh`。Music の再生は `tests/scenarios/` の Music の物があれば使う（p010 で確かめる）。

### 13.5 実機

- **5330（i915、基準）**: Video Player の H.264 1080p30・1080p60 を Vulkan Video の decode → plane の copy → app の sample（readback 無し）で。decode の結果の hash を software と比べ（`plan/tools/rtx-video/nv12-hashes.py` の形）、表示の数と late を見る（WS243 の計り方）。p026。
- **RTX 2070（nvrtx）・Pi 4**: SAMPLED のある device の非退行（今の GPU の道）。Q1 の割り当て。
- **camera**: 5330 ＋ webcam（WS282 の実機の動作の後、Q318-6）。YUYV の mode で表示と encode（encode は libavcodec が無ければ AAC 以外は無いので、表示と frame の流れと import の確かめが主）。

### 13.6 driver の WS に渡す試験

vkfake で確かめた項目（session の作成、parameter、GOP、feedback、H.265 の Std の値）を、WS319 以降の driver の WS の受け入れ（実の device で同じ試料を encode・decode し、hash・PSNR を見る）に渡す。この WS の hardware の encode と H.265 の decode は、実機の確かめが driver の WS の最初の受け入れになる（Q318-1）。

## 14. Phase の表（案）

担当の型: O = Opus（phase-runner、kernel は phase-runner-high、app は phase-runner-mid）、S = Sonnet 5.5 high（phase-runner-low、標準廉価実装手順）、D = document-writer・conformance-reviewer。大きさは 1 回の起動で済む物に分けた。

| Phase | 目的 | 型 | 依存 | 受け入れ |
| --- | --- | --- | --- | --- |
| p001 | 設計（この文書、proposed の header、問い、design-reviewer） | O | — | Q1 の受け入れ、Q318 の回答 |
| p002 | 公開の header（proposed → `media.h` に足す・`media-vulkan.h`）、内部の header（ops v2 は新しい名前で並べる）、新しい file の骨格（ENOSYS）、Makefile・exports、`run-unit.sh`・`check-skeleton.sh` | O | p001、Q318-3 の答え（encode の宣言の扱い） | build warning 0（amd64）、`check-skeleton: PASS`、今の app と試験が変わらず通る（run-host-codec） |
| p003 | 試験の道具 vkfake（§13.2）と、その自己試験（lavapipe の上で H.264 の decode の今の runtime を通す） | O | p002 | 今の `vkvideo-runtime.c` を vkfake で H.264 の decode させ、記録の Std の値が手の計算と合う |
| p004 | libavcodec の decode を libmedia へ、表の最後へ。Video Player・Music を `media_decoder_*` に、`media-app/` を外す（今の機能のまま） | O | WS326 p023（main） | run-host-codec PASS（libmedia の中の libavcodec）、amd64 build warning 0、QEMU の Video Player（Venus）で libavcodec と h264sw の両方の道 |
| p005 | context と device の共有の核（NONE・PRIVATE・SHARED、requirements、queue と lock、vkvideo-runtime を context の device に、旧い口の shim） | O | p003、p004 | 単体試験、vkfake で SHARED・PRIVATE の H.264 の decode、旧い口の app が変わらず動く（QEMU） |
| p006 | frame の model と decode の出力（参照の数、CPU・VULKAN・WRAP、i915 の plane の copy、software の upload、ops v2 と h264sw・aac・avcodec の backend の追従、export を内部へ） | O | p005、WS326 completed | 単体試験、vkfake の i915 の形と Pi 4 の形の両方で VULKAN の frame、5330 で plane の copy の可否を確かめる（短い試験） |
| p007 | 変換（compute の shader と生成、交渉、§6.2 の全行、readback） | O | p006 | lavapipe の上で全組の画素を CPU の参照の式と照合（許容 ±1）、spirv-val |
| p008 | thread の台帳と notify、engine の callback（shim で wake_fd を残す）、libbrowser の engine を notify に | O | p005 | 単体試験、browser の `<video>` の host 試験か QEMU（既存の browser の試験） |
| p009 | Video Player を新しい口へ（SHARED、VkImage の frame、fd の import を外す） | O（mid） | p007、p008 | QEMU Venus の Video Player、5330 の短い再生（p026 の前の確かめ） |
| p010 | Music・media-probe・libbrowser を新しい口へ、旧い口と shim・`media-decoder.h` を消す、exports.map、試験の道具の追従 | O（mid） | p009 | build warning 0、run-host-codec、QEMU の Video Player・Music、`nm` で旧い symbol が無い |
| p011 | Vulkan の header の更新（§10.2）と既存の build の確かめ（sysroot は Q1） | O ＋ Q1 | Q318-3 の承認 | 全 arch の libvulkan・libmedia・i915・bcm2711・nvrtx の build warning 0、H.264 の decode の回帰（vkfake） |
| p012 | encoder の核（`encoder.c`・`codec-query.c`・packet の枠・codec data）、AAC-LC の backend、libavcodec の encode の add-in | O | p007、p011 | 単体試験、host の往復（§13.3）、問い合わせの表の試験 |
| p013 | AAC の追加の設計（標準廉価: 窓の切り替えと短い窓・TNS の header と仕様の comment、試験の道具、phase.md） | O | p012、Q318-5 | design-reviewer、骨格の build |
| p014 | AAC: 窓の判定・窓の列・短い窓の grouping と scalefactor | S | p013 | run-unit PASS、往復の SNR、突然変異の確かめ |
| p015 | AAC: TNS の解析・量子化・符号化 | S | p013 | 同上 |
| p016 | H.265 の Vulkan Video の decode の設計（標準廉価: module・header・骨格・適合の取得・phase.md） | O | p011 | design-reviewer、骨格の build、適合の取得の script |
| p017 | H.265: NAL と VPS・SPS（VUI の要る部分）→ StdVideoH265 | S | p016 | run-unit、適合の stream の SPS の値 |
| p018 | H.265: PPS と slice segment header | S | p017 | 同上 |
| p019 | H.265: POC・RPS・DPB の marking と出力の順 | S | p018 | run-unit、手の計算の例 |
| p020 | H.265 の decode の runtime（vkvideo の H.265 の道、vkfake の試験、適合の全 stream の解析） | O | p006、p019 | vkfake の記録が trace と合う、解析の誤り 0 |
| p021 | Vulkan Video の encode の backend（H.264: session・SPS/PPS・GOP の IDR と P・rate control・feedback・入力の image） | O | p012 | vkfake の記録（GOP・parameter・rate control の落とし）、packet の枠の試験 |
| p022 | Vulkan Video の encode の backend（H.265） | O | p021、p017 | 同上 |
| p023 | camera の口と zedBSD の source（列挙・mode の選び・DEVICE の mmap と upload・IMPORT・再投入・落とし・抜け） | O | p007、p012 | 偽の source の単体試験、amd64 build、5330 の build |
| p024 | MJPEG の段（Q318-4 の答えによる。推しは CPU の entropy の復号 ＋ GPU の IDCT と変換） | O（推しなら設計 O と実装 S に分ける） | p023、Q318-4 | 単体試験（T.81 の例と往復）、camera の偽の source |
| p025 | kernel: camera の import を i915 の coherent な memory へ（§10.4） | O（high） | p023 | amd64 の vmunix warning 0、WS282 の host 試験（cam）が通る、5330 で import の可否（webcam があれば） |
| p026 | 実機の受け入れ: 5330 の Video Player（i915 の GPU の道、1080p30・60、hash）、RTX・Pi 4 の非退行、QEMU Venus | O | p010、p020（H.264 の回帰のため） | §13.5 |
| p027 | camera の実機（5330 ＋ webcam、表示と frame の流れと import） | O | p023〜p025、WS282 の実機（Q318-6） | 連続の frame、落とし、抜けと差し直し |
| p028 | 解説の文書（`docs/reference/` の libmedia）と規約の全文の見直し | D | 全部 | 規約の checklist、文書の例と照合 |

- WS326 に触れない Phase（p002・p003・p011・p013〜p019・p023〜p025）は WS326 の completed を待たずに始められる（始める順は Q1）。p004 は WS326 p023、p006 は WS326 の completed を待つ。
- 標準廉価実装手順の Phase（p014・p015・p017〜p019）は設計の Phase（p013・p016）が header の基点を決め、`BASE=` の照合を phase.md に書く。

## 15. Q1 への注意（ユーザーの判断ではない物）

1. WS326 p024（速さ）は decoder の内部の速さに限り、出力の `media_picture` への複写の削減は WS318 p006 の frame の形の後に回すのを推す（二重の作り替えを避ける）。
2. p004・p010 は `plan/tools/media/run-host-codec.sh`・`plan/tools/rtx-video/` の追従が要る（plan/tools は Q1 の物）。Phase の範囲に入れるか、Q1 が直す。
3. `media-app/` の 6 file の削除（p004）と `media-decoder.h` の削除（p010）は Q1 の pipeline。
4. host に Vulkan の validation layer（Debian の `vulkan-validationlayers`）を入れると vkfake の試験が Vulkan の使い方の誤りも拾える（disk は数十 MB）。入れるかは Q1。
5. p011 の sysroot の作り直しは Q1（toolchain-lock の unlock の間）。
6. 実機の割り当て: p006（5330 の短い確かめ）、p026（5330・RTX・Pi 4）、p027（5330 ＋ webcam）。

## 16. design-reviewer の結果と反映

[design-review.md](design-review.md) に全件と反映を記録した（下の要約は review の後に書く）。
