<!-- awesome-plan project=zedbsd record=ws141-license-audit -->
# Raspberry Pi 4 の GPU の参照の正本と license の監査（ws141-p001）

作成: 2026-10-04、P2（q691）。この文書は GPL の code・comment・定数の名前を含まない（path・hash・license の判定だけ）。

## 正本

| 正本 | 版 | 取得元 | 用途 |
| --- | --- | --- | --- |
| Linux | tag `v6.19`（commit `05f7e89ab9731565d8a62e3b5d1ec206485eeb0b`） | `https://github.com/torvalds/linux`（sparse の clone、`plan/ws141/temp/linux/`、commit しない） | vc4（display）・v3d（3D）の手順の理解。作業の文書は `plan/ws141/temp/` |
| Mesa | tag `mesa-25.3.6`（commit `06f9e28304d5d3f109c33535c1c25b9df5769af2`） | `https://gitlab.freedesktop.org/mesa/mesa.git`（`src/broadcom`・`include/drm-uapi`、`plan/ws141/temp/mesa/`） | V3D の control list の packet の形（`cle/v3d_packet.xml`）、tiling、device の情報 |
| BCM2711 ARM Peripherals | PDF、SHA-256 `d6e16ea089a1716e80621f99a26e130a2379f0a81d945dc22e4f80dc49dcdc78` | `https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf` | VideoCore の割り込みの番号（HVS・V3D・pixelvalve 0〜4・HDMI 0/1）、I2C（BSC）。HVS・pixelvalve・HDMI・V3D の register の表は**載っていない** |

BCM2711 ARM Peripherals の条件: 「Raspberry Pi の製品と一緒に使う時だけ使ってよい」（Legal Disclaimer Notice）。zedBSD の Raspberry Pi 4 の driver はこの範囲。

正本には上の 3 つに加え、Raspberry Pi の firmware の wiki（mailbox の property の文書）と固定の firmware の DTB を使う（下の追加の表）。

判定の方法: 各 file の先頭の SPDX の行、または MIT の permission notice の機械判定（`plan/ws141/temp/lic.py`、commit しない）。表記の無い file は、その project の既定（Linux は COPYING の GPL-2.0、Mesa は `docs/license.rst` の MIT）として扱う。

## 判定の要点

1. **Linux の vc4・v3d の driver の本体は GPL**（GPL-2.0・GPL-2.0+・GPL-2.0-only）。register の定義の header（vc4 の display の register、HDMI の register、v3d の register）も GPL。読むだけにし、作業の文書は `plan/ws141/temp/` に置く（Guardrail の WS141 の節）。
2. **MIT の物**: Linux の uapi の `vc4_drm.h`・`v3d_drm.h`、vc4 の旧い 3D の部分（packet・validate・render CL・QPU の定義・fence・irq・gem）。v3d は `v3d_sysfs.c` だけが MIT。
3. **Mesa の `src/broadcom/cle/v3d_packet.xml`** は file に license の表記が無い。Mesa の既定（MIT）として扱う。V3D 4.2 の control list の packet の形の出典はこれにする。
4. **device tree の binding**: display（`brcm,bcm2711-hdmi`・`bcm2835-hvs`・`bcm2835-pixelvalve0` など）は **GPL-2.0 だけ**（dual ではない）。V3D の binding（`brcm,bcm-v3d.yaml`）は GPL-2.0-only OR BSD-2-Clause。compatible の文字列・reg-names・割り込みの数は DT との interface（事実）として使う。
5. **register の offset と bit の出典の問題**: V3D 4.2 の register の offset と bit を載せた MIT・BSD の資料は、Mesa にも Broadcom の公開の文書にも無い（Mesa の simulator は Broadcom の非公開の header から名前を取り、offset を持たない）。HVS・pixelvalve・HDMI も同じで、GPL の header にしか無い。WS141 の「ライセンスの扱い」5（register の定義は MIT の Mesa・Broadcom の公開の文書・DT の binding から）は**display と V3D の register については満たせない**。2026-10-04 ユーザーの決定「事実として使う」で、値はハードウェアの事実として使い、名前は一括で独自に改名し、配置・comment・構造は写さない（Guardrail の WS141 の節の改訂、[rpi4-gpu-design.md](rpi4-gpu-design.md) の判断の項目 1）。
6. **BLOB**: vc4・v3d の source を配列の初期化子で grep した（2026-10-04）。firmware に当たる byte の配列は無い。あるのは色の変換の係数・pixel の形式の一覧・HDMI の PHY の設定の表で、firmware ではない（定数として改名の対象）。HVS・V3D は ARM の側の firmware の load を要さない。VideoCore の firmware（`start4.elf`）は boot の partition の物で、driver は mailbox で話すだけ。

