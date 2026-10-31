# zedBSD libvulkan

Vulkan 1.0 の137 core commandと、Vulkan 1.0に適用される
`VK_KHR_surface`、`VK_KHR_display`、`VK_KHR_swapchain`、
`VK_KHR_display_swapchain` の18 commandを提供する共有ライブラリです。
公開ヘッダは `include/libc/vulkan/`、インストール先は
`/lib/libvulkan.so`、SONAMEは `libvulkan.so` です。

アプリは標準Vulkan APIを使います。zedBSDのGPU ioctl、resource ID、
Venus wireをアプリへ公開しません。`userland/tests/vkdemo/` が標準APIを
使う実例で、別OSのVulkan実装でもビルドできます。

現在のbackendはamd64 zedBSD上のVenusです。QEMUのvirtio-vga-gl、
virglrenderer 1.1.0とVulkan 1.1以上のnative deviceを使い、
公開するcore versionを1.0に制限しています。元のGPU能力から
実装で保持できる能力を選択し、deviceごとに拡張の対応を列挙します。
初期受け入れ環境はLinux Intel i915/ANVです。

## 構成と所有権

- `instance.c` / `device.c` は複数GPU、物理能力、logical deviceとqueueを管理します。
- `objects.c` / `wire.c` / `context.c` は動的handle、allocator callback、
  private protocol IDと返信、共有command streamを管理します。
- API familyごとのCファイルが標準入力を独立したwire表現へ変換します。
  shaderはアプリが渡したSPIR-Vをnative driverへ送ります。
- `memory.c` はGPU共有資源を `mmap` します。公開HOST_VISIBLE typeは
  HOST_COHERENTだけで、CPU copyをcoherent mappingの代用にしません。
- `wsi*.c` はdirect-display surface / swapchainとFIFO順序を実装します。
  GPU完了を待って表示側所有のimageへ転送し、表示中imageをアプリへ返しません。
- Kは普通のGPU session/resource/map/submit/displayインタフェースを使い、
  権限、範囲、fileとmappingの寿命を独立して検査します。

後続core、外部memory/sync拡張、Wayland WSI、EGLは公開しません。
Wayland対応時は `VK_KHR_wayland_surface` のWSI backendを追加します。
EGLは別途GLES-on-Vulkanを扱う時の課題です。

## ビルドと検証

通常のbase package設定で `libvulkan` を選択します。
amd64用vkdemo検証設定ではlibvulkanと動的アプリをまとめてビルドできます。
公開headerはsysrootにも `vulkan/` の階層を保持します。

```sh
make -j16 BUILD="$PWD/build/vkdemo-amd64" \
  ZEDBSD_CONFIG="$PWD/plan/ws014/tests/config-vkdemo-amd64.mk" \
  "$PWD/build/vkdemo-amd64/bin/vkdemo"
```

ELF checkerは155 exports、SONAME、依存ライブラリとinterpreterを照合します。
[全API検証台帳](../../../plan/ws030/phase004/api-verification.md) は
各commandを実装と独立試験に対応させています。
`plan/ws030/tests/` の限定試験は実objects/codec/family、実K/VM/PCI/HALの
対象境界を通常実行とASan/UBSanで確認します。
`plan/ws014/tests/run-vkdemo-remote.py` は指定したprivate QEMUホストで
標準アプリの実画面とGPU readbackを照合します。イメージ転送を伴います。

この実装・内部試験は、Khronos CTS合格や正式な適合認証を意味しません。
全featureの全組合せや全limitの境界を実GPUで実測したとは主張しません。
256MiBのhost-visible apertureは有限で、容量不足を標準allocation errorとして
返します。native返信の有限watchdogもあり、非常に長いcompile等では
contextをdevice-lostとして終了する場合があります。

## Vulkan Video（H.264 decode）と synchronization2（ws083）

