<!-- awesome-plan project=zedbsd record=ws318-design -->

# WS318 設計: libmedia の作り直し（Vulkan の device の共有・codec の問い合わせと指定・encoder・camera・libavcodec の内部の fallback）

- 親: [ws.md](ws.md)。作成: ws318-p001（P5、q1752-i01、2026-10-31）。調べた tree: main 843847185（worktree `p5-ws318`）。
- 正本: ユーザーの決定は ws.md の 1〜6 と設計の合意。この文書はその解釈と実装の設計。公開の interface の案は [proposed/media.h](proposed/media.h)・[proposed/media-vulkan.h](proposed/media-vulkan.h)（各関数の comment が仕様。p002 で source へ写す）。
- ユーザーの判断を待つ点は [user-questions.md](user-questions.md)（Q318-1〜14）。敵対的レビュー（37 件）と反映は [design-review.md](design-review.md)。
- HAL（`include/hal/hal.h`）は変えない（§10.1）。承認の項目は Vulkan の header と sysroot（Q1、§10.2）だけ。

## 1. 目的・範囲・範囲の外

目的（ws.md）: libmedia を Keiland の app の唯一の media の入り口にする。hardware は Vulkan Video の 1 本、無ければ libmedia の独自の software、無ければ libmedia の中で libavcodec を dlopen。decoder・encoder（H.264・H.265・AAC-LC）・camera を、app と共有する Vulkan の device の上で、fd を API に見せずに扱う。基準の機械は i915（5330）。

範囲:

1. 公開の API の作り直し: context（device の無し・libmedia の private・app と共有）、codec の問い合わせと指定（decode と encode）、decoder、frame（参照の数の picture、CPU か VkImage）、encoder、camera、engine の callback。
2. backend の表: decode は Vulkan Video → 独自の software（H.264 は WS326、AAC-LC）→ libavcodec。encode は Vulkan Video → 独自の software（AAC-LC。H.264 は WS329、H.265 は WS330 が後で足す）→ libavcodec。
3. libavcodec の decode の add-in を `userland/desktop/media-app/` から libmedia の中へ移し、encode の add-in を足す。
4. 形式を前提にしない: decoder・encoder・camera の形式を実行の時に問い、合わなければ GPU で変換（描画の pass と transfer）。
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
| 公開の header | `media.h`（engine）、`media-decoder.h`（decoder・frame・`struct media_gpu_picture`）、`media-sbc.h`、`media-aac-encoder.h`、`mediafile.h`。`exports.map` は glob（`media_decoder_*` が内部の `media_decoder_gpu_hold` も出している） | `userland/desktop/libmedia/` |
| decode の表 | `decoder_backends[] = { media_vkvideo_ops, media_aac_ops }`。h264sw の backend は骨格で表に未登録（WS326 p023 で `{vkvideo, h264sw, aac}` と遅れた拒否の切り替え） | `decoder.c:52`、`h264sw-backend.[ch]` |
| libavcodec | app の側の add-in。`media-app/decoder.c` が native を先に開き、DEVICE・PROFILE・FORMAT・BUSY と最初の packet の ENOTSUP・ENODEV・EBUSY で `app_avcodec_ops` へ。libavcodec が MISSING の時は native の問題の値を返す（`decoder.c:517`）。`avcodec.c` は dlopen（`libavcodec.so.61/63`）と `kl_once`（libkeiland）。`bitstream.c` は libmedia の物と接頭辞の他は同じ。header 無しで、構造体の先頭の field の配置だけを版ごとに照合（`avcodec-layout.h`、`plan/tools/media/host-layout.c`） | `userland/desktop/media-app/` |
| 使う所 | Video Player・Music（`media-app/*.c` を自分の source に入れる）、libbrowser の `page/media.c`（engine、`media_engine_wake_fd` を loader の watch に）、`media-probe`（`media-private.h`・`picture.h` を include し `struct media_frame` の field を読む、native だけの試験の道具）、`plan/tools/media/host-codec.c`（`media_vkvideo_ops` の偽物を定義） | 各 Makefile |
| Vulkan の道 | `vkvideo-runtime.c` が decoder ごとに `dlopen("libvulkan.so.1")`、instance と device を作る。GPU の picture は decode の image の memory を `OPAQUE_FD` で export、app が同じ作成の値の別の VkImage として import し UNDEFINED → SHADER_READ_ONLY に一度だけ移して sample（`present.c:1645`）。arm64 では readback を 4 枚の flight で重ねる（`vkvideo-runtime.c:37`） | `vkvideo-runtime.c`、`videoplayer/present.c` |
| DPB と出力 | i915・nvrtx・bcm2711 の driver は全部 `DPB_AND_OUTPUT_COINCIDE` だけ（出力の image は DPB の参照でもある） | 各 `vk-video.c` |
| i915 の NV12 | NV12 の image は video の image としてだけ作れる（create flags 0、usage は `DECODE_DST|DECODE_DPB|TRANSFER_SRC` の部分集合）。形式の feature に SAMPLED・TRANSFER_DST が無い。planar の image は一般の image の操作で断られ、NV12 で通る copy は image → buffer だけ。queue family は graphics 1 本・video decode 1 本（各 queueCount 1） | `i915/vk/vk-image.c:118,671,957`、`vk-instance.c:576-644`、`vk-command.c:2729` |
| Pi 4（bcm2711） | queue family は 1 つ（GRAPHICS・TRANSFER、video の session があれば DECODE も）、queueCount 1、**compute 無し** | `bcm2711/vk/vulkan-query.c:270` |
| RTX（nvrtx） | queue family は 1 つ（GRAPHICS・COMPUTE・TRANSFER・VIDEO_DECODE）、queueCount 1 | `nvrtx/vk/vk-device.c:604` |
| libvulkan | **Vulkan 1.0 だけ**（instance の apiVersion の minor が 0 以外は `VK_ERROR_INCOMPATIBLE_DRIVER`、物理 device も 1.0）。device の拡張は許可の表（知らない名前は断る）。sync2・video_queue・video_decode_queue・decode_h264・external_memory(_fd) はある。timeline semaphore・sampler YCbCr conversion は無い | `libvulkan/instance.c:59,1073`、`device.c:387` |
| Vulkan の header | Khronos 1.3.269 から選んだ物。`vk_video/` は H.264 の std と decode だけ。encode は 1.3.269 で provisional の EXT の構造体の番号だけ。H.265 の decode は 1.3.269 の Khronos の入力には KHR として入っているはず（1.3.238 で KHR、推定・未照合）だが、zedBSD の選びに入っていない | `include/libc/vulkan/`、`API-PROVENANCE.md` |
| libavcodec の package | LGPL の build、`--disable-encoders`（AAC を含め encoder は全部無い）。FFmpeg の native の LGPL の encoder に H.264・H.265 は無い | `userland/packages/multimedia/libavcodec/Makefile:58` |
| camera | `include/uapi/camera.h`（CAM_FORMAT_YUYV・NV12・MJPEG ほか、`CAM_MEMORY_DEVICE`・`CAM_MEMORY_IMPORT`、`struct cam_buffer.fd`）。`cam.c`: import は backing が CONTIGUOUS・RAM・UNCACHED の時だけ（それ以外 `EXDEV`）、handle は buffer の解放まで retain。STREAM_OFF は queue に入っていた物も含め全 slot を UNUSED に戻す、QUEUE は UNUSED の slot だけ、mmap が残る間は REQUEST_BUFFERS が EBUSY・close は buffer を解放しない。`get_allocation_backing` を持つ GPU は bcm2711 だけ。i915 の gem の object は 1 つの連続の run で、session の page は LLC を通る write-back | `cam.c:858-950,1040-1130,1552,1762,2232`、`i915/resource.c:385`、`ppgtt.c:32` |
| OS の境界の checker | `plan/tools/keiland-os-boundary/check.sh` の A1（`/dev/...` の文字列）・A5（`<uapi/...>`）は `userland/desktop/libmedia/` にも当たる | `check.sh:226-261` |
| thread | engine は `pthread_create` の 1 本（名前無し）。libc に `pthread_setname_np` は無く、`pthread_sigmask` はある | `engine.c`、`include/libc/pthread.h` |
| shader | Video Player は GLSL を `regenerate.py` で SPIR-V の配列の header にする（host の glslangValidator） | `videoplayer/shaders/` |
| host | lavapipe（Mesa 25.0.7、Vulkan 1.4、video 無し）のほか intel・nouveau・radeon・virtio の ICD の定義がある。glslangValidator・spirv-val はある、validation layer は無い。disk の空きは 13 GB（2026-10-31 の `df`、変わる） | host |