## file ごとの表

| path（Linux v6.19） | SHA-256 | license | 扱い |
| --- | --- | --- | --- |
| `Documentation/devicetree/bindings/display/brcm,bcm2711-hdmi.yaml` | `2e0091282ab3055054ee00b1b0dc0f521d7014d62181a371cf04a18f01213ad5` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/display/brcm,bcm2835-dpi.yaml` | `32b9bae0a7efc07f3b8d3316c130f02d46abf4b6bb017c2aefe473c55322982a` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/display/brcm,bcm2835-dsi0.yaml` | `566ab5bbce2dcb8c4130ec23bc27ccf583ad5dae226cfc9c23c5ef491eb0b93f` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/display/brcm,bcm2835-hdmi.yaml` | `f3c35b1c5e3d1b2d444282c65ae05163f91cc3eb23130af99a6828769aa3e1e7` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/display/brcm,bcm2835-hvs.yaml` | `f982c0ea04c59f09f4f9d76efff289e7d790348c532bb8c442a3e3b6ea6da27a` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/display/brcm,bcm2835-pixelvalve0.yaml` | `60e79d60c724a1fccd5fe08b1b0fcc98040a1e4a49c1dc75c24508b3bb897196` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/display/brcm,bcm2835-txp.yaml` | `3ace52c0a2bd4f151e5d55b2c3bddca17facd2351541be864256160d0d6d465e` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/display/brcm,bcm2835-v3d.yaml` | `373b1da571be89af216d77cff6caed222e4aef03f21d8f4977258f4e425293fe` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/display/brcm,bcm2835-vc4.yaml` | `3eb977aad8d9e75e5bb2b655f70a9a9c617c6de6a9f611859c7fd3f447846037` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/display/brcm,bcm2835-vec.yaml` | `6d33972e2231f47aff12dd759856caab5b18a2fab102ea46418830f034fdae20` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `Documentation/devicetree/bindings/gpu/brcm,bcm-v3d.yaml` | `4af921f3677b4e8a7af87dd92e388103cf6cd5ad1206865700bb13823dee1f1e` | (GPL-2.0-only OR BSD-2-Clause) | 定義（binding）の取り込みの候補（BSD-2-Clause を選ぶ） |
| `drivers/firmware/raspberrypi.c` | `3472506f4681db3e1802f4f9f587fd7fb71cf7f7370892dd4473174699159fc2` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/Kconfig` | `73352b7712f2d81f9523bb552075bf14a7843e99518a8260887553cf20b3d788` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/Makefile` | `4ed5762c56f8df42318cc2b76be03a7974a0e6f596e8f5748651218cb9d9391a` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_bo.c` | `fdc9e3ea8983ed37e142592dc856cafd0d30d60b71a47ec98919231b8b7793b5` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_debugfs.c` | `d4542e278a055c697c3274f6d12dc0a725be80489eacc12363748b8f728c7747` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_drv.c` | `dd7312fb769242f7cd4709184f958c5c7edf019de67bdec4c7660ffa1640db96` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_drv.h` | `ae9835f37e98b15751a3214d449fdf8332fc1a99c9013bc655d21628f72c6076` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_fence.c` | `4147abdf64f3ad32c0371928e0bf173f60a8863095dcf25507334855b26f53b2` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_gem.c` | `83a23f52de2d2011b7e1b71e394dd1baf69f73615b558fa38724d51a0998bfc2` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_gemfs.c` | `df0207f3a676e63655dc818bfdce419bc4d54e2e218eee254c6a963c9074fb21` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_irq.c` | `2bdf8ba31b1120f357ed702440c9f2bf69915d1e5e6b804b297f32f58960ed50` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_mmu.c` | `cad07c01f0a10414348dcdf65915916427ad4d974b065435b471e605f934cca1` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_perfmon.c` | `f8e133013fb996580e3d2526101f8145e7d2cad5b6571ebcfced614654c204cc` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_performance_counters.h` | `d1d94111433702ea3bff38b0c6735a59697e5118d69df6fa6204643b464b7237` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_regs.h` | `ab374d39a47af85b1bef41721f1b68019eb4531253e4b41cc5c94405df8037f9` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_sched.c` | `2afa8f9cfc1bee21c3e4e82ecd13790ba214a88368334c8aca7dd072dbb78e58` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_submit.c` | `aebfb6adae9f1aad5cab70b79c8efa64c20de7ca07db5ba18ad93d3ae2c2b468` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_sysfs.c` | `bf97a4567827b52a5fb18092d09f767d6955308113e1a94ce29368e24497b647` | MIT | 定義の取り込みの候補 |
| `drivers/gpu/drm/v3d/v3d_trace.h` | `debc27b8646819dab05a76f71e6b68508aeec56a17f41abe792a5e3ddf67cd9d` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/v3d/v3d_trace_points.c` | `e2f761de654b884db3d3cb6261a10ae700b48ea5b95acb44261bc19d807398c2` | GPL-2.0+ | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/Kconfig` | `4d95b97b5ba71458b06c0df46dc0f3cd75fe56944d6a7e1ea1315efacc33222a` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/Makefile` | `c66d060e9d07a53d35d5644dc2d9b5defcdcddb82b8e883d877e328581fca6e2` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/tests/.kunitconfig` | `383e51931733462afb96f8f0a715c0b9244bd6ca30be798ff7a8ef4ac9195ebc` | 表記なし（設定の file） | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/tests/vc4_mock.c` | `af93b9f06d3eba10235faf5d7979879c5269f4c8f000b6613d3c074bbc042f9d` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/tests/vc4_mock.h` | `37c8ee68915f1e048977a8772f5c1cdbf12e23d9fb62d20e51a292a9ae49b474` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/tests/vc4_mock_crtc.c` | `1d3aedfc8d7c549100d237494487d5f24b81e93ba54728c39fcf527d598acc42` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/tests/vc4_mock_output.c` | `60c1947ff501a4ed980b73fd86049db0a7febe154f3e4698fdd8d847b7c8d8f7` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/tests/vc4_mock_plane.c` | `ded1e418336cf7c5c90d346ba498d3793eab15d6e4522943eeb0be2e4a0634b9` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/tests/vc4_test_pv_muxing.c` | `c3954b8a99d932fb822fea0d5d629272b09279410128738645ae4e0eb9efd9ba` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_bo.c` | `a20c7bd4b202a327b6777fc0b98c45f31d3f32cfedac61598199f7e9a5ecc815` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_crtc.c` | `dc529da1453b9b6495f147b269a90c60ab817b8b161319381a85f336093286fa` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_debugfs.c` | `70858bacb64602bc8bd8bf193f7c033af116199c3cf1d857593f1be81d86893e` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_dpi.c` | `9d2d3ada1c071d505ef061b955c1f0ee3c6112448db9c5c979ccb12a7f58f5e0` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_drv.c` | `f44d8d1e511f0f674f2b720c8a398e23d9c396484ce2d9ba73d6709721e8d83d` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_drv.h` | `62084cc7d2c7342c2d5f1a42bffd1c31a25de89fae3c192b54169cc83c51565d` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_dsi.c` | `ced8c8641473774087fcffbe19c49360adf6e588640f1c8f3cbd4c2e4f79420a` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_fence.c` | `1cc83896fa4785d2fd2c560208bf524f1123961a868a70539306694a3f89a43a` | MIT | 定義の取り込みの候補 |
| `drivers/gpu/drm/vc4/vc4_gem.c` | `e98b8cac4a34a3fa7a36c096639e641ccfe68af9e8fa957013f99cebe465ceca` | MIT | 定義の取り込みの候補 |
| `drivers/gpu/drm/vc4/vc4_hdmi.c` | `43df6bfb0420cc6e117a2c8ece1385fa20dea1d305367cbd1dcecf7e15817bbb` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_hdmi.h` | `2311e4a3e3c7a092b40a65f3f5ec2d35f1243e646cb9d15514a95fdcad609a08` | 表記なし（kernel の既定の GPL-2.0、COPYING） | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_hdmi_phy.c` | `4bf33891dd9cd39b198e800f841c75332a27f46821d77dce740f4ded74b09918` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_hdmi_regs.h` | `e97390a7ef71d57683cfb03d3c294b75eae372e332f25aa182a19efbd8894ab4` | 表記なし（kernel の既定の GPL-2.0、COPYING） | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_hvs.c` | `89fb981c35aee12d85dcd0f0120ce170ec672391ef4b28429431676a636f27dc` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_irq.c` | `12b515fc5047c6a9e4fa2a241f26b9e9a331a2e19540e992973196b0c8dafd3f` | MIT | 定義の取り込みの候補 |
| `drivers/gpu/drm/vc4/vc4_kms.c` | `fe34bdb66037dcafeaddef3039c048eecd3a91e804612cb562c9ea1273bd0c5a` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_packet.h` | `65c3026c94f6f64bdda82ef925f91d19db068cd9c27b5b9ba14ff7593348eab6` | MIT | 定義の取り込みの候補 |
| `drivers/gpu/drm/vc4/vc4_perfmon.c` | `5ecd1a105a86089d12782c5d6ac1e422302638c84d38c4887228cfc14c6456ff` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_plane.c` | `72abdef454d6c12e3509f0198024027317c3fa32cbb1c59c172255b3491ab158` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_qpu_defines.h` | `d1e69773e765c8926d9f0039ab1890d34f73128b6123740d86967d06f050b979` | MIT | 定義の取り込みの候補 |
| `drivers/gpu/drm/vc4/vc4_regs.h` | `550f2517ce8371ab9c4cfb065f699667e0c3c4079d5715a5be6f5abfa3214964` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_render_cl.c` | `e2fb02fad47d8a070358dcef7c6f1d5407c0ba9246a41458fbb59a34e3093d07` | MIT | 定義の取り込みの候補 |
| `drivers/gpu/drm/vc4/vc4_trace.h` | `cf46e5f24e97253fc86911c38ecae1e316c9e266204d2c9b013b31e7dc5edbf0` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_trace_points.c` | `bc63241f4622dabf0d2968d6b633e252f1dc500035f613b492146204c669d6e7` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_txp.c` | `6bbf7c4d94571fb8fb41fbbe316d1cb6abfe651190aa0c1695dae3de73dd87b3` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_v3d.c` | `11f1c06b402949b3c85b9185d973050a255567c9f32d6cb2c6f849fa55be94b3` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `drivers/gpu/drm/vc4/vc4_validate.c` | `03d5e8145419fe117549a3d171974ac8904325b5539074449ad570c9084cb746` | MIT | 定義の取り込みの候補 |
| `drivers/gpu/drm/vc4/vc4_validate_shaders.c` | `ae57ca9ab0a799ecf2d2fe39729842ed1573926062e4819876b9eecb3dee9be9` | MIT | 定義の取り込みの候補 |
| `drivers/gpu/drm/vc4/vc4_vec.c` | `948c80540383f121bbdde907b28eaac91c9ba537207dea948cf8b5fb218aa5c2` | GPL-2.0-only | 読むだけ（作業の文書は temp） |
| `include/soc/bcm2835/raspberrypi-firmware.h` | `6fddc5eac637338233ed4e1cf63775d6eddaf310a1f28eccd48ab99b0e75ea6f` | GPL-2.0 | 読むだけ（作業の文書は temp） |
| `include/uapi/drm/v3d_drm.h` | `eafcaa1f22beedd040baa27912b506732bc32fed01b23a4b581148a267afbac3` | MIT | 定義の取り込みの候補 |
| `include/uapi/drm/vc4_drm.h` | `753ab260b706d1840e4b1c2887bdababbe7341de2e3460c9ace4ace4a161f889` | MIT | 定義の取り込みの候補 |