`VK_KHR_video_queue`・`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264` と
`VK_KHR_synchronization2` は、capset の native の語（176 byte の record の
byte 168 の tag と byte 172 の bit 0）で H.264 decode を約束した backend
（native の i915）の時だけ列挙します。Venus（fork・stock）では出さず、
backend の queue family の video の flag と format の video の feature も隠します。
`video.c` は profile と parameter set の key を手元で検べ、session・parameters・
記録を zedBSD 独自の opcode（`uapi/gpu-op.h` の `0x10000`〜`0x1000d`）で送ります。
`sync2.c` は synchronization2 の 6 command を 1.0 の command に翻訳します
（1.0 に無い stage は ALL_COMMANDS、access は MEMORY_READ|MEMORY_WRITE に広げる）。

規格に合わない点（ユーザーの判断 H2・HD6、2026-10-07）:

- N1: device の `apiVersion` は 1.0 のままで、Vulkan 1.1（と synchronization2）を
  前提とする video の拡張を名乗ります。
- N2: `VK_KHR_sampler_ycbcr_conversion` を名乗らずに NV12
  （`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`）と `PLANE_0/1` の aspect を使います。
- N3: OPTIMAL（decoder の Tile Y）の decode の絵に `vkGetImageSubresourceLayout`
  が plane の offset・pitch を答えます。zedBSD の私的な約束で、試験の app が
  de-tile に使います（decode の絵の SAMPLED・TRANSFER_SRC が入れば不要）。
- N4: slice が 256 を超える picture は decode せずに飛ばします（出力は未定義）。

parameter set の key の重複と H.264 の範囲の外の id は `VK_ERROR_INITIALIZATION_FAILED`
で拒みます（固定した 1.3.269 の header では `VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR`
が beta の encode の値のため）。result status query・inline query は持ちません。

## Display の制御と抜き差しの通知（ws113-p003）

instance の `VK_EXT_display_surface_counter` と device の `VK_EXT_display_control`（4 command）を
提供します。device の拡張は、renderer の node が display・topology の事象・display の制御
（`GPU_CAP_DISPLAY`・`GPU_CAP_DISPLAY_EVENTS`・`GPU_CAP_DISPLAY_CONTROL`）を持つ時だけ列挙します
（display が別の display 専用 node にある構成では出しません）。

- `vkRegisterDeviceEventEXT`（DISPLAY_HOTPLUG）: fence は登録時の topology の sequence
  （この renderer に属す display node の `GPU_DISPLAY_EVENTS` の値の和、非破壊の QUERY で ACK しない）を
  cursor に持ち、後の観測で和が cursor を超えたら signal します。thread は持たず、
  `vkGetFenceStatus`・`vkWaitForFences` の観測ごとに kernel に一度だけ待たずに聞きます
  （wait は 1 ms ごとの観測）。fence ごとの cursor なので、ある client の観測が別の fence を消費しません。
  client は signal の後に新しい fence を登録してから display を列挙し直し、古い fence を壊します。
- `vkRegisterDisplayEventEXT`（FIRST_PIXEL_OUT）: 登録時の `GPU_DISPLAY_REFRESH` の数を cursor にし、
  その後の refresh の境界で signal します。scanout していない出力は境界を作りません。generation が
  変わった（抜いて挿した）display は新しい cursor を取り直し、次の実の境界で signal します（挿したこと
  自体では signal しません）。
- signal した event の fence を reset すると、その event は再び signal しません（過去の event を再生しない）。
  未発火の fence の reset は監視を取り消しません。
- `vkDisplayPowerControlEXT`: この device が swapchain を持つ display の lease で `GPU_DISPLAY_POWER` を送ります。
  lease が無い・出力に power の制御が無い（i915 の HDMI など）・切断の時は `VK_ERROR_UNKNOWN`
  （規格がこの command に挙げる失敗は OUT_OF_HOST_MEMORY だけ）。lease の終わりで ON に戻ります。
- surface の counter は持ちません（`supportedSurfaceCounters = 0`）。`vkGetSwapchainCounterEXT` は
  `VK_ERROR_OUT_OF_DATE_KHR`、counter を求める swapchain の作成は `VK_ERROR_INITIALIZATION_FAILED`。