## 3. 決定（根拠と、選ばなかった案）

| # | 決定 | 根拠 | 選ばなかった案と理由 |
| --- | --- | --- | --- |
| D1 | 公開の header は `media.h`（Vulkan の型を持たない全部）と `media-vulkan.h`（Vulkan の型の部分）。`mediafile.h`・`media-sbc.h`・`media-aac-encoder.h` はそのまま。`media-decoder.h` は最後に `media.h` へ統合。`exports.map` は glob をやめ、名前を全部並べる | master-design-policy §2.6（少数の、まとまった header）。Vulkan を使わない使い手と host の試験が Vulkan の header 無しで compile できる。Windows などで Vulkan の部分だけ別に実装できる（ユーザーの決定 2）。glob は内部の関数を漏らす | 1 つにまとめる（全部の使い手が vulkan.h を要る）、機能ごとに 6 つ（§2.6 の細切れ） |
| D2 | context を新しく持つ。device の mode は NONE・PRIVATE・SHARED。旧い global（`decoder_gpu_hold`）は context と decoder の config へ | 共有の device は 1 度渡して decoder・encoder・camera が使う。PRIVATE は今の動き（libmedia の device と readback）を残す | 関数ごとに device を渡す（寿命と queue の lock が散る）、global の device（複数の device を持てない） |
| D3 | libmedia は SHARED では app の `vkGetInstanceProcAddr` だけで Vulkan に届く。PRIVATE は今どおり `libvulkan.so.1` を dlopen | 同じ dispatch を使う | libmedia が SHARED でも dlopen（app と別の loader の状態） |
| D4 | 要る物の問い合わせは 2 段（instance と device）。api_version は 1.0 に固定し、KHR の entry point だけを使う | libvulkan は 1.0 だけを受ける（§2）。大きい版を求めると instance の作成が失敗する | 1.1 以上の core を前提（zedBSD で動かない） |
| D5 | queue: どの役（decode・encode・変換）の queue も app と共有でき、共有の queue は app の lock の callback を通す。app は同じ lock を自分の submit・present・`vkQueueWaitIdle` に使い、libmedia の object がある間は `vkDeviceWaitIdle` を呼ばない。requirements は queue の足りない family に shared_hint | i915 は graphics 1 本、RTX と Pi 4 は decode も graphics も 1 本の queue（§2）。VkQueue は外部の同期が要る | decode・encode の queue を libmedia 専用（RTX・Pi 4 で成り立たない）、libmedia の変換を app の command buffer に記録（encode・camera の道に app の command buffer が無い） |
| D6 | frame（`struct media_frame`）を decoder・camera・encoder の共通の型にし、参照の数で生かす。種類は CPU・VULKAN・WRAP（app の image） | ユーザーの合意（camera の frame を表示と encode が同じ画像で使い、参照の数で返す）。名前は今の公開の `media_frame` を引き継ぐ | 新しい型 `media_picture`（内部の `struct media_picture` と衝突） |
| D7 | VULKAN の frame は plane の並び（plane ごとに image・aspect・view の形式・大きさ）で渡す。1 つの multi-planar の image の plane でも、plane ごとの別の image でもよく、app は plane ごとに view を作るだけ | i915 では sample できる NV12 の image が作れない（§2）ので、R8 と R8G8 の別の image でしか NV12 を渡せない。Pi 4・RTX は multi-planar の image の plane の view で渡せる。app の code は 1 通り | 1 つの VkImage だけ（i915 で NV12 を渡せない）、BGRA だけ（i915 の変換に描画の pass が要り、Pi 4 の直の道を捨てる） |
| D8 | 同期: libmedia は書き込みの完了（fence）を待ってから frame を渡す。app は自分の GPU の読みが終わってから参照を落とす。image は reader の family が 2 つ以上なら CONCURRENT、1 つなら EXCLUSIVE（family の移し無し）。app の image を読む時の binary semaphore は 1 回だけ待ち、以後の読みは libmedia の fence の順で | libvulkan に timeline semaphore が無い（§2）。今の GPU の picture の契約と同じ。CONCURRENT は 2 つ以上の family が要る | 渡す時に binary semaphore（app が 1 回しか待てず、複数の reader で壊れる）、timeline（無い） |
| D9 | 形式の交渉: app が受ける形式を好みの順に並べ、libmedia は (a) 変換無し (b) transfer の copy だけ (c) 描画の pass の変換、の順に最初に作れる物を選ぶ。i915 の H.264 の decode は「image → buffer の copy → buffer → R8・R8G8 の image の copy」の transfer だけで NV12 を渡せる（§6） | ユーザーの決定 3（NV12 の前提を捨て i915 を基準に）。i915 の executor が今受ける操作だけで組める | 形式を NV12 の 1 つの image に固定（i915 で作れない）、i915 の executor に SAMPLED な NV12 を足すのを前提（driver の WS319 の範囲、Q318-10） |
| D10 | 変換は描画の pass（全画面の三角形と fragment shader、出力は単一の plane の colour attachment: R8・R8G8・RGBA8・BGRA8）で行い、compute を使わない | Pi 4 は compute が無い（§2）。2026-10-12 の guardrail（画像は GPU で）の当の機械。i915・RTX・VF2 も graphics を持つ。R8・R8G8 の storage image（`shaderStorageImageExtendedFormats`）を避けられる | compute（Pi 4 で動かない）、compute と描画の 2 通り（code が 2 倍。必要なら速さの後の Phase で compute を足す） |
| D11 | 渡す image が DPB の参照でもある時（全 driver が COINCIDE）: (1) device が DISTINCT を出せば出力の image を DPB と分け、そのまま渡す。(2) COINCIDE で decode の image に SAMPLED と plane の view が出せれば、libmedia が同じ memory に同じ作成の値の別の VkImage（別名）を bind し、別名を SHADER_READ_ONLY に一度だけ移して渡す（今の fd の import の道を device の中に移した物）。(3) それ以外は copy（§6.2） | (2) は今 Pi 4・RTX で 1080p60 まで動いている道（WS243）で、性能を落とさない。ただし Vulkan の規則では別名の layout の遷移が内容を保つ保証が無く、zedBSD の driver が守る約束（driver の WS に引き継ぐ、§11） | DPB の image をそのまま渡す（DPB の layout では sample できない）、常に copy（Pi 4 の速さが落ちる） |
| D12 | decoder・encoder の呼び出しは呼び手の thread で同期（GPU を有限の時間だけ待つ）。libmedia の常駐の thread は engine と camera だけ。全部名前を付け、本数を決め、signal を block して作る。起こしは notify の callback（edge で 1 回、lock を持たずに、libmedia の thread からだけ） | WS309 の原則。decoder を使う app（Video Player）は既に自分の media の thread で decode している | decoder を非同期（app の作りと音・絵の同期が全部変わる）、fd の wake を残す（ユーザーの合意で外す） |
| D13 | backend の ops は 2 層: backend は自分の出力（CPU の plane か自分の VkImage）を作り、変換・upload は decoder の層と encoder の層が `convert.c` で行う。ただし hardware の decoder の PRIVATE の readback は今どおり runtime の中（flight の重ね）に残す | 変換を backend ごとに持たない（AHA、§2.6）。software の backend（WS326・WS329・WS330）は CPU の plane だけを出せばよい。Pi 4 の 720p30 の readback の重ねを崩さない | backend ごとに upload（同じ code が 4〜5 か所）、readback も層へ（Pi 4 の重ねが消える） |
| D14 | 遅れた拒否の切り替えは WS326 p023 の規則のまま（send の ENOTSUP・ENODEV・EBUSY）、表の最後に libavcodec。誤りの写しは §9 の表で決める（EIO・ETIMEDOUT は切り替えずに stream を終える、DEVICE_LOST は LOST）。全部が断った時の値は MISSING を優先（§4.9） | app が「libavcodec を入れれば見られる」と言える。今の vkvideo の振る舞い（失敗の decode は stream を終える）を保つ | 最初の backend の値（今の decoder.c、MISSING が出ない） |
| D15 | encoder は開いた後に backend を変えない。open の時に config の全部を受けられる最初の backend | 1 本の stream の packet が別の encoder の物に変わると壊れる | decoder と同じ遅れた切り替え |
| D16 | libavcodec の add-in は libmedia の中で `pthread_once`。encode の codec の context の設定は `av_opt_set_*`（option の名前、版で変わらない）、AVFrame・AVPacket の field は今の配置の照合を広げて使う | libmedia は libc だけに依る（libbrowser が link）。AVCodecContext の配置は版で変わる | libkeiland に依る、AVCodecContext の field を直に書く |
| D17 | camera の OS の部分は `camera-zedbsd.c` の 1 file（`<uapi/camera.h>` と ioctl と `/dev/cam` の文字列はここだけ）。OS の境界の checker の A1・A5 の許可の行を Q1 に頼む | OS の境界を 1 file に閉じる。後で Linux（V4L2）などを足せる | camera の層に ioctl を直に |
| D18 | camera の slot は libmedia が状態を持つ（§7.4）。最後の `media_frame_unref` が kernel への再投入（待たない ioctl 1 つ）を行うが、camera の lock の中で ioctl を呼ばない形にし、close の後の unref は ioctl をせず解放だけ | camera の thread は DEQUEUE で待っているので、そこへ頼むと全 buffer が持たれた時に遅れる。lock の中の unref の自己の deadlock を避ける | camera の thread に依頼（遅れる） |
| D19 | H.265 の Vulkan Video の解析（StdVideoH265 へ）は libmedia の `h265*.c` に、software の decoder（WS330）の解析とは分ける | WS326 の h264.c と h264sw-ps を分けた理由と同じ（Vulkan の header に依る構造体か、自分の単位か） | WS330 を待つ（WS318 が先に完了する予定に反する） |
| D20 | 標準廉価実装手順は仕様で振る舞いが決まる所（H.265 の解析・POC・RPS・DPB、packet の枠（Annex B・avcC・hvcC・ADTS・AudioSpecificConfig）、色の変換の式）と、設計の Phase が数値まで決めた AAC の判定（窓の切り替え・TNS）に使う。device の共有・変換の pass・camera・移行は普通の Phase（Opus） | spec-module-delegation §1。AAC の符号の形は ISO/IEC 14496-3 が決めるが、判定（過渡の検出・閾値・TNS の次数と on/off）は決めないので、設計の Phase が informative な出典で数値まで固定する | 全部を Sonnet（探りながら作る所は §1 の外） |
| D21 | 試験の偽の Vulkan Video（vkfake）は、host の lavapipe の上に重ねた「代わりの `libvulkan.so.1`」として作る（`LD_LIBRARY_PATH` で見つけさせ、本物の loader を絶対 path で開いて転送）。API は 1.0 に絞り、zedBSD の libvulkan の拡張の許可の表を真似る | SHARED（proc addr）と PRIVATE（dlopen）の両方を同じ道具で通せる。lavapipe の 1.4 が zedBSD に無い物を隠さない | proc addr の差し込みだけ（PRIVATE と今の runtime を通せない） |