| path（Mesa 25.3.6） | SHA-256 | license | 扱い |
| --- | --- | --- | --- |
| `src/broadcom/cle/gen_pack_header.py` | `2e414ea4b1d6f2628f301a058f5eb1e16c4c530a2bad7ee21cc7853244a8919d` | MIT | 定義の取り込みの候補 |
| `src/broadcom/cle/meson.build` | `ce0269e49b2578c42eed5146dd3d7e797c2ffcb993edaf776d22083a94543998` | MIT | 定義の取り込みの候補 |
| `src/broadcom/cle/v3d_decoder.c` | `9fd56dcfe3492c81498ad1889286fb1148f4a71bc977b1ab208b3c51e4ee81a0` | MIT | 定義の取り込みの候補 |
| `src/broadcom/cle/v3d_decoder.h` | `6e4deb7aae13e24963de7e1ce9f4545e7c34482b6540d13ded137b2b9b8a9954` | MIT | 定義の取り込みの候補 |
| `src/broadcom/cle/v3d_packet.xml` | `b13f995b1a4b606a1adcb2c048ff9bca067106be14e22ce1ee86e01055094e1b` | 表記なし（Mesa の既定の MIT、docs/license.rst） | 定義の取り込みの候補 |
| `src/broadcom/cle/v3d_packet_helpers.h` | `23ffee040b3d4754b79f7703fb13382306db79b166b562b92784e6ce41c33d70` | MIT | 定義の取り込みの候補 |
| `src/broadcom/cle/v3dx_pack.h` | `6675c34c7c35ced36d5c2f50232f0192f52f1e467bc1f1225b9d386ae9a9095d` | MIT | 定義の取り込みの候補 |
| `src/broadcom/cle/vc4_packet.xml` | `d57e69b656befa3c3070cd9e1bc45b3c35a79db1329eef0f55bc63fceca78f5d` | 表記なし（Mesa の既定の MIT、docs/license.rst） | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_cpu_tiling.h` | `dd50b023d9931b2e2aed73e8b1d12d0c94ed45cda8228cc77389bac1b5013959` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_csd.h` | `b424b81eb86a961cd8451cf4fe7adfb0c34c5169e5da9555d3e45d0f86e69064` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_debug.c` | `61d7a0b093ffe77891ea8435c283a862512dd6403502f61142c5111da5a8ed1a` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_debug.h` | `9eae8505e450fedb43fe3310024195795d78c7f5c7e56c74f3bd38d2ec5b273d` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_device_info.c` | `36b5b286e461e89ddb8c7425d6733f9887963289f566987f3682d92d10b1fd8b` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_device_info.h` | `27f8e9adbd7710eb3927ddd8d9789926503497b383fb728b3cf71b1ca39c6c68` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_limits.h` | `a4ccb6b77ccc2862c2a56fd49bc461fdab24bb4d6dee28a08d872f7b1220856b` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_macros.h` | `60b2942ecd03fab48473fa25a5aca5f7b4621b6c0a204be81c608ce26a1a89e2` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_performance_counters.h` | `ba15dd5300dcee88d4a5caeef0b3929798a148f02e2d35e9c0473019df75bbe1` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_tfu.h` | `fa4722f6346ba507fc7c325e1b6f727ea8d460da3804b53c30b0c128db5b0e12` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_tiling.c` | `16621af23ca184b30108372f477f9fc76734234a9f0d50d633b6a4fab692b253` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_tiling.h` | `d39210222d85b97629fde56cdaf5332060c2747b441b45c5ef2eef2514556659` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_util.c` | `dbd6959e3b10053ff58c636aade579b82d22c7be681d2c20f456f3a18582515b` | MIT | 定義の取り込みの候補 |
| `src/broadcom/common/v3d_util.h` | `0804032bbd6c93d41f3ccd278975a358f3546aaf05383d9853390d30da12c2dc` | MIT | 定義の取り込みの候補 |
| `include/drm-uapi/drm.h` | `6b80aff056e2ac2e126e5144a3ce2c750292edb4d080d4689ac487dc17e4dae8` | MIT | 定義の取り込みの候補 |
| `include/drm-uapi/drm_fourcc.h` | `b2107e3ad4c6df58d69739d000e7cb0ab63ce4b5828f909dabc4d952be5917c4` | MIT | 定義の取り込みの候補 |
| `include/drm-uapi/v3d_drm.h` | `4e02a6af8c7c585d8a3f9e04abe8fa5af3723f3d7ef0415c240329303005774d` | MIT | 定義の取り込みの候補 |

### 追加の参照（作業の文書が読んだ周辺の file、2026-10-04）

| path（Linux v6.19） | SHA-256 | license | 扱い |
| --- | --- | --- | --- |
| `drivers/pmdomain/bcm/bcm2835-power.c` | `53ba4530e8e90a9284d6fc57d2d2a55ba585a6e8c11f8dee63b79ffd58e0604d` | GPL-2.0+ | 読むだけ（V3D の電源 domain と reset の手順） |
| `drivers/pmdomain/bcm/raspberrypi-power.c` | `dbad5a29f4f76f341e913634c0b7b155a02f6b03f9fac7ce26ad8460863a21cf` | GPL-2.0 | 読むだけ |
| `drivers/clk/bcm/clk-raspberrypi.c` | `2a999a429986cfae5d1e22fbbb6c14bc1ab0ec67ba656e21bdee64ac9a12639a` | GPL-2.0+ | 読むだけ（firmware の clock の扱い） |
| `drivers/clk/bcm/clk-bcm2711-dvp.c` | `acd1d1cb847b945f24d6fbbad607d4f8789c51e5b2a781666402251ca4bb7962` | GPL-2.0-or-later | 読むだけ（HDMI の reset と gate） |
| `drivers/reset/reset-simple.c` | `d02683788561c8d2ada66ae0b20bf9e738a54193a527644eb5edd5e40945c01e` | GPL-2.0-or-later | 読むだけ |
| `drivers/reset/core.c` | `f6fb280cfce71b7ac92bdaa3576337d43351889e1e63e05b1c139a24716596ba` | GPL-2.0-or-later | 読むだけ |
| `drivers/i2c/busses/i2c-brcmstb.c` | `7b63e056d6f03ab99cbb4f7361f303f97fb13f3d895d270de83a530ab6885eb8` | GPL-2.0-only | 読むだけ（HDMI の DDC の実際の driver） |
| `drivers/i2c/busses/i2c-bcm2835.c` | `cb971b971bfdeac7a055217a78282ab1a2f449e531b79ee5dc9f0ca02560628f` | GPL-2.0 | 読むだけ（DDC の driver でないことの確認） |
| `drivers/gpu/drm/drm_atomic_helper.c` | `2f68a8a867a1f66583c0f4b78a820500af2fd86212604ec3f625f6711c279fae` | MIT | 読むだけ（commit の順） |
| `drivers/gpu/drm/drm_edid.c` | `0cd869d8e55ce58dd787eb8ec68c34663bb3291a352e43fee9f02955be0c4a12` | MIT | 読むだけ（DDC の EDID の読み） |
| `arch/arm/boot/dts/broadcom/bcm2711.dtsi` | `b33a898bd3d841552699a90ca85153c32778dfd76e46ead50ee191c9e3f67520` | GPL-2.0 | 読むだけ（address・割り込みは DT の interface の事実） |
| `arch/arm/boot/dts/broadcom/bcm2711-rpi.dtsi` | `13a42cdc5aa8e85a2bca0d058eced7fada8213203ea9eacaf8ad32e3eccfbb47` | GPL-2.0 | 読むだけ |
| `arch/arm/boot/dts/broadcom/bcm2711-rpi-4-b.dts` | `6bc587c46e263bf096ee5f402669bd985d95a6048f77991ffd96bf7a7e6c6d86` | GPL-2.0 | 読むだけ |
| `arch/arm/boot/dts/broadcom/bcm283x.dtsi` | `999bc1a10e79136b30e1f87633ba4eb2eb9e5d2b79edece0b6d5133625167447` | 表記なし（kernel の既定の GPL-2.0、COPYING） | 読むだけ |
| `arch/arm/boot/dts/broadcom/bcm2835-rpi.dtsi` | `7142bfd3bec96945b30b45f72cd2bc70ed8cddbff3961a896a58b60e57ac5350` | 表記なし（kernel の既定の GPL-2.0、COPYING） | 読むだけ |
| `include/dt-bindings/power/raspberrypi-power.h` | `eae21118acf3c5ec97e48b81424cbaadeb9391f3a5cba3b8aba7badb3a62d2e6` | GPL-2.0 | 読むだけ |

| path（Mesa 25.3.6） | SHA-256 | license | 扱い |
| --- | --- | --- | --- |
| `src/broadcom/vulkan/v3dv_cmd_buffer.c` | `eaf3c553f3fcd8e26213981d05ae94b88635684b1a5b360d35271424d0861f6d` | MIT | 手順の参照（tile alloc の大きさ、CSD の設定） |
| `src/broadcom/vulkan/v3dv_queue.c` | `5a2b307ca9db3f9dcda0fa5bf6b5cb22fe84f664d893c7e3f5c60cbc00a254ac` | MIT | 手順の参照（submit の埋め方） |
| `src/broadcom/vulkan/v3dvx_cmd_buffer.c` | `8e314e2c49360193d888de3ad07b0a2755c63def7dab94c31893007fac561e9c` | MIT | 手順の参照（bin の CL の前置き） |
| `src/broadcom/vulkan/v3dvx_meta_common.c` | `ffaf743923992b5b1657a131552f81ed9131adb4083d1a75b7444fb48f4c940c` | MIT | 手順の参照（render の CL、clear と store） |
| `src/broadcom/vulkan/v3dv_meta_copy.c` | `e9683a98530ffa2ebe931638ff154aa35db0353666155aba280c2096abb6f1b0` | MIT（file先頭のRaspberry Pi Ltd許諾を確認、2026-10-09追加） | 固定Mesa commitからignored tempへ取得。image/TFUの呼び手を読むだけ、code/objectの取り込み無し |
| `src/broadcom/vulkan/v3dvx_queue.c` | `3c5e22ad6e8164433fbe1ac6a76f2a37c540f9bc45a3afb315180c8afe8028c5` | MIT | 手順の参照（何もしない job） |
| `src/broadcom/simulator/v3dx_simulator.c` | `c4bd6ac8ca1342b2266535540b55b7b79bc9dff5d2ae99a0c173402019c4b163` | MIT | 手順の参照（完了の poll、cache の flush） |
| `src/broadcom/drm-shim/v3d_noop.c` | `b3a0b944e6b83789c2d1c30022a81a41fa5b2e9e27ae003631e03ec4253c663d` | MIT | 識別の register の期待値 |
| `docs/license.rst` | `0d1a0472ecc81830e75c20d59b0ea02841e3db21255e0ebad97ab682c54d6615` | （license の説明の文書） | `v3d_packet.xml` の既定の license の根拠（[design](rpi4-gpu-design.md) の判断の項目 2） |

| path（Raspberry Pi firmware、commit `3d301dd924bcd758a4c8cb19fe8531031f033f43`、`vendor/raspberrypi-firmware` の固定） | SHA-256 | license | 扱い |
| --- | --- | --- | --- |
| `boot/bcm2711-rpi-4-b.dtb` | `75761b73c284e26623e4d1624bff13e67bce2ae620880efd81d6571a3739fcfb` | firmware の配布物（DT の source は GPL-2.0 の dts） | boot で firmware が渡す。driver は実行時に node を読む（interface の事実）。repository には入れない（submodule） |
| `boot/overlays/vc4-kms-v3d-pi4.dtbo` | `155ec071f26a8d45c4524ee00c5037ad7ceb8cc81d221640b9d9d7177c416ddf` | 同上 | 読むだけ（何を okay・disabled にするかの確認） |
| `boot/overlays/vc4-fkms-v3d-pi4.dtbo` | `eae7ebead84e86c3d287453ccfe443560c28f53189d6afa657a7f1d084c77c32` | 同上 | 読むだけ |

| path（Raspberry Pi firmware の wiki、`https://github.com/raspberrypi/firmware.wiki.git` の commit `c9e615a74d377a7e94ac2140336d1352941ef9a2`） | SHA-256 | license | 扱い |
| --- | --- | --- | --- |
| `Mailbox-property-interface.md` | `1197d7f52ae2ce6b731bb1ea8e1fce93df4395983629994069e71d498160dd8b` | 表記なし（Raspberry Pi の公開の interface の文書） | mailbox の tag・clock の ID・framebuffer・EDID の tag の値の出典（interface の事実）。display の終了の通知と電源 domain の set の tag は載っておらず、GPL の header にしか無い（[design](rpi4-gpu-design.md) の判断の項目 17） |