- 同時に出せる出力の数の制限（native の claim の `ENOSPC`）は、swapchain の作成で
  `VK_ERROR_INITIALIZATION_FAILED`（device を失わない、一時的）になります。

host の試験は `plan/ws113/tests/host-display-events.sh`、独立の client は `userland/tests/display-events/`。

## Overlay の plane（ws214-p004）

`GPU_DISPLAY_HAS_OVERLAYS` の出力では、plane を primary（0）、overlay（1〜K）、cursor（最後）の順に列挙します（stack の順も同じ）。
overlay の plane は `VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR` で、`vkGetDisplayPlaneCapabilitiesKHR` の src・dst の範囲は
kernel の `GPU_DISPLAY_OVERLAY_INFO`、置き場所は出力の中だけ（minDstPosition は (0,0)）です。overlay の surface は primary の claim を借り
（primary の swapchain が先に要る）、swapchain の `minImageCount` は 1（2 枚を同時に acquire できる）。画像は共有の画像だけで、copy の経路は
ありません。位置と拡大縮小は `VkDisplayPresentInfoKHR` の `srcRect`（画像の中）と `dstRect`（出力の中）で、矩形の無い present は前の矩形のまま
です。

原子性の規則（実装の定めで、仕様の外。仕様は禁じない）:

1. 1 回の `vkQueuePresentKHR` には 1 つの plane の swapchain を渡します。複数を渡された時は chain ごとに順に扱い、揃えることは約束しません。
2. **切り替えの保留**: overlay の present のうち、見える・隠れるの切り替え（隠れた plane を出す、出ている plane を隠す）は kernel に出さず
   display ごとに保留します。保留は「保留した時までに呼ばれた primary の present の数＋1」（`min_primary`）を持ち、primary の present は
   呼ばれた順の ticket を持ちます。ticket が `min_primary` 以上の primary の present が、保留を全部取り込んで 1 回の
   `GPU_DISPLAY_COMMIT` にします。保留より前に呼ばれた primary（GPU を待っている前のコマ）は取り込みません。
3. 保留がある plane への present は、切り替えかどうかに関わらず保留を置き換えます（`min_primary` と期限は最初の物のまま）。置き換えられた
   画像はすぐ acquire できます。
4. 保留の無い plane の切り替えでない present は単独で commit します。中身の変わらない画像（usage が TRANSFER だけの chain で、前の copy
   から書かれていない）で待ちが済んでいれば呼んだ thread で（fast path）、そうでなければ overlay の lane の worker が GPU の copy を待って。
5. よって compositor は「切り替えの overlay の present → primary の present」の順に、間に描画を挟まずに呼びます。逆だと切り替えは次の
   primary まで遅れます。
6. **primary を伴わない flush**: 保留の後 2 refresh の間に `min_primary` 以上の primary の present が呼ばれなければ、保留は単独で commit
   されます（log `why=expired`）。`vkQueueWaitIdle`・`vkDeviceWaitIdle` は保留を単独で commit して採用を待ってから返ります（`why=flush`）。
   退役した chain（oldSwapchain・破棄）の保留は捨て、`vkDestroySwapchainKHR` は出ている plane を隠す commit を出して採用を待ちます
   （`why=destroy`）。単独の flush は primary と同じ refresh を約束しません。

**隠す約束**: overlay の present で `srcRect` と `dstRect` がともに offset (0,0)・extent 1×1 なら、その plane を隠します（画像は読まれず、
swapchain は壊れません）。1×1 の overlay を本当に出す用途は無いものとします。

失敗: 保留を含む commit を kernel が ENOSPC・EINVAL で拒むと、取り込んだ保留を全部捨て、primary も出しません（前の画面を保つ）。primary の
present は成功のままで、保留の chain は次の present・acquire で `VK_ERROR_OUT_OF_DATE_KHR` を返します。保留の無い commit の
ENOSPC・EINVAL はその chain だけ OUT_OF_DATE です。overlay の画像は、それを外す記録の採用までは kernel が読むので acquire できず、
acquire はその採用を `GPU_DISPLAY_WAIT` で display の lock を持たずに待ちます。