## 4. API（要点。全文は proposed/ の header）

### 4.1 object と寿命

| object | 作る | 消す | 持つ物 | 寿命の条件 |
| --- | --- | --- | --- | --- |
| `media_context` | `media_context_create` | `media_context_destroy`（生きた子・frame があれば EBUSY） | device の束縛（関数の表・queue・lock）、変換の pipeline、frame の pool、log。libavcodec の読み込みは process で 1 回（context をまたいで共有） | SHARED の app の device より短い |
| `media_decoder` | `media_decoder_create` | `media_decoder_close` | backend の状態、track の写し、切り替えの状態、出力の pool | context より短い |
| `media_encoder` | `media_encoder_create` | `media_encoder_close` | backend の状態、入力の変換の image、出力の packet の buffer | 同上 |
| `media_camera` | `media_camera_open` | `media_camera_close` | device の fd（内部）、slot の表、thread | 同上 |
| `media_engine` | `media_engine_create` | `media_engine_close` | thread、file、decoder | 渡された context か、NULL なら自分の PRIVATE の context |
| `media_frame` | decoder・camera・`media_frame_create`・`media_frame_wrap_vulkan` | 最後の `media_frame_unref` | 画素（pool の image・buffer・camera の slot・app の image）と作り手への参照 | 作り手（decoder・camera）の close の後も生きる。context より短い |

### 4.2 共有の device の手順（SHARED）

1. app: `media_vulkan_instance_requirements(vkGetInstanceProcAddr, &ir)` → 自分の拡張と合わせて `vkCreateInstance`（apiVersion は 1.0）。
2. app: 物理 device ごとに `media_vulkan_device_requirements(get, instance, physical, MEDIA_QUEUE_DECODE|MEDIA_QUEUE_ENCODE|MEDIA_QUEUE_CONVERT, &dr)`。表示できる device の中で `dr.decode_codecs` の多い物を選んでよい。
3. app: `dr.extensions` を足し、`dr.synchronization2` なら `VkPhysicalDeviceSynchronization2FeaturesKHR` を鎖に、`dr.queues` の family と数を自分の queue の作成に足す（同じ family は数を合わせ、足りなければ自分の queue を共有）。`vkCreateDevice`。
4. app: `struct media_vulkan_device` を埋め（実際に enable した拡張、queue と役、共有の queue には lock、自分が frame を読む family）、`media_context_create({MEDIA_DEVICE_SHARED, &device, notify, data})`。以後 app は共有の queue の submit・present・`vkQueueWaitIdle` を同じ lock の中で行い、`vkDeviceWaitIdle` を使わない。
5. decoder・encoder・camera を context の上で開く。終わりは子を閉じ、frame を全部落とし、`media_context_destroy` の後に `vkDestroyDevice`。

要る拡張（device が持つ物だけを返す）: `VK_KHR_synchronization2`、`VK_KHR_external_memory`・`VK_KHR_external_memory_fd`（camera の import）、`VK_KHR_video_queue`、`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264`・`VK_KHR_video_decode_h265`、`VK_KHR_video_encode_queue`・`VK_KHR_video_encode_h264`・`VK_KHR_video_encode_h265`（header の後、§10.2）。instance: `VK_KHR_get_physical_device_properties2`、`VK_KHR_external_memory_capabilities`。multi-planar の形式（NV12）と plane の aspect は、今の runtime が libvulkan の上で既に使っている zedBSD の約束（`vk-image.c:1033`）で、1.0 の core の物ではない。

### 4.3 codec の問い合わせと指定

- `media_codec_query(ctx, MEDIA_DECODE|MEDIA_ENCODE, codec, infos, capacity, &count)`: 試す順の実装の一覧。各行に id・kind・名前・状態（0 か MISSING・VERSION・DEVICE、zedBSD の package で encoder が無い libavcodec の行は FORMAT）・profile・level・最大の大きさ・rate control・B-frame・参照の数・変換無しの形式・音の rate と channel。
- 指定: decoder・encoder の config の `allow`（kind の bit）か `implementation`（id 1 つ。断られても次へ回さない）。
- hardware の行は Vulkan の問い合わせ（video の caps・形式、encode の caps）を context の最初の問い合わせで 1 回取り、context に保つ。libavcodec の行は dlopen と `avcodec_find_{de,en}coder_by_name` で埋める（分からない値は 0）。

### 4.4 decoder

`media_decoder_create(ctx, track, config, &dec)` の他は今の関数のまま（send・receive・picture・sound・trim・frame_us・flush・close・name・backend）に `media_decoder_kind` を足す。`media_decoder_picture` は参照 1 つの frame を返す。frame の memory と形式は config に従い decoder の層が揃える（§6）。app が hold より多く持つ間は send が ENOBUFS。

### 4.5 frame

- 共通: ref・unref・size・aspect・memory・format・colour（行列・range・primaries・transfer・chroma の位置）・time。
- CPU: `media_frame_planes`（読みだけ）、`media_frame_planes_write`（`media_frame_create` で作った frame だけ）、`media_frame_scale`（CPU で描く使い手、browser の engine）。
- VULKAN: `media_frame_vulkan` が `struct media_vulkan_frame`（identity・形式・plane の並び・見える窓・layout）。identity は増えるだけの数で、別の image に再び使わない。app は identity で view を cache し、少数の LRU で古い物を（自分の GPU の仕事の後に）消す。今の fd の import は無くなる。
- 作る: `media_frame_create`（CPU、または plane ごとの単一の plane の image で colour attachment と transfer の先にできる物）、`media_frame_wrap_vulkan`（app の image、書いた semaphore、解放の callback）、`media_frame_written`（layout と semaphore）。

### 4.6 encoder

- `media_encoder_create(ctx, config, &enc)`。config: codec・allow/implementation・profile・level・大きさ・frame rate・rate control（DEFAULT・CQP・CBR・VBR と bitrate・max_bitrate・qp）・key frame の間隔・B-frame（実装の上限まで、libmedia の Vulkan Video の encoder は 0、Q318-9）・colour・音の rate と channel と bitrate・出力の枠。
- `media_encoder_input(enc, &memory, &format)`、`media_encoder_send_frame`・`media_encoder_send_sound`・`media_encoder_receive`（packet の bytes は次の receive まで encoder の物）・`media_encoder_codec_data`（parameter set・avcC・hvcC・AudioSpecificConfig）・`media_encoder_close`。
- AAC は既存の `media_aac_encoder_*`（A2DP の低い層、export は残す）を encoder の backend（`aac-encoder-backend.c`）に包む。