## i14 compilerの追加参照（2026-10-09）

固定Mesa25.3.6 commit `06f9e28304d5d3f109c33535c1c25b9df5769af2`、ignored `temp/mesa/` のみ。各sourceの先頭のMIT許諾とhashを読み直した。新kernel QPU/compilerは独立のZlib実装であり、単一ALU/保守的idle/register liveness/semantic uniformの構成を採用。既存Zlib `src/drivers/gpu/i915/compiler/spirv.c` はdevice-independent frontendとしてread-onlyでarm64 source listへ追加し、Gen12 code generatorは使用しない。全WSの最終license/設計類似監査はp007で再実施する。

| path | SHA-256 | license | 扱い |
| --- | --- | --- | --- |
| `src/broadcom/qpu/qpu_instr.h` | `4b1b90e3bb8ea614dea36484e24d83049392c55cf19b01125627fa5ff4c9d07f` | MIT（各file先頭の許諾を確認） | native命令のformat factsと独立host oracle。kernelへのcode/object取り込み無し |
| `src/broadcom/qpu/qpu_instr.c` | `f61745f35e5b34c74aad56b01afb9e068860ecb067361b8f9610dc1b673c8d4e` | MIT（各file先頭の許諾を確認） | native命令のformat factsと独立host oracle。kernelへのcode/object取り込み無し |
| `src/broadcom/qpu/qpu_pack.c` | `abf436006dd3bed52cf1e77245d7267105a9100f02d8b3d643ddc6641fd6d551` | MIT（各file先頭の許諾を確認） | native命令のformat factsと独立host oracle。kernelへのcode/object取り込み無し |
| `src/broadcom/compiler/v3d_compiler.h` | `54723cbf03636cca567e7883e857652e53d6b8b526d9e9a3399befc040bbbe0f` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |
| `src/broadcom/compiler/vir_to_qpu.c` | `28cb5e58c46b2a81da62afe34468310525b15bcb5643e8b5586fed56954423da` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |
| `src/broadcom/compiler/nir_to_vir.c` | `d5debdf3036532223afc70a3246c4d07f038dceaa83c96f75d19a98c45c9431a` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |
| `src/broadcom/compiler/qpu_schedule.c` | `e282c7ae7a5cc231c83637c9c57285a480654f3cb12e4e86c0152508d1c9cc1e` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |
| `src/broadcom/compiler/qpu_validate.c` | `4102b7282bcc1647582dc210173022cf04c35a018abe58d079dd153de2506dd0` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |
| `src/broadcom/compiler/v3d_tex.c` | `c8a260f669908f80ba1547d42180ee6aebc87c6024dfc2e1d9dcf119c1a7f62b` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |
| `src/broadcom/compiler/v3d_nir_lower_io.c` | `ca9e6568235109dfa63a417e324df1441c2c8b21f48962a97de48bef4099ee82` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |
| `src/broadcom/compiler/vir_register_allocate.c` | `ff2493df97d6191e2e4fd0726fa47ffe42808d5d3b813bb66b7690686e6d38b9` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |
| `src/broadcom/vulkan/v3dv_pipeline.c` | `53dec555a2cf7a785d73cbc11b02eb06ed6d61634544b31f5ca572815468d562` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |
| `src/broadcom/compiler/meson.build` | `8fc878fac85e2450999e83c1a7676b79128b498f1189ae50f06cef4deaf00f78` | MIT（各file先頭の許諾を確認） | ABI/latency/VPM/TMU/TLB/thread規則を読むだけ。外部compiler実装の取り込み無し |