log: `VULKAN_ZEDBSD_OVERLAY_LOG=1` で overlay の present・commit ごとに 1 行
`WSI OVERLAY present plane= serial= ticket= min_primary= path=held|replaced|fast|lane|commit|flush why= result= sequence= adopted= call_ns=`
を標準エラーに書きます（`why` は cursor の log と同じ語に nospace・invalid・expired・flush・destroy・busy を足した物）。
host の試験は `plan/ws214/tests/vulkan-overlay-host.sh`。

## Compositor が貸す image（game mode）

compositor の game mode では、client はその display の swapchain の image に直に描きます（present の copy をしません）。

- **出せる display の chain**: `VkSwapchainCreateInfoKHR` の pNext に `VkExternalMemoryImageCreateInfo`（`handleTypes` は
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT` だけ）を繋ぐと、direct の chain（display がそのまま見せる linear の image に app が描く chain）の
  image を外へ出せます。`vkGetMemoryFdKHR` に `memory = VK_NULL_HANDLE` と、pNext の `VkMemoryDedicatedAllocateInfo`（`image` に swapchain の image、
  `buffer` は null）を渡すと、その image の fd（image の記述を持つ kernel の capability）を返します。direct でない chain の image は
  `VK_ERROR_INVALID_EXTERNAL_HANDLE` です。
- **非適合点（N5）**: Vulkan には swapchain の image の export がありません。上の組み合わせ（swapchain の作成の external memory の record、memory の無い
  `vkGetMemoryFdKHR` と dedicated の record で image を名指す事）は標準の型だけを使う、この実装の決まりです。返る fd はこの実装の import
  （`VkImportMemoryFdInfoKHR` と dedicated の image の `vkAllocateMemory`）と Wayland の WSI が受けます。
- **Wayland の WSI**: compositor が `kl_game_v1` を出していれば、surface ごとに `kl_game_surface_v1` を作り、swapchain を作るたびにその chain
  （format・usage・extent・枚数、貸された image に描けない chain は枚数 0）を知らせます。compositor が image を貸し（`image`・`lend`）、それが今の chain に
  合えば（同じ channel の並びの format、同じ extent、usage を含む、枚数が足りる、linear）、present は `VK_SUBOPTIMAL_KHR` を返します。作り直した chain の
  image は貸された image（app の format のまま。UNORM の image を SRGB で読む場合も同じ byte）で、present は copy せず、描画の完了の fence とともに
  その buffer を commit します。合わない chain は貸し出しを断ります（`decline`）。compositor が貸し出しを終えると（`withdraw`）、貸された chain は普通の
  buffer として合成され、`VK_SUBOPTIMAL_KHR` で自分の image の chain へ戻ります。
- display の chain の direct の image の作成が `VK_ERROR_FEATURE_NOT_PRESENT`・`VK_ERROR_FORMAT_NOT_SUPPORTED` で失敗した時は、自分の image を持ち present で
  copy する経路へ戻ります。export の ioctl の `EOPNOTSUPP`・`ENOTSUP`・`EINVAL` は `VK_ERROR_FEATURE_NOT_PRESENT` です。

## 宣言とprotocolの来歴

[API-PROVENANCE.md](../../../include/libc/vulkan/API-PROVENANCE.md) に
固定した公式宣言の版・hash・ライセンスとNoct再生成手順を記録しています。
実装は独立して記述し、Mesa、loader、virglrendererのC実装を移入していません。
公開APIの宣言のライセンス表示は `include/libc/vulkan/LICENSE-API` にあります。
command の番号と wire の形は Venus protocol の値・仕様を引用した事実で、引用元は
[gpu-op.h](../../../include/uapi/gpu-op.h) と `wire.c` の頭の注記にあります（第三者のライセンス表示は置いていません、ws167-p002・ws219-p001）。