### 4.7 camera

- 列挙: `media_camera_count`・`media_camera_describe`・`media_camera_modes`（CAM_ENUM_FORMAT・ENUM_MODE・ENUM_INTERVAL から）。
- `media_camera_open(ctx, index, config, &cam)`: 大きさと rate に近い mode を、(1) encoder の入力の形式と同じ、(2) app の受ける形式と同じ、(3) libmedia が変換できる、(4) MJPEG（Q318-4）の順で選ぶ。memory: SHARED の context で import が使えれば `CAM_MEMORY_IMPORT`（libmedia が作った memory を export して kernel に渡す）、使えなければ `CAM_MEMORY_DEVICE`（mmap）と GPU への upload（CPU の複写が毎 frame 1 回、Q318-12）。
- `media_camera_start`・`stop`・`take`（EAGAIN・ENODEV・EIO、open からの落とした数）・`media_camera_mode`（選んだ mode と encoder へ直に行くか）・`close`。通知は config の notify（MEDIA_EVENT_CAMERA）。

### 4.8 engine と callback

`media_engine_create(ctx, path, source, flags, notify, data, &engine)`。ctx が NULL なら engine は自分の PRIVATE の context を作る（今の browser の `<video>` は Vulkan Video の decode と readback を使っているので、それを保つ）。engine の thread は状態の変化・picture の到着で notify を 1 回呼び、app が `media_engine_status` を読むと（engine の lock の中で状態を写す前に）再び呼べる状態に戻る。`media_engine_wake_fd` は無くなる。libbrowser は notify の中で自分の pipe に 1 byte 書き、今の `net_loader_watch` をその pipe に向ける（変更は `page/media.c` の中だけ、WS074 と調整）。

### 4.9 誤りの値

- errno: EINVAL、ENOMEM、EAGAIN（先に receive・待ちが空）、ENOBUFS（hold を超えた、codec data の大きさ）、ENOTSUP、EBUSY、ENOENT・EACCES・ENODEV（camera）、EIO（stream を続けられない）。
- `MEDIA_PROBLEM_*`: 今の 1001〜1006 に CONFIG（1007）と LOST（1008）を足す。全部の実装が断った時: libavcodec を試すべきだったが無い → MISSING、次に codec を知る最初の実装の断り、どれも codec を知らない時だけ FORMAT。今の app（media-app）は MISSING の時に native の値を見せるので、Video Player・Music の文言（`videoplayer/main.c:1468`、`music/main.c:714`）が変わる。意図した変更として p004 で文言を確かめる（Q318-13）。

## 5. 内部の構成

### 5.1 file（`userland/desktop/libmedia/`、新しい物に ★）

| file | 中身 | 由来 |
| --- | --- | --- |
| ★`context.c` | context・device の束縛・queue の lock・子の数・LOST | 新 |
| ★`vulkan-device.c` | requirements、proc addr からの関数の表、PRIVATE の device の作成（`vkvideo-runtime.c` の instance・device の作成を移し、context で 1 つに） | 一部移す |
| ★`frame.c` | `media_frame` の種類（CPU・VULKAN・WRAP・CAMERA）、参照の数（`__atomic`）、pool、identity の数 | 新（`picture.c` の pool は CPU の frame の下で使い続ける） |
| ★`convert.c`・★`convert-shaders.h`・★`shaders/*.vert,*.frag`・★`shaders/regenerate.py` | 交渉、変換の graph、描画の pass の pipeline、upload・plane の copy・readback | 新（作りは videoplayer の shaders に倣う） |
| ★`colour.c` | BT.601・709・2020 の行列と range の係数（表の値、出典は block ごと） | 新（S に向く） |
| ★`codec-query.c` | `media_codec_query`、hardware の caps の cache | 新 |
| `decoder.c` | decoder の層（WS326 p023 の切り替え）＋ context・config・出力の変換・libavcodec の行・§4.9 の値の選び | 変える |
| ★`encoder.c` | encoder の層: 選び・入力の変換・readback | 新 |
| ★`packet-format.c` | Annex B と長さの前置の変換、avcC・hvcC・ADTS・AudioSpecificConfig の組み立て | 新（S に向く） |
| ★`avcodec.c`・★`avcodec-layout.h` | libavcodec の add-in（decode を移し encode を足す） | `media-app/` から移す |
| ★`aac-encoder-backend.c` | 既存の AAC-LC の encoder を encoder の ops に | 新 |
| `aac-encode.c`（と新しい補助） | 窓の切り替え・短い窓・TNS（Q318-5） | 変える |
| `vkvideo.c`・`vkvideo-runtime.c` | context の device、H.265 の道、export を内部へ、D11 の別名と copy の道 | 変える |
| ★`vkvideo-encode.c`・★`vkvideo-encode-h264.c`・★`vkvideo-encode-h265.c` | Vulkan Video の encode の backend | 新 |
| ★`h265*.c`・★`h265*.h`（module は p016 で確定） | H.265 の NAL・VPS・SPS・PPS・slice segment header を StdVideoH265 へ、POC・RPS・DPB | 新（標準廉価実装手順） |
| ★`camera.c`・★`camera-zedbsd.c`・★`camera-source.h` | camera の層と zedBSD の source | 新 |
| ★`thread.c` | 名前の付いた thread の台帳 | 新 |
| `engine.c` | context と notify | 変える |
| `media-private.h` | ops の表（decoder v2・encoder・camera source・frame の種類） | 変える |
| `media-decoder.h` | 移行の間は旧い宣言、最後に `media.h` へ統合して消す | 消す（Q1） |

### 5.2 decoder の backend の ops（v2、`media-private.h`）

- `kind`、`query(ctx, codec, info)`、`open(ctx, track, out, state)`（`out` は decoder の層が決めた backend の出力の希望: hardware なら D11 の (1)〜(3) のどれか、CPU なら plane の形式）。
- `picture(state)` は backend の frame（`struct media_frame *`）を返す。picture_free・picture_size・picture_scale・picture_aspect・picture_gpu は frame の ops へ移る。
- `send`・`receive`・`sound`・`flush`・`close`・`trim`・`frame_us`・`decoder_name` は今のまま。
- software の backend（h264sw・aac・avcodec）は CPU の frame を出す。decoder の層が config の memory が VULKAN なら upload（§6）。hardware の PRIVATE の readback は runtime の中（D13）。

### 5.3 encoder の backend の ops（新）

`kind`・`query`・`open(ctx, config, state)`（config を全部受けられなければ MEDIA_PROBLEM_CONFIG）・`input(state, &memory, &format)`・`send_frame`（入力は `input` の形に揃え済み）・`send_sound`・`receive`・`codec_data`・`close`。
表（`encoder.c`）: `{ vkvideo_encode_h264, vkvideo_encode_h265, aac_lc, avcodec }`。WS329・WS330 は software の行を hardware の後・avcodec の前に足す。

### 5.4 backend の順の表

| codec | decode | encode |
| --- | --- | --- |
| H.264 | vulkan-video → h264-software（WS326）→ libavcodec:h264 | vulkan-video → （h264 software、WS329）→ libavcodec（encoder を持つ libavcodec があれば） |
| H.265 | vulkan-video（この WS で解析を作る）→ （h265 software、WS330）→ libavcodec:hevc | vulkan-video → （h265 software、WS330）→ libavcodec（同上） |
| AAC-LC | aac-lc（独自）→ libavcodec:aac | aac-lc（独自）→ libavcodec:aac（同上） |
| AV1・VP9・VP8・MPEG-4・Opus・MP3・Vorbis・Theora・MJPEG・PCM | libavcodec（今の `codec_names[]`） | — |

### 5.5 遅れた拒否の切り替え（decode）

WS326 p023 の規則（`h264sw-backend.h` の comment）をそのまま使い、libavcodec を表の最後に置く。`implementation` を指定した decoder は切り替えない。切り替えで出力の形が変わっても decoder の層の交渉が app の求める形に揃える。

## 6. 形式と変換

### 6.1 交渉