## native shader/texture stateの一次資料追加（2026-10-09）

固定Mesa25.3.6 commit `06f9e28304d5d3f109c33535c1c25b9df5769af2` の公式GitLab rawから取得し先頭MIT許諾を確認。XML schemaとnative stateの意味を照合するignored作業資料。whole p007 auditは未達。

| path | SHA-256 | license | 扱い |
| --- | --- | --- | --- |
| `src/broadcom/vulkan/v3dvx_pipeline.c` | `59bf28e7143f60bf6b264064773b63937ff04ed4dff96087ffb1f09181007158` | MIT（先頭許諾確認） | ignored `temp/v3dvx_pipeline-fetch.c`、hardware事実と手順を読むだけ、source/objectの取り込み無し |
| `src/broadcom/vulkan/v3dvx_image.c` | `95d3219b72adb1c342a165d0f82275c5ccc6f5b997b40f837d0beebb8c4264e7` | MIT（先頭許諾確認） | ignored `temp/v3dvx_image-fetch.c`、hardware事実と手順を読むだけ、source/objectの取り込み無し |
| `src/broadcom/vulkan/v3dvx_descriptor_set.c` | `1247acafa2ffeebaef00f49a800f3cabf319793dae90ff7c4326fff15b325757` | MIT（先頭許諾確認） | ignored `temp/v3dvx_descriptor_set-fetch.c`、hardware事実と手順を読むだけ、source/objectの取り込み無し |
| `src/broadcom/vulkan/v3dv_image.c` | `1d86296404f2a133aa96de2ff3c49dadea1f02c0e84df7d484397f5208c28474` | MIT（先頭許諾確認） | ignored `temp/v3dv_image-fetch.c`、hardware事実と手順を読むだけ、source/objectの取り込み無し |
| `src/broadcom/compiler/vir.c` | `a1f314656a6bdb53a68b682e665303644fb8bbb74078a76d5c447107a9a7ccaf` | MIT（Broadcom 2016–2017、先頭許諾確認） | ignored `temp/vir-fetch.c`、VPM sectorと共有segmentのhardware事実のみ。source/object取り込み無し |
