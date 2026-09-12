# zedBSD libvulkan

Vulkan 1.0 の137 core commandと、Vulkan 1.0に適用される
`VK_KHR_surface`、`VK_KHR_display`、`VK_KHR_swapchain`、
`VK_KHR_display_swapchain` の18 commandを提供する共有ライブラリです。
公開ヘッダは `libc/include/vulkan/`、インストール先は
`/lib/libvulkan.so`、SONAMEは `libvulkan.so` です。

アプリは標準Vulkan APIを使います。zedBSDのGPU ioctl、resource ID、
Venus wireをアプリへ公開しません。`userland/base/vkdemo/` が標準APIを
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

## 宣言とprotocolの来歴

[API-PROVENANCE.md](../../../libc/include/vulkan/API-PROVENANCE.md) に
固定した公式宣言の版・hash・ライセンスとNoct再生成手順を記録しています。
実装は独立して記述し、Mesa、loader、virglrendererのC実装を移入していません。
公開APIとprotocol IDの必要なライセンス表示はそれぞれの資料を参照してください。