1. decoder の層は backend の open の前に、backend が出せる出力（hardware: `vkGetPhysicalDeviceVideoFormatPropertiesKHR` の DECODE_DST の形式・tiling・usage・create flags と DPB の mode、software: CPU の plane の形式）を得る。
2. app の `formats` を順に見て、(a) 変換無し、(b) transfer の copy だけ、(c) 描画の pass の変換、の順に最初に作れる物を選ぶ。`format_count` 0 は backend の形式のまま（VULKAN で sample できなければ (b)）。作れる物が無ければ create は ENOTSUP。
3. 選んだ道を decoder に保ち、backend が変わった時（§5.5）にやり直す。

### 6.2 変換の graph（CPU は画素を触らない。例外は §6.4）

| 元 | 先 | 道 |
| --- | --- | --- |
| hardware の decode（DISTINCT で出力に SAMPLED） | 同じ形式の VULKAN | 無し（出力の image の plane をそのまま） |
| hardware の decode（COINCIDE、decode の image に SAMPLED と plane の view: Pi 4・RTX） | 同じ形式の VULKAN | 無し（D11 の別名の image の plane） |
| hardware の decode（SAMPLED 無し: **i915**） | NV12 の VULKAN | `vkCmdCopyImageToBuffer`（PLANE_0・PLANE_1）→ `vkCmdCopyBufferToImage` で R8 と R8G8 の別の image へ（transfer だけ） |
| 同上 | BGRA8・RGBA8 | 上の 2 枚の image を sample する描画の pass で RGB の image へ（transfer ＋ 描画 1 pass） |
| CPU の frame（software・libavcodec の decode） | VULKAN（NV12・I420 の plane） | host-visible の staging buffer へ（§6.4）、`vkCmdCopyBufferToImage` で plane ごとの image へ |
| 同上 | RGB の VULKAN | 上の plane の image を sample する描画の pass |
| camera の import の memory（YUYV・UYVY・NV12） | encoder の入力（NV12 の ENCODE_SRC） | 同じ形式で ENCODE_SRC が LINEAR を許し pitch が合えば、同じ memory に image を bind（pass 無し）。それ以外は buffer → R8G8B8A8 の image への copy（YUYV を 2 画素の組として）→ 描画の pass で R8・R8G8 の plane へ → encoder の入力への copy（encoder の入力の形と usage は driver の caps で決まる。p021 で確かめる） |
| camera の frame | 表示の VULKAN | 同じ形の copy と描画の pass で NV12 の plane か RGB へ |
| VULKAN の任意 | CPU（PRIVATE の encoder・CPU の encoder） | 描画の pass か copy で plane の image → `vkCmdCopyImageToBuffer` → fence → map |
| app の RGB の image | encoder の NV12 | 描画の pass の RGB → YUV（行列は config の colour）で R8・R8G8 へ |

- shader（GLSL → SPIR-V の配列、videoplayer と同じ `regenerate.py`。全画面の三角形の vertex 1 つと fragment）: `yuv420-to-rgb`（NV12・I420 の plane を sample、行列・range は push constant）、`packed422-to-yuv420`・`packed422-to-rgb`（YUYV・UYVY を RGBA8 の image として sample）、`rgb-to-yuv420`（luma と chroma の 2 pass）。
- 色の係数: ITU-R BT.601・BT.709・BT.2020 の式と ITU-T H.273 の range（`colour.c`、値は block ごとに「Values referenced:」で出典を書く）。
- 描画の pass の出力の形式（R8・R8G8・RGBA8・BGRA8）の colour attachment の可否は `vkGetPhysicalDeviceFormatProperties` で確かめ、できない形式は交渉で選ばない。

### 6.3 i915 を基準にした時の条件

- i915 の executor が今受ける操作だけで NV12 を app に渡せる（§6.2 の 3 行目）。transfer は 2 回（image → buffer、buffer → 2 枚の image）。i915 は UMA なので buffer は device-local・host-visible の memory で、帯域は 1080p で 1 frame あたり約 6 MB（読み書き）。
- i915 の driver が decode の出力に SAMPLED（か DISTINCT の出力）を出すようになれば、§6.2 の 1・2 行目の道になり copy が無くなる。driver の変更を WS318 に入れるか WS319 に置くかは Q318-10（推しは WS319）。
- i915 は queue が graphics 1 本・video 1 本なので、変換は app の graphics の queue を lock で共有する（D5）。

### 6.4 CPU の複写（2026-10-12 の guardrail との関係）

次の 2 つは毎 frame CPU が画素を 1 回写す。どちらも GPU の外の producer（software の codec、kernel の mmap）からの受け渡しで、host の pointer の import（`VK_EXT_external_memory_host`）が libvulkan に無いため避けられない。Q318-12 でユーザーに確かめる。

1. software・libavcodec の decode の出力 → staging buffer。p006 で decoder の層が staging の buffer を CPU の frame の pool として渡し、backend が直に書く形（h264sw は内部の frame から出力の plane への複写が既に 1 回あるので、その先を staging にして追加の複写を 0 にする）。libavcodec は AVFrame から staging への 1 回（8 bit 4:2:0 と 4:2:2・RGB などは CPU で変換せず、plane をそのまま写して GPU の pass で変換。対応しない pixel 形式（10 bit など）は ENOTSUP で次へ）。
2. camera の `CAM_MEMORY_DEVICE`（import が使えない device）の mmap → staging。

## 7. thread・lock・同期

### 7.1 thread（全部 `thread.c` の台帳で名前・本数を持ち、signal を全部 block して作る。起動と終了を media_log に 1 行）

| 名前 | 種類 | 本数 | 仕事 | 待つ所 | 結果の渡し方 |
| --- | --- | --- | --- | --- | --- |
| media-engine | 常駐（engine の寿命） | engine ごとに 1 | file の read、decode、picture の ring | read、decode の中の GPU の fence、ring の空き | lock の下の状態 ＋ notify（edge） |
| media-camera | 常駐（start〜stop） | camera ごとに 1 | `CAM_DEQUEUE_BUFFER`（timeout 100 ms、camera の lock の外で）、frame の作成、変換（GPU）、MJPEG の段 | DEQUEUE、変換の fence | 待ちの列（上限 = hold、超えたら古い物を落とし数える）＋ notify |
| （呼び手の thread） | — | — | decoder・encoder・frame の呼び出し | GPU の fence（5 s を上限、§9） | 戻り値 |

- software の decoder・encoder の並列（slice・行）は使う WS（WS326 p024・WS329・WS330）が `thread.c` の pool で足す。
- notify は lock を持たずに、libmedia の thread からだけ、待ちが空から非空になった時だけ呼ぶ（WS309 の main_woken と同じ）。app が take の EAGAIN・status で空にすると再び呼べる。

### 7.2 lock の順

context の lock（子の数・LOST・pool）→ object の lock（camera の待ちの列と slot の表、engine の状態）→ queue の lock（libmedia の queue は libmedia の mutex、共有の queue は app の lock）。notify・解放の callback・最後の unref（落とした frame）は lock を全部外してから（落とす frame は局所の列に移し、unlock の後に unref）。frame の参照の数は `__atomic` の組み込み。

### 7.3 GPU の同期

- libmedia の submit は全部 fence を付け、frame を渡す前・packet を返す前にその fence を待つ（呼び手の thread か camera の thread）。
- queue の間（video → 変換）は binary semaphore。app との間は D8。
- layout: DPB は DPB の layout のまま。app に渡す plane は `SHADER_READ_ONLY_OPTIMAL`（D11 の別名は作った時に一度だけ UNDEFINED から移す。copy・描画の先は libmedia が移す）。encoder の入力は ENCODE_SRC の layout へ libmedia が移す（app の image は `media_frame_written`・wrap で知らされた layout から）。

### 7.4 camera の slot の状態

| 状態 | 意味 | 次へ |
| --- | --- | --- |
| KERNEL | kernel の空き列か転送中（QUEUE 済み） | DEQUEUE で WAITING |
| WAITING | libmedia の待ちの列（frame になった、take されていない） | take で HELD、上限超えで落として FREE |
| HELD | app か encoder が参照を持つ | 最後の unref で FREE |
| FREE | libmedia の持ち物、kernel に無い | 走っていれば再投入（QUEUE）で KERNEL、止まっていればそのまま |

- 再投入は「状態を camera の lock の中で FREE → QUEUING に変え、lock の外で `CAM_QUEUE_BUFFER`、結果を lock の中で KERNEL か FREE（誤り）に」。
- stop: STREAM_OFF は kernel の全 slot を UNUSED に戻すので、KERNEL の slot を FREE に。WAITING の frame は落とす。HELD はそのまま。start は FREE の slot だけを QUEUE する。
- close: stop の後に camera を「gone」にし、HELD の frame の最後の unref は ioctl をせずに資源を解放する。import の memory は kernel が handle を buffer の解放（REQUEST_BUFFERS 0）まで持つので、全 slot が FREE になってから REQUEST_BUFFERS 0、その後に VkDeviceMemory を解放する。HELD が残る間の close は fd を閉じず、最後の unref で後片付けをする。
- DEVICE（mmap）の道: frame は毎回 staging へ写してから slot を直ちに再投入する（§6.4）ので、mmap の範囲を指す frame は外に出ない。close は munmap の後に REQUEST_BUFFERS 0。

## 8. 資源の所有と寿命（主な物）

| 資源 | 持ち主 | 消す時 |
| --- | --- | --- |
| VkDevice・VkQueue（SHARED） | app | context の destroy の後 |
| VkDevice（PRIVATE） | context | context の destroy |
| 変換の pipeline・render pass・descriptor・command pool | context | context の destroy |
| decode の DPB の image（と D11 の別名） | decoder（今の `media_gpu_slots` を frame.c の image の束へ） | decoder の close と、その image を持つ最後の frame |
| 出力の plane の image（copy・変換の先） | decoder の出力の pool | pool は decoder の close で dead、最後の frame で消える |
| camera の slot の memory | camera | §7.4 |
| export の fd（camera の import） | libmedia の内部の一時物 | 最初の `CAM_QUEUE_BUFFER` の直後に close（kernel が handle を retain） |
| encoder の入力の変換の image・bitstream の buffer・feedback の query pool | encoder | encoder の close |
| wrap の frame の image | app | 解放の callback の後に app が消す |

## 9. 失敗と回復（誤りの写し）

| 起きた事 | 層の答え | 切り替え |
| --- | --- | --- |
| VK_ERROR_DEVICE_LOST | context を LOST に。呼び出しは MEDIA_PROBLEM_LOST（close・unref は通る）。libmedia の thread で見つけたら notify(MEDIA_EVENT_LOST) を 1 回 | しない（出力の image が失われた device の物） |
| VK_TIMEOUT（fence が 5 s で来ない） | EIO（stream を終える）、media_log に 1 行 | しない（今の vkvideo と同じ） |
| decode の失敗（status の query） | EIO（stream を終える。arm64 の Pi 4 は今どおり 8 枚まで飛ばす） | しない |
| send の ENOTSUP・ENODEV・EBUSY（backend の断り） | — | する（WS326 p023 の規則） |
| VK_ERROR_OUT_OF_DEVICE_MEMORY・OUT_OF_HOST_MEMORY | ENOMEM | しない |
| hardware の session の不足 | open で BUSY | 次の backend へ |
| libavcodec が無い・知らない版 | 行の state が MISSING・VERSION | 次が無ければその値 |
| camera が抜けた | camera の thread が止まり notify、take が ENODEV | — |
| camera の frame の誤り（CAM_FRAME_ERROR・INCOMPLETE） | 落として数える。連続 30 回で EIO | — |
| import が `EXDEV`・`ENOTSUP` | 黙って DEVICE（mmap）に戻し、media_log に理由 | — |

## 10. libmedia の外の変更

### 10.1 HAL

変えない。この WS の要る物は全部 userland（libmedia・app）と driver（camera・i915 の share）と libc の header で、HAL の契約（`include/hal/hal.h`）に関わる物が無い。§10.4 の kernel の変更も既にある kernel の口（`kern_pmem_to_kernel`、`i915/resource.c:385` で使用中）だけを使う。

### 10.2 Vulkan の header（`include/libc/vulkan/`）— Q1 の承認の項目（Q318-3）

- p011a: 今の入力（1.3.269）から H.265 の decode（`VK_KHR_video_decode_h265` と `vk_video/vulkan_video_codec_h265std*.h`）を選び直す。版は上げない。まず固定の tag の入力に含まれることを照合する（未照合の推定）。
- p011b: KHR の encode（`VK_KHR_video_encode_queue`・`_h264`・`_h265` と encode の std）は 1.3.274 以降にしか無いので、固定の tag を上げて作り直す（`API-PROVENANCE.md` の手順、Noct の `maintain-api.noct`）。enum の EXT → KHR の変化の使い手は今の tree に無い（encode を使う code が無い）ことを build で確かめる。
- どちらも target の build は共有の sysroot（`build/amd64/sysroot` ほか）の header を読むので、sysroot の作り直しは Q1（toolchain・共有の build/ の規則）。

### 10.3 libvulkan

libvulkan は今 H.264 の decode の命令と構造体だけを kernel の executor へ送り、拡張は許可の表で絞る。encode と H.265 の命令の転送と許可の表への追加は、それを実装する driver の WS（最初は WS319）で足す。libmedia は関数を proc addr で引き、拡張が無ければその行を DEVICE にする（壊れない）。

### 10.4 camera の import を i915 へ（kernel、driver の変更）

- `src/drivers/gpu/i915/resource.c`: share の ops に `get_allocation_backing`（gem の object の連続の run を 1 つの run として、`DRV_GPU_BACKING_CONTIGUOUS | DRV_GPU_BACKING_COHERENT`）。
- `src/drivers/camera/cam.c` の `cam_slot_import`: COHERENT なら cached の kernel の別名（direct map）で書き、UNCACHED なら今のまま uncached の別名。どちらでもなければ `EXDEV`。
- 理由: i915 の memory は LLC で CPU と coherent。uncached が要るのは bcm2711（Pi 5）の都合。
- 未確認: libvulkan の i915 の memory の export が object 全体の allocation だけの共有になるか（p025 の最初に確かめる）。

## 11. 他の WS との境（どちらが持つか）

| 物 | 持つ WS | 注意 |
| --- | --- | --- |
| `h264sw-*.c`（decoder の module） | WS326 | WS318 は触らない |
| `h264sw-backend.c` の最初の実装、`decoder.c` の表 `{vkvideo, h264sw, aac}` と遅れた拒否の切り替え | WS326 p023 | WS318 p004 は p023 の merge の後に libavcodec の行を足す。WS326 の phase023 の「試験の項目」の「WS318 p002」は p004 の誤り（Q1 に直しを頼む） |
| decoder の ops の v2 と `h264sw-backend.c` の出力の直し（picture の pool → staging の CPU の frame） | WS318 p006a | WS326 の completed の後（header の凍結 `BASE=fd1d6f88c` が外れた後） |
| software の decoder の出力の複写の削減 | WS326 p024 は decoder の内部の速さ。出力の形は WS318 p006 | §15 の注意 1 |
| H.264 の software の encoder | WS329 | WS318 p012a の encoder の ops と表の位置。WS329 の依存に WS318 p012a を足す（Q1） |
| H.265 の software の decoder と encoder | WS330 | WS318 の `h265*.c` との共通化は WS330 p001 が決める |
| 共通の名前（bits・NAL・CABAC など） | WS326 p026 | WS318 の `h265*.c` は WS326 p026 の後なら共通の bit reader を使ってよい（p016 で決める） |
| camera の枠組みと uvc、bcm2711 の import、`cam.c` | WS282（保留中） | WS318 p025 が `cam.c` の import の条件を変える（WS282 の所有の file、Q1 が両方の WS に記録）。UVC の実機の動作は WS282 の再開が前提（Q318-6） |
| libbrowser の `page/media.c` | WS074 | WS318 p008・p010 が engine の口の変更に合わせて直す（Q1 が WS074 と調整） |
| 個別の driver の Vulkan Video | WS319〜WS325 | vkfake の試験の項目と D11 の別名の約束（driver が DISTINCT を出さない間は、COINCIDE の decode の image の別名の layout の遷移で内容を保つ）を driver の WS の受け入れに渡す（§13.6） |
| AAC-LC の encoder の低い層（A2DP） | WS205 | 窓の切り替え・TNS を足しても `media_aac_*` の API と既定の動きは変えない |

## 12. 移行の手順と app の書き換えの範囲

順に、各段で tree が build でき今の app が動く形を保つ。

1. **p004（入り口を揃える、今の機能のまま）**: libavcodec の decode の add-in を libmedia の `avcodec.c` へ（`pthread_once`、`app_*` → `media_*`、`bitstream.c` は libmedia の物）、`decoder.c` の表の最後に。旧い `media_decoder_open` は libavcodec を使わない（browser の engine と media-probe の今の動きを保つ。Q318-11）。Video Player・Music は一時の内部の口 `media_decoder_open_allow(track, MEDIA_ALLOW_ANY, &dec)`（p010 で消す）で libavcodec まで使う。Makefile から `media-app/*.c` を外し、`media-app/` の 6 file は Q1 に消す依頼。`run-host-codec.sh` を追従（`thread-posix.o` を外す、Q1 の許可）。
2. **p005〜p008（新しい口を足す、旧い口は shim）**: context・frame・変換・thread を足す。旧い `media_decoder_open` は process の暗黙の context（PRIVATE、`media_decoder_gpu_pictures` の値）の上の shim、`media_frame_gpu` の fd の export は shim の中だけ。`media_engine_open` と `wake_fd` は shim（内部の pipe を notify で書く）。p006a は ops と frame を変えるので、同じ Phase で media-probe（`media_frame_planes` で hash）と `plan/tools/media/host-codec.c` の偽の ops（Q1 の許可）を追従する。
3. **p009 Video Player**: `present.c` の instance・device の作成に requirements を足し（api 1.0）、queue の lock を present・`vkQueueWaitIdle` にも使い `vkDeviceWaitIdle` をやめ、`media_context_create(SHARED)`、`media_decoder_create(... MEDIA_MEMORY_VULKAN, {NV12, BGRA8})`、`media_frame_vulkan` の plane ごとに view（fd の import と `VP_GPU_PRESENT` の分けを外す）。shader は NV12 の 2 枚の plane を今と同じに sample する。
4. **p010 残りと旧い口の削除**: Music（NONE の context、音だけ）、libbrowser の engine（明示に PRIVATE の context、`media_engine_create` と notify）。旧い口と一時の口、`struct media_gpu_picture`、`media-decoder.h` を消し、`exports.map` を名前の列に。`plan/tools/rtx-video/` の media-probe の使い方の追従（Q1 の許可）。media-probe は `allow = HARDWARE|SOFTWARE` で native だけを見る。
5. app の書き換えの範囲はこの 4 つ。Photos・Phone・camvk（kernel の試験の道具なので `/dev/camN` を直に使い続ける）は変えない。

## 13. 試験の道

### 13.1 host の単体試験

- `plan/ws318/tests/run-unit.sh NAME...|all`（WS326 と同じ形: host の cc、`-std=c89`、ASan・UBSan、最後の行 `run-unit: PASS`）と `check-skeleton.sh`（header の基点の照合、cc と clang で warning 0）。p002 で作る。
- 対象: frame の参照の数と pool と identity、形式の交渉（表の全組）、codec の問い合わせの順と指定、問題の値の選び（§4.9）、切り替え（偽の backend の表）、encoder の選び（CONFIG）、packet の枠、色の係数、camera の層（偽の source の ops: 連続の frame・落とし・抜け・import の EXDEV・stop と start・close の後の unref）、thread の台帳、H.265 の各 module、AAC の窓の切り替え・TNS。

### 13.2 偽の Vulkan Video（vkfake、p003a・p003b）

- 代わりの `libvulkan.so.1`（D21）。本物の loader を絶対 path で開き、`VK_DRIVER_FILES` で lavapipe だけに固定（host の他の ICD を使わない）。api は 1.0 に、device の拡張は zedBSD の libvulkan の許可の表に絞る。ASan では llvmpipe の leak を抑える suppression を使う。
- 差し込む物: video の queue family（設定で「別の family」か「graphics と同じ 1 つの family」）、video の caps と形式の問い合わせ、session・parameter、`vkCmdDecodeVideoKHR`・`vkCmdEncodeVideoKHR`・encode の feedback の query・`vkGetEncodedVideoSessionParametersKHR`。video の queue への submit は lavapipe の queue への submit に置き換え（wait・signal・fence を保つ）、decode は「bitstream の hash から決まる模様」を DST に copy で書き、encode は SRC を読んだ hash を bitstream の buffer に書く。
- 記録: session・parameter の Std の構造体の値、DPB の slot、参照の list、layout の遷移を log にし、試験が期待と比べる。
- caps の形: i915 の形（NV12・OPTIMAL・SAMPLED 無し・COINCIDE・graphics と video の 2 family）、Pi 4 の形（1 family・compute 無し・SAMPLED あり・COINCIDE）、RTX の形（1 family）、DISTINCT の形、encode の有り無しと rate control の種類。
- 限り: arm64 の compile の時の分け（`VKVIDEO_FLIGHTS` など）は host の amd64 では通らない。p006a で runtime の値（device の性質から決める）に変えるか、arm64 の実機（Pi 4 の非退行）で確かめる。
- 変換の pass は lavapipe で実に描くので、画素を試験の中の独立の CPU の式と比べられる（許容 ±1）。

### 13.3 host の codec の試験

- `run-host-codec.sh`（p004 で追従）: libmedia の中の libavcodec の decode（host の FFmpeg 7、libavcodec 61）を今の試料で。
- p012b: host の libavcodec 61 で encode（libx264・libx265・aac）→ libmedia の decode で往復、PSNR と frame 数。`host-layout.c` を encode で使う field と option の名前の照合に広げる（版 61 と 63）。AAC は独自の encoder → 独自の decoder の SNR。
- H.265 の解析: ITU-T H.265.1 の適合の bitstream（Main の範囲）を WS326 と同じ形（パラメータの無い GET・SHA-256・cache は main の `build/sources/h265-conformance/`）で取り、解析で誤り無く通し、Std の値を host の `ffmpeg -bsf:v trace_headers` の値（副の oracle）と比べる。取る数と大きさは p016 で見積もり、disk の空き（2026-10-31 に 13 GB）を見て Q1 と決める。

### 13.4 QEMU（Venus、amd64）

- Video Player: Venus は Vulkan Video が無いので h264sw（WS326）→ 共有の device の plane の image へ upload の道。`plan/tools/rtx-video/venus-videoplayer.sh`（WS243、image は `config-amd64-venus-video.mk`）を新しい道に合わせて使う（判定は今の script の方法、console の log では判定しない）。
- 起動の確かめは `boot-test.sh`。Music の再生は `tests/scenarios/` の Music の物があれば使う（p010）。

### 13.5 実機

- **5330（i915、基準）**: Video Player の H.264 1080p30・1080p60 を Vulkan Video の decode → transfer の copy → app の sample（readback 無し）で。decode の結果の hash を software と比べ（`nv12-hashes.py` の形）、表示の数と late を見る（WS243 の計り方）。p026。
- **RTX 2070・Pi 4**: D11 の別名の道の非退行（今の GPU の道と同じ数）。Pi 4 は arm64 の flight の重ねの readback（browser の engine、PRIVATE）も。Q1 の割り当て。
- **camera**: 5330 ＋ webcam（WS282 の実機の動作の後、Q318-6）。

### 13.6 driver の WS に渡す試験

vkfake で確かめた項目（session の作成、parameter、GOP、feedback、H.265 の Std の値）と D11 の別名の約束を、WS319 以降の driver の WS の受け入れ（実の device で同じ試料を encode・decode し、hash・PSNR を見る）に渡す。この WS の hardware の encode と H.265 の decode は、実機の確かめが driver の WS の最初の受け入れになる（Q318-1）。

## 14. Phase の表（案）

型: O = Opus（phase-runner、kernel は phase-runner-high、app は phase-runner-mid）、S = Sonnet 5.5 high（phase-runner-low、標準廉価実装手順）、D = document-writer・conformance-reviewer。1 回の起動で済む大きさに分けた（review 19）。

| Phase | 目的 | 型 | 依存 | 受け入れ |
| --- | --- | --- | --- | --- |
| p001 | 設計（この文書、proposed の header、問い、design-reviewer） | O | — | Q1 の受け入れ、Q318 の回答 |
| p002 | 公開の header（proposed → `media.h` に足す・`media-vulkan.h`）、内部の header（ops v2 は新しい名前で並べる）、新しい file の骨格（ENOSYS）、Makefile・exports の名前の列、`run-unit.sh`・`check-skeleton.sh`、`colour.c`・`packet-format.c` の header と仕様の comment | O | p001 | amd64 build warning 0、`check-skeleton: PASS`、run-host-codec が変わらず PASS |
| p003a | vkfake の核: 代わりの libvulkan、lavapipe への転送、1.0 と許可の表、video の queue family と caps・形式の問い合わせ（4 つの形） | O | p002 | 自己試験: 4 つの形で caps と family が設定どおり、lavapipe の描画が通る |
| p003b | vkfake の video: session・parameter・decode の模倣と記録。今の `vkvideo-runtime.c`（PRIVATE、dlopen）を vkfake で H.264 の decode させる | O | p003a | 記録の Std の値が試料の手の計算と合う、ASan・UBSan 0 |
| p004 | libavcodec の decode を libmedia へ、Video Player・Music を libmedia だけに、media-app を外す（今の機能のまま、旧い口は libavcodec 無し） | O | WS326 p023（main） | run-host-codec PASS、amd64 build warning 0、QEMU Venus の Video Player で h264sw と libavcodec の両方、Video Player・Music の MISSING の文言 |
| p005 | context と device の共有の核（NONE・PRIVATE・SHARED、requirements、queue と lock、PRIVATE の device を context で 1 つに、旧い口の shim） | O | p003b、p004 | 単体試験、vkfake の 4 つの形で SHARED・PRIVATE の H.264 の decode、旧い口の app が変わらず動く（QEMU） |
| p006a | frame の model と ops v2（参照の数・種類・identity、h264sw・aac・avcodec の backend の追従、staging の CPU の frame、media-probe と host-codec の追従） | O | p005、WS326 completed | 単体試験、run-host-codec、media-probe の hash が前と同じ（QEMU か host） |
| p006b | hardware の decode の VULKAN の出力（D11 の (1)〜(3)、i915 の transfer の道、別名、export を内部へ） | O | p006a | vkfake の i915・Pi 4・RTX・DISTINCT の形で plane の frame、layout の記録が正しい |
| p007a | 変換の pass の核（render pass・pipeline・shader の生成、yuv420-to-rgb、upload の後の pass、交渉の表） | O | p006b | lavapipe で全組の画素を CPU の式と照合（±1）、spirv-val |
| p007b | 残りの変換（packed422、rgb-to-yuv420、readback）と色の係数（`colour.c` は S でもよい） | O（S） | p007a | 同上 |
| p008 | thread の台帳と notify、engine の callback（shim で wake_fd を残す）、libbrowser の engine を notify と PRIVATE に | O | p005 | 単体試験、browser の `<video>` の既存の試験（QEMU） |
| p009 | Video Player を新しい口へ（SHARED、plane の frame、queue の lock、fd の import を外す） | O（mid） | p007a、p008 | QEMU Venus の Video Player、5330 の短い再生 |
| p010 | Music・media-probe・libbrowser を新しい口へ、旧い口と一時の口・`media-decoder.h` を消す、exports の名前の列 | O（mid） | p009 | build warning 0、run-host-codec、QEMU の Video Player・Music、`nm -D` で旧い symbol が無い |
| p011a | Vulkan の header に H.265 の decode を選び直す（版は 1.3.269 のまま、入力の照合から）、sysroot は Q1 | O ＋ Q1 | Q318-3 | 全 arch の libvulkan・libmedia・GPU の driver の build warning 0 |
| p011b | Vulkan の header を KHR の encode のある固定の tag へ、sysroot は Q1 | O ＋ Q1 | Q318-3 | 同上、H.264 の decode の回帰（vkfake） |
| p012a | encoder の核（`encoder.c`・`codec-query.c`）、packet の枠（`packet-format.c`、S でもよい）、AAC-LC の backend | O | p007b | 単体試験、AAC の往復の SNR、問い合わせの表の試験 |
| p012b | libavcodec の encode の add-in（`av_opt_set_*`、`host-layout.c` の追従） | O | p012a | host の libavcodec 61 で H.264・H.265・AAC の往復（§13.3） |
| p013 | AAC の追加の設計（標準廉価: 窓の切り替えの判定・短い窓の grouping・TNS の次数と on/off を informative な出典で数値まで固定した header、試験の道具、phase.md） | O | p012a、Q318-5 | design-reviewer、骨格の build |
| p014 | AAC: 窓の判定・窓の列・短い窓の grouping と scalefactor | S | p013 | run-unit PASS、往復の SNR、突然変異の確かめ |
| p015 | AAC: TNS の解析・量子化・符号化 | S | p014 | 同上 |
| p016 | H.265 の Vulkan Video の decode の設計（標準廉価: module・header・骨格・適合の取得・phase.md） | O | p011a | design-reviewer、骨格の build、適合の取得の script |
| p017 | H.265: NAL と VPS・SPS（VUI の要る部分）→ StdVideoH265 | S | p016 | run-unit、適合の stream の SPS の値 |
| p018 | H.265: PPS と slice segment header | S | p017 | 同上 |
| p019 | H.265: POC・RPS・DPB の marking と出力の順 | S | p018 | run-unit、手の計算の例 |
| p020 | H.265 の decode の runtime（vkvideo の H.265 の道、vkfake の H.265 の模倣を足す、適合の全 stream の解析） | O | p006b、p019 | vkfake の記録が trace と合う、解析の誤り 0 |
| p021 | Vulkan Video の encode（H.264: session・SPS/PPS・GOP の IDR と P・rate control・feedback・入力の image、vkfake の encode の模倣を足す） | O | p011b、p012a | vkfake の記録（GOP・parameter・rate control の落とし）、packet の枠 |
| p022 | Vulkan Video の encode（H.265） | O | p021、p017 | 同上 |
| p023a | camera の層（slot の状態 §7.4、待ちの列、notify、mode の選び）と偽の source での単体試験 | O | p007b、p012a | 単体試験（連続・落とし・抜け・stop と start・close の後の unref） |
| p023b | zedBSD の source（列挙・DEVICE の mmap と staging・IMPORT と export・EXDEV の戻り）、OS の境界の checker の許可の行（Q1） | O | p023a | amd64・rpi4・rpi5 の libmedia の build warning 0、`keiland-os-boundary/check.sh` PASS |
| p024 | MJPEG の段（Q318-4 の答えによる） | O（S） | p023a、Q318-4 | 単体試験（T.81 の例と往復）、偽の source |
| p025 | kernel: camera の import を i915 の coherent な memory へ（§10.4） | O（high） | p023b | amd64 の vmunix warning 0、WS282 の host 試験（cam）PASS、i915 の export の allocation の照合（host 試験か 5330 の小さな試験の program） |
| p026 | 実機の受け入れ: 5330 の Video Player（i915 の transfer の道、1080p30・60、hash）、RTX・Pi 4 の非退行、QEMU Venus | O | p010、p020 | §13.5 |
| p027 | camera の実機（5330 ＋ webcam、表示と frame の流れと import） | O | p023b〜p025、WS282 の実機（Q318-6） | 連続の frame、落とし、抜けと差し直し |
| p028 | 解説の文書（`docs/reference/` の libmedia）と規約の全文の見直し | D | 全部 | 規約の checklist、文書の例と照合 |

- WS326 の完了を待たずに始められるのは p002・p003a・p003b・p011a・p011b・p016〜p019（依存の鎖が WS326 に届かない物）。始めるかはユーザーの日程の決定（WS326 の後にすぐ）に関わるので Q318-14 で聞く。
- 標準廉価実装手順の Phase（p014・p015・p017〜p019、S を付けた p007b・p012a の一部）は設計の Phase（p002・p013・p016）が header の基点を決め、`BASE=` の照合を phase.md に書く。

## 15. Q1 への注意（ユーザーの判断ではない物）

1. WS326 p024（速さ）は decoder の内部の速さに限り、出力の複写の削減は WS318 p006a の staging の形の後に回すのを推す。
2. plan/tools の追従（`run-host-codec.sh`・`host-codec.c`・`host-layout.c`・`rtx-video/`、`keiland-os-boundary/app-allow.tsv`）は Phase の範囲に入れるか Q1 が直す。
3. `media-app/` の 6 file（p004）と `media-decoder.h`（p010）の削除は Q1 の pipeline。
4. host に Vulkan の validation layer（Debian の `vulkan-validationlayers`）を入れると vkfake の試験が使い方の誤りも拾える（数十 MB）。
5. p011a・p011b の sysroot の作り直しは Q1。
6. 他の WS の記録の直し: WS329 の依存に WS318 p012a、WS326 phase023 の「WS318 p002」→ p004、WS282 と WS074 に WS318 の変更の所在。
7. 実機の割り当て: p009（5330 の短い確かめ）、p026（5330・RTX・Pi 4）、p027（5330 ＋ webcam）。
8. ws.md の「ユーザーの決定（原文）」は Q1 が書いた記録なので、この Phase では変えていない（AGENTS §8 の原文を載せない規則との整理は Q1）。

## 16. design-reviewer の結果と反映

2026-10-31 に design-reviewer を 1 回（37 件: blocker 3・major 20・minor 14）。全件と反映は [design-review.md](design-review.md)。主な変更: i915 の NV12 を transfer だけの道と plane ごとの image で（D7・D9・§6.2）、変換を compute から描画の pass へ（D10、Pi 4 に compute が無い）、queue の lock をどの役にも（D5）、DPB の参照の image を渡す道を別名・DISTINCT・copy の 3 つに（D11）、api 1.0（D4）、vkfake を代わりの libvulkan に（D21）、camera の slot の状態（§7.4）、誤りの写し（§9）、Phase の分けと依存の直し（§14）、問いを 14 に。
