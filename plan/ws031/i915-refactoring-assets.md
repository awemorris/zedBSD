# WS031 i915再構成 — 全ファイル保全台帳

作成日: 2026-09-21。基準commit: `7e7ff337c7f145e5fd79e4daaafa05a1cd4a4bcc`。
[設計本文](i915-refactoring-design.md) / [関数台帳](i915-refactoring-functions.md) /
[敵対的レビュー](i915-refactoring-review.md)。

## 保全単位

src/drivers/gpu/i915以下の**全378ファイル**を列挙する。
C/H/INC 376（明示関数2,988）、manifest 1、shell runner 1。
関数がない178ファイルだけではなく、関数を持つ200ファイルの型・変数・定数・
function-local static・ops・macro・include・条件分岐・licenseも移行対象。

「主な受入先」は最終責務の配置案。混在ファイルは関数台帳とレビュー§4の状態表を併用する。
**受入先名があることは、その全statementの所属・削除可否を確認済みという意味ではない。**
未分類の残りを捨てず、旧TU/元ファイルを保全したまま切り出す。
個々の変数・型・macroまでの完全な名前付き移動表は残件である。

| 移行対象 | 旧ファイルを除く前の条件 |
| --- | --- |
| 関数 | 関数台帳の主担当、分割された枝、呼出元とprototypeが対応。Sは別TUから直接呼ばない |
| 型・変数・ops・local static | 定義元と所有寿命を指定、全参照とinitializerの関数アドレスを追従。globalとlocal staticの重複を検査 |
| macro・inline・定数 | include順と条件付き展開を保存。function-like macroも動作の一部として保持 |
| 生成物 | 元入力/生成器/spec/出力manifestの組を記録。手管理へ切替える場合はその判断と出典を残す |
| 試験/診断 | test無効時の本番起動・通常描画に必要な要素を持ち去らない。試験側でのみ依存注入 |
| 不要とする要素 | 個別の理由と代替、callerなしの証拠を残す。「全関数を移したからファイルを削除」は不可 |

表のSHA256はレビュー時点の元ファイル内容を固定するためのもの。
define数はコメント・文字列を除いたソース上の出現数（計6,648）。
preprocess後の有効定義数でも、macro意味論の検証件数でもない。
独立関数形検査の追加候補83箇所は24種類のmacro/initializerであり、
今回追加すべき明示関数は見つからなかった。AST/linkの完全性証明ではない。

## 全ファイル

旧パスはsrc/drivers/gpu/i915相対、新パスも特記なければ同ディレクトリ相対。
関数を持つC/INCの「受入先」には補助状態・定数の配分も伴う。
複数owner共有の型はprivate header、immutable dataはdataの機能別fragmentへ切り出す。

| 旧ファイル | 関数数 | define数 | 主な受入先（本文・状態表併用） | 元SHA256 |
| --- | ---: | ---: | --- | --- |
| [draw_fixture.h](../../src/drivers/gpu/i915/draw_fixture.h) | 0 | 27 | `tests/fixtures/draw-fixture.h` | `047770620020fa21a73fa9b4e56c5b4f35a39a7a0a05bccd507dbf26eb6030c3` |
| [engine.c](../../src/drivers/gpu/i915/engine.c) | 15 | 3 | `engine.c`<br>`reset.c` | `31cd8d6ad0a44be7d979994cf4356c8f973fdd7c296c2312579ce100b76489b8` |
| [gem.c](../../src/drivers/gpu/i915/gem.c) | 9 | 0 | `memory.c` | `f8309d886abb571f0504089a8a3d3dc1b5afe14b74850655dd883d527890a7bc` |
| [ggtt.c](../../src/drivers/gpu/i915/ggtt.c) | 13 | 0 | `ggtt.c` | `1b3a2f8ae824fe4e4dc6d4c5f2ddfb2732ed6fdd8a88407e6a8b2da2b64f5a23` |
| [i915.c](../../src/drivers/gpu/i915/i915.c) | 44 | 8 | `command.c`<br>`device.c`<br>`engine.c`<br>`i915.c`<br>`job.c`<br>`reset.c`<br>`resource.c`<br>`session.c`<br>`ops.h`、`device-info.c`、capability profile | `d24920ddf8751617ff22470694b31d52c8d82a6db1057ea7a1ed77d1053ce8ed` |
| [internal.h](../../src/drivers/gpu/i915/internal.h) | 0 | 57 | `i915.h` / `device.h` / `session.h` / `memory.h` / `ggtt.h` / `ppgtt.h` / `context.h` / `request.h` | `533d177bc5856d90685f0f873e9e6c3bc4aeb2f192e9369feae06479382fa7c7` |
| [irq.c](../../src/drivers/gpu/i915/irq.c) | 9 | 1 | `irq.c` | `186d93ce05f20712827b4a9ab98295e90107266936b4210a647bb5609c2e2a44` |
| [linux/i915-commands.inc](../../src/drivers/gpu/i915/linux/i915-commands.inc) | 0 | 71 | `data/i915-commands.inc` | `2143297bab8e8dcdc743818bc937c44f0edb742fa22c4b1838ebb0c53f233554` |
| [linux/i915-ids.inc](../../src/drivers/gpu/i915/linux/i915-ids.inc) | 0 | 8 | `data/i915-ids.inc` | `3aaf610f1c2be212d9053317761a2fc02856e440ce6b17da13ce3be893ea9a7a` |
| [linux/i915-lrc-offsets.inc](../../src/drivers/gpu/i915/linux/i915-lrc-offsets.inc) | 0 | 12 | `data/i915-lrc-offsets.inc` | `b67aa3d20a3ec1f2c8b8adef623a74d6a8b3f7af6b7b54c1fabef6cfe1c4ddd4` |
| [linux/i915-mocs.inc](../../src/drivers/gpu/i915/linux/i915-mocs.inc) | 0 | 38 | `data/i915-mocs.inc` | `58f4eefc90a151cb497202650e87e80666aa4af68f18e186d5ac819c528decd9` |
| [linux/i915-regs.inc](../../src/drivers/gpu/i915/linux/i915-regs.inc) | 0 | 174 | `data/i915-regs.inc` | `22fe9d37e745e9088d82f5d1dfbbcd1f1705e1d5661bac9ebab134c02fa62f52` |
| [linux/i915-workarounds.inc](../../src/drivers/gpu/i915/linux/i915-workarounds.inc) | 0 | 51 | `data/i915-workarounds.inc` | `f83a5c56ed1a9f0f05acd76ae7eda21d2e9d1ad9908c4c9cf9f2b31d9e7adde2` |
| [lrc.c](../../src/drivers/gpu/i915/lrc.c) | 12 | 5 | `context.c` | `dd28d5843784366fc02e2d0c6bf0c11f14b91d78fe674013e4054b0f66f42428` |
| [parity/backend.h](../../src/drivers/gpu/i915/parity/backend.h) | 0 | 1 | `device.h` / `mmio.h` / `memory.h` / `power.h` | `dd40c9ec01684c64e38e51016b7eb611c2d0557c5ccb2fbd60656898d0d81786` |
| [parity/backend_delayed.c](../../src/drivers/gpu/i915/parity/backend_delayed.c) | 11 | 0 | `sync.c` | `3f6c1b888e3064eddfb9a42b5d869c41bc678de27989f73a22c583b681a66af3` |
| [parity/backend_delayed.h](../../src/drivers/gpu/i915/parity/backend_delayed.h) | 0 | 2 | `sync.h` | `da67d1d65788dc6a292c29985ea25129281dfddbb3c9f5a7c70c10e605eb9bc4` |
| [parity/backend_dma.c](../../src/drivers/gpu/i915/parity/backend_dma.c) | 2 | 0 | `memory.c` | `b24bd6cfefead22e0bed3b39c67f898a3dd43054ae56553e6f4be530823d0481` |
| [parity/backend_mmio.c](../../src/drivers/gpu/i915/parity/backend_mmio.c) | 13 | 11 | `mmio.c` | `e4f6a401faf9f3c254419154fa4fecfd30c76de9b53c16d8d7aec1bc45665c0a` |
| [parity/backend_pci.c](../../src/drivers/gpu/i915/parity/backend_pci.c) | 10 | 0 | `device.c` | `ef6d0aac8ac7cb9d7e8a3bee08d10f2dbf86d4544e1766ce60cfd57c1fb4eda4` |
| [parity/backend_sync.c](../../src/drivers/gpu/i915/parity/backend_sync.c) | 15 | 0 | `sync.c` | `f729613eadf4ecab8f9320bf84f3c50b09d3b24ef85b02ef2ef756d2b7e5714b` |
| [parity/backend_sync.h](../../src/drivers/gpu/i915/parity/backend_sync.h) | 0 | 2 | `sync.h` | `2646d75a4d97b50383157ee4503e6645d88aa84ac7ac46764d4179c2af3a0bf8` |
| [parity/bios.c](../../src/drivers/gpu/i915/parity/bios.c) | 17 | 17 | `display/vbt.c` | `e7e3a2690451553a92c7fff7fa43a7de36882f7e750cb2b233206c5807897b65` |
| [parity/bios.h](../../src/drivers/gpu/i915/parity/bios.h) | 0 | 21 | `display/vbt.h` | `209a5369e83adafd45605057257080c21d7877eb2451731b8a3458c3b92b2b0a` |
| [parity/cdclk.c](../../src/drivers/gpu/i915/parity/cdclk.c) | 29 | 23 | `display/clock.c` | `d989c473352031ff44d68fb926d8a87d8d8d4b126a63874ee566d4b058cee93e` |
| [parity/cdclk.h](../../src/drivers/gpu/i915/parity/cdclk.h) | 0 | 1 | `display/clock.h` | `42ca0d8e3412d3d48a0f646c2f81f5cf6a88eaba536f97214e458fadca79a5a6` |
| [parity/combo_phy.c](../../src/drivers/gpu/i915/parity/combo_phy.c) | 19 | 18 | `display/phy.c` | `e79859b6e10493a351c254b41df1b491cdeb0e42ec2a55223284b66a000dbe7a` |
| [parity/combo_phy.h](../../src/drivers/gpu/i915/parity/combo_phy.h) | 0 | 4 | `display/phy.h` | `cf45295335d9757b4234b077aae9b33785c4c159cf105b8ef8eba41c07381b7b` |
| [parity/display_core.c](../../src/drivers/gpu/i915/parity/display_core.c) | 17 | 36 | `display/power.c` | `3c95a9ca84ec2888e2a915992e49a140f863119cc8089211fba1331b11049401` |
| [parity/display_core.h](../../src/drivers/gpu/i915/parity/display_core.h) | 0 | 1 | `display/power.h` | `80988aaa0c98267b920c380ea9e0f5c367a3b120914f6d5e51021f26b99bd08f` |
| [parity/display_nogem.c](../../src/drivers/gpu/i915/parity/display_nogem.c) | 46 | 109 | `display/takeover.c` | `28a6739bec843033df4f7f333665eead60e21403866f1cb5d2ca4bc792f94ed7` |
| [parity/display_nogem.h](../../src/drivers/gpu/i915/parity/display_nogem.h) | 0 | 23 | `display/takeover.h` | `c8cc970ecf34f4f24ea4f7ee96b8dd06f4862bf1ecb3938fc8d5806a193002c8` |
| [parity/display_state.c](../../src/drivers/gpu/i915/parity/display_state.c) | 33 | 13 | `display/state.c`<br>`display/watermark.c` | `4bd24643dbe05af13e64efd85ee6923324ae64e0e357aeaeb0b25296dc60418b` |
| [parity/display_state.h](../../src/drivers/gpu/i915/parity/display_state.h) | 0 | 2 | `display/state.h` / `display/watermark.h` | `c2f53e9eb8a90598a942792a5535466b171cd784653b012fc763060db3e2e78d` |
| [parity/dmc.c](../../src/drivers/gpu/i915/parity/dmc.c) | 26 | 31 | `display/dmc.c` | `70e9cefd3431350275a482d62488dc0f8ebd9bdebe19123b15516f80c2507696` |
| [parity/dmc.h](../../src/drivers/gpu/i915/parity/dmc.h) | 0 | 2 | `display/dmc.h` | `d71b332f3aef5d28b969ae14bf7cf0ff235dbf40e2ed71f051d3341844a4de9a` |
| [parity/dp/dp_compat.h](../../src/drivers/gpu/i915/parity/dp/dp_compat.h) | 12 | 93 | `display/dp-internal.h`（macroの意味を保存し明示loopへ置換） | `484a411b3ddbd44825e1f2389c622ca2261d3b48ef56f14813dd084c1b24e743` |
| [parity/dp/dp_fake_hw.c](../../src/drivers/gpu/i915/parity/dp/dp_fake_hw.c) | 24 | 23 | `tests/display/dp-fake-hw.c` | `1993e8673333770c8bec9b1b413865d390d48d916ba083332332ab981ce7f847` |
| [parity/dp/dp_fake_hw.h](../../src/drivers/gpu/i915/parity/dp/dp_fake_hw.h) | 0 | 2 | `tests/display/dp-fake-hw.h` | `83ef2cf0b5d118f214b8874af52302ef9198df03cc999b413c739d66eef5f921` |
| [parity/dp/dp_fixture_latitude5330.h](../../src/drivers/gpu/i915/parity/dp/dp_fixture_latitude5330.h) | 0 | 1 | `tests/display/dp-fixture-latitude5330.h` | `f6c34469c6dcb91065462fc80aaab2d96dc54da00d75085e02e13360d87428c6` |
| [parity/dp/dp_ref_types.h](../../src/drivers/gpu/i915/parity/dp/dp_ref_types.h) | 0 | 1 | `display/internal.h（型/inlineを所有headerへ再配分）` | `b96a22a7f632e19c6d8941e19dc91d4127a76073a74cb918a17d711ed89d5f03` |
| [parity/dp/drm_dp.h](../../src/drivers/gpu/i915/parity/dp/drm_dp.h) | 0 | 1193 | `data/display-drm-dp.inc` | `76893a25373240b83fbedfc2fca7b202f448fe2552d83f808dc118aec5012465` |
| [parity/dp/drm_dp_helper_port.c](../../src/drivers/gpu/i915/parity/dp/drm_dp_helper_port.c) | 17 | 12 | `display/dp.c` | `86a8fd5cc463469b6cf4c053bf7a5b4e7c520a44de595c50149d84741435acb2` |
| [parity/dp/drm_edid_port.c](../../src/drivers/gpu/i915/parity/dp/drm_edid_port.c) | 4 | 3 | `display/edid.c` | `d70bc4b33dfdf996ddbf36ff055def8345c4d05d676e340209e87477d0af43eb` |
| [parity/dp/edp_ktest.c](../../src/drivers/gpu/i915/parity/dp/edp_ktest.c) | 4 | 0 | `tests/display/edp-ktest.c` | `9a657fd39968cc3ae457e8fc9e64c9027b16703c2185339e352171b2825d723e` |
| [parity/dp/edp_ktest.h](../../src/drivers/gpu/i915/parity/dp/edp_ktest.h) | 0 | 1 | `tests/display/edp-ktest.h` | `6f5a51f934552b1aa515aadb7bec15814bad5488410d7cbe2e81d7630c770548` |
| [parity/dp/edp_sync_ktest.c](../../src/drivers/gpu/i915/parity/dp/edp_sync_ktest.c) | 16 | 0 | `tests/display/edp-sync-ktest.c` | `e30c9373774657d7a34f046cf64bed29ef75af520b22a83af985a0f7fa392fbd` |
| [parity/dp/intel_dp_aux.h](../../src/drivers/gpu/i915/parity/dp/intel_dp_aux.h) | 0 | 1 | `display/aux.h` | `762ec4a5347b2d1ea0a58fe72360232f413a366b56eb6cc0d74320385165ce08` |
| [parity/dp/intel_dp_aux_port.c](../../src/drivers/gpu/i915/parity/dp/intel_dp_aux_port.c) | 13 | 2 | `display/aux.c` | `d20cb57dbf100b2dc0c11ee069a9d60e5350bce1b9ac4fcf407b8caab05868f7` |
| [parity/dp/intel_dp_aux_regs.h](../../src/drivers/gpu/i915/parity/dp/intel_dp_aux_regs.h) | 0 | 52 | `data/display-intel-dp-aux-regs.inc` | `b21ad111df82a2b2b5be26028de5fd6a8e0e14fe81a28dc66a4f6cb6ddf14f6e` |
| [parity/dp/intel_pps.h](../../src/drivers/gpu/i915/parity/dp/intel_pps.h) | 0 | 2 | `display/panel.h` | `0c14917a4b7040d1dd1197f1c1263382c25e6afc3c9bce40c3ddf56c1319ef92` |
| [parity/dp/intel_pps_port.c](../../src/drivers/gpu/i915/parity/dp/intel_pps_port.c) | 54 | 8 | `display/panel.c` | `4b7d35da2807791770d6aa5addb1c477ce005ab785cfc91033055072238b4839` |
| [parity/dp/intel_pps_regs.h](../../src/drivers/gpu/i915/parity/dp/intel_pps_regs.h) | 0 | 51 | `data/display-intel-pps-regs.inc` | `ed219d8b23456d25b6f969d40193a88b0d1a42c5ba4edff16dc4c37321895b0e` |
| [parity/dp/parity_dp_aux_glue.inc](../../src/drivers/gpu/i915/parity/dp/parity_dp_aux_glue.inc) | 1 | 0 | `display/aux.c` | `07aa479b056667b9e30ade120292311eca1b36eea34b944f4a7980f789e586d0` |
| [parity/dp/parity_dp_kernel.c](../../src/drivers/gpu/i915/parity/dp/parity_dp_kernel.c) | 34 | 0 | `display/dp.c` | `017981df384be3ae6ad3fe9c36e92a6830e7e5c032773a9a8770104fda38d607` |
| [parity/dp/parity_dp_kernel.h](../../src/drivers/gpu/i915/parity/dp/parity_dp_kernel.h) | 0 | 1 | `display/dp.h` | `572abbded668e30561d005d83184eb9c276b2ee4d5e4c62027e97779784a64de` |
| [parity/dp/parity_drm_dp_glue.inc](../../src/drivers/gpu/i915/parity/dp/parity_drm_dp_glue.inc) | 1 | 0 | `display/dp.c` | `0996e09b13b61ae8b563df1b910c8bcb2db24f2c08d645403ecaa0e131ea236b` |
| [parity/dp/parity_drm_edid_glue.inc](../../src/drivers/gpu/i915/parity/dp/parity_drm_edid_glue.inc) | 1 | 0 | `display/edid.c` | `6e221862682a0fa148c017e6712591d35476bff7e9f268f0a0b39de8dc1966bc` |
| [parity/dp/parity_edp.c](../../src/drivers/gpu/i915/parity/dp/parity_edp.c) | 26 | 0 | `display/dp.c` | `7a2d19aab9e518ffe5c30af5e11be950433b4c5ae7372495fc717ba2bc23e3f8` |
| [parity/dp/parity_edp.h](../../src/drivers/gpu/i915/parity/dp/parity_edp.h) | 0 | 13 | `display/dp.h` | `0456da5bc763acd10979d4aa32ebc398a3344940e69ec416e5cd16dcd874e2ee` |
| [parity/dram_bw.c](../../src/drivers/gpu/i915/parity/dram_bw.c) | 6 | 7 | `display/watermark.c` | `1c54ec2c294d12c33f7e76b26cc84505b8b7a40fae2783a9f7cee31c003d4099` |
| [parity/dram_bw.h](../../src/drivers/gpu/i915/parity/dram_bw.h) | 0 | 4 | `display/watermark.h` | `fb60eb9446b3016f96a46c309782940a9021573d7d042c75d14b5420d86b79f7` |
| [parity/driver_probe.c](../../src/drivers/gpu/i915/parity/driver_probe.c) | 27 | 4 | `device.c`<br>`display/display.c`<br>`display/hotplug.c`<br>`display/power.c`<br>`display/watermark.c` | `0cd10de8a5f9712a53944b10b7c867c59f3c6b342b71a44ac4d893c07d747775` |
| [parity/driver_probe.h](../../src/drivers/gpu/i915/parity/driver_probe.h) | 0 | 24 | `device.h` / `display/display.h` / `display/hotplug.h` / `display/power.h` / `display/watermark.h` | `a1256040ee330753a3c979c1080e1d7bc00fae72a8ae88cc9ed8dc68c694b3c8` |
| [parity/drm_device.c](../../src/drivers/gpu/i915/parity/drm_device.c) | 7 | 0 | `device.c` | `2b895acec008529a40cf5e58266271ec9b62cb83981c01731ac04a986b7713e1` |
| [parity/drm_device.h](../../src/drivers/gpu/i915/parity/drm_device.h) | 0 | 3 | `device.h` | `02f83372cd31b80621e557e1887aaebd4852647fd0d330da7490f411e5275d48` |
| [parity/eu_test.c](../../src/drivers/gpu/i915/parity/eu_test.c) | 45 | 18 | `tests/execution/eu-test.c` | `96da98150f1d9f37459686c09d6056e827a6c143ab7cfaa4f8bf29ad725608fc` |
| [parity/eu_test.h](../../src/drivers/gpu/i915/parity/eu_test.h) | 0 | 33 | `tests/fixtures/eu-test.h` | `080c02a1809143b8f5c9345438c1be49062ea2389d1b14a5423a025375fe13d8` |
| [parity/firmware_adlp_dmc.c](../../src/drivers/gpu/i915/parity/firmware_adlp_dmc.c) | 0 | 0 | `data/firmware/firmware-adlp-dmc.c` | `71539a189cf4c4df72013e6b67956e86e69dc88636a6c51edc5d713b340e3c01` |
| [parity/firmware_tgl_dmc.c](../../src/drivers/gpu/i915/parity/firmware_tgl_dmc.c) | 0 | 0 | `data/firmware/firmware-tgl-dmc.c` | `a841331516bff58e9228a09042ab0fa2d3c4af101c009a0a8de7d8211eea0a2d` |
| [parity/firmware_vbt_dell_latitude_5320.c](../../src/drivers/gpu/i915/parity/firmware_vbt_dell_latitude_5320.c) | 0 | 0 | `data/firmware/firmware-vbt-dell-latitude-5320.c` | `bb4197a109e300983b4538b9d1d3a3ae777006fe6e0d919a2d3044322d8bbef2` |
| [parity/firmware_vbt_dell_latitude_5330.c](../../src/drivers/gpu/i915/parity/firmware_vbt_dell_latitude_5330.c) | 0 | 0 | `data/firmware/firmware-vbt-dell-latitude-5330.c` | `06492ceb89d2a0905190804c7d368f49306b16394321fb873101796b9dcb38ff` |
| [parity/gt_defaults.c](../../src/drivers/gpu/i915/parity/gt_defaults.c) | 9 | 0 | `context.c` | `0d14aab9717205aebcba7ae23e72af054a7e3a087986dd3a7c370c27ba209c6e` |
| [parity/gt_defaults.h](../../src/drivers/gpu/i915/parity/gt_defaults.h) | 0 | 5 | `context.h` | `83ed26cd2014f474a0d3ed935b3f1e8ba842704937767bf95f6ae6b9868eb73c` |
| [parity/gt_engine.c](../../src/drivers/gpu/i915/parity/gt_engine.c) | 12 | 2 | `engine.c` | `d839d94bf682d3e73f5048b4282457a8a2732e04af0927498612890cc935a33f` |
| [parity/gt_engine.h](../../src/drivers/gpu/i915/parity/gt_engine.h) | 0 | 35 | `engine.h` | `ea490c7b33c6c818fb08bdd6353cb26d38fd6531b82d222948ba681a98b4eda3` |
| [parity/gt_fw_ranges.inc](../../src/drivers/gpu/i915/parity/gt_fw_ranges.inc) | 0 | 0 | `data/gt-fw-ranges.inc` | `8488e76c7ebdd280b2c232510cc4f3ca5c87b0df0cdf2f5264ca77008d2e4dcb` |
| [parity/gt_init.h](../../src/drivers/gpu/i915/parity/gt_init.h) | 0 | 4 | `device.h` / `power.h` / `workarounds.h` / `ppgtt.h` | `2827307a7eec752be6554396eb0254e035f5b87b20e4bdfe00885b59fd024c31` |
| [parity/gt_init_base.c](../../src/drivers/gpu/i915/parity/gt_init_base.c) | 24 | 55 | `device.c`<br>`power.c`<br>`ppgtt.c`<br>`workarounds.c` | `3ee1c71464693666886e497b87f4cf49d9b1c228fe345cd21d740352dfb8e63a` |
| [parity/gt_lrc.c](../../src/drivers/gpu/i915/parity/gt_lrc.c) | 28 | 2 | `context.c` | `b4bc82e542e2a36e9e36b2c84b89f06f0aadf11f75a86123cb52888b01e253e6` |
| [parity/gt_lrc.h](../../src/drivers/gpu/i915/parity/gt_lrc.h) | 0 | 64 | `context.h` | `31257fe38c5c9128ce10f5a438f8661fcffe26d882e53315a99e48195946c367` |
| [parity/gt_lrc_offsets.inc](../../src/drivers/gpu/i915/parity/gt_lrc_offsets.inc) | 0 | 6 | `data/gt-lrc-offsets.inc` | `c8b1d148a67c79c28f4e76b819977c3871170cb05e1a8df878dba3dac4015991` |
| [parity/gt_mem.c](../../src/drivers/gpu/i915/parity/gt_mem.c) | 39 | 1 | `ggtt.c`<br>`memory.c`<br>`ppgtt.c` | `1637686e79eb2ec46f593fbc3094c61fb310378882d1f69cbd0f73097e2db3fb` |
| [parity/gt_mem.h](../../src/drivers/gpu/i915/parity/gt_mem.h) | 0 | 18 | `ggtt.h` / `memory.h` / `ppgtt.h` | `c9ea6344132297778714fc68f8a5b3cdc777d6fb5fed41309c30be95fa53d67b` |
| [parity/gt_migrate.c](../../src/drivers/gpu/i915/parity/gt_migrate.c) | 5 | 0 | `engine.c` | `3bc77c9e87821cef38df74399884f404a21635d3fd14bdc2f83873877a21cc71` |
| [parity/gt_migrate.h](../../src/drivers/gpu/i915/parity/gt_migrate.h) | 0 | 4 | `engine.h` | `9455472f5dd8b69282ec2717bed686b1a9eb5be20390abc5cf6454894d6a70a5` |
| [parity/gt_mmio.c](../../src/drivers/gpu/i915/parity/gt_mmio.c) | 13 | 44 | `mmio.c` | `e133e761e303fb263f2e260467103206e70d3ec786f60c69ab286efb08fb5550` |
| [parity/gt_mmio.h](../../src/drivers/gpu/i915/parity/gt_mmio.h) | 0 | 15 | `mmio.h` | `34a5fc91fe252a58d2520ae2956678439248463eceb126f5a382dcebebe5b289` |
| [parity/gt_request.c](../../src/drivers/gpu/i915/parity/gt_request.c) | 12 | 0 | `request.c` | `54e2c0b23017c15c4082bfb247f5a428f157f3ce329c8ebd0c72f360538a87ec` |
| [parity/gt_request.h](../../src/drivers/gpu/i915/parity/gt_request.h) | 0 | 39 | `request.h` | `288eefa3c206b85b3a73ffa13766899284b2fe7650ddc23465ec168b4ed7acb9` |
| [parity/gt_resume.c](../../src/drivers/gpu/i915/parity/gt_resume.c) | 4 | 0 | `engine.c` | `3475c3a36390249639aec750b300c7f29ef8b88f95f8315edeeb28f5b89c91d3` |
| [parity/gt_resume.h](../../src/drivers/gpu/i915/parity/gt_resume.h) | 0 | 1 | `engine.h` | `025e8489e8b3e27fd6c5f4f942728627ce624f13fb5ec21eaa7cce2442eda6b5` |
| [parity/gt_submit.c](../../src/drivers/gpu/i915/parity/gt_submit.c) | 10 | 1 | `request.c` | `b063f896ee0f6d23172611ccb4d14d0e9269e08cd09ff34b974b33747e848979` |
| [parity/gt_submit.h](../../src/drivers/gpu/i915/parity/gt_submit.h) | 0 | 7 | `request.h` | `5f5ac48da93699c2ef86ef8d610a8e0d1a89a562fb2c0ed1ab4dd73e7690718f` |
| [parity/gt_tlb.c](../../src/drivers/gpu/i915/parity/gt_tlb.c) | 2 | 9 | `ppgtt.c` | `2b6536efd316e20a0dccc6e874bacc101f20702af83d3f557e94e69d2e7af474` |
| [parity/gt_tlb.h](../../src/drivers/gpu/i915/parity/gt_tlb.h) | 0 | 1 | `ppgtt.h` | `0e0e251f25d8cc1e434b317f87759701e775a52b293a3fec7e7460fc989bbe5a` |
| [parity/gt_verify_wa.c](../../src/drivers/gpu/i915/parity/gt_verify_wa.c) | 11 | 0 | `workarounds.c` | `23973cac42f3cb32806c4c6595c5b7cc7aad595d180f0acdb5efc2e8588168ee` |
| [parity/gt_verify_wa.h](../../src/drivers/gpu/i915/parity/gt_verify_wa.h) | 0 | 9 | `workarounds.h` | `10eeb6f2d86be8a4b04bb7417a81a16495ada556eb83c6ca7c65ff7b6d36f186` |
| [parity/gt_wa_adlp.c](../../src/drivers/gpu/i915/parity/gt_wa_adlp.c) | 8 | 50 | `workarounds.c` | `4bdfff840b5753ed749eb903a8ebb80788b43a03cc4955e582b6d56380a5f93c` |
| [parity/irq.c](../../src/drivers/gpu/i915/parity/irq.c) | 46 | 109 | `irq.c` | `d52f76c3a3ab32f39a47839465ebf470a2e36e309132a672c9cccfee1b8f204f` |
| [parity/irq.h](../../src/drivers/gpu/i915/parity/irq.h) | 0 | 2 | `irq.h` | `aecf73017cddc650b3858505c1b8a40c17aecfd446bb984c3dc7623ddf0ff0c6` |
| [parity/ktest.c](../../src/drivers/gpu/i915/parity/ktest.c) | 56 | 5 | `tests/execution/ktest.c` | `996ce3ff126e89687854adf654b1e88ad34299846b9e2026838243eefc459a07` |
| [parity/ktest.h](../../src/drivers/gpu/i915/parity/ktest.h) | 0 | 1 | `tests/fixtures/ktest.h` | `93bcf9bea52aac8eb766cf12d88cc31ff0f5d58f6c02c97347316ff55acd4393` |
| [parity/lcd/drm_connector_status_port.c](../../src/drivers/gpu/i915/parity/lcd/drm_connector_status_port.c) | 1 | 0 | `display/hotplug.c` | `f0c0c8d8d59ab115562e39645c0888f5b1d1255c5b5e27a5c240978ed292e274` |
| [parity/lcd/drm_dp_bw_port.c](../../src/drivers/gpu/i915/parity/lcd/drm_dp_bw_port.c) | 2 | 0 | `display/dp.c` | `7433ee39abf6e977bf583b9c6f91b20698ed2f497f87c66ba84b92dcc82af7fa` |
| [parity/lcd/drm_dp_link_port.c](../../src/drivers/gpu/i915/parity/lcd/drm_dp_link_port.c) | 24 | 0 | `display/dp.c` | `3403c9d12e651b5633f73638d62c2b551f27f95283578cdd9cfeb326cd5dd950` |
| [parity/lcd/drm_edid_mode_port.c](../../src/drivers/gpu/i915/parity/lcd/drm_edid_mode_port.c) | 2 | 13 | `display/edid.c` | `71681a7207b3a5be6c943e246a037e66dc54e4fe15e058e39ddf0188743e1713` |
| [parity/lcd/drm_modes_hv_port.c](../../src/drivers/gpu/i915/parity/lcd/drm_modes_hv_port.c) | 3 | 0 | `display/edid.c` | `d26a1d3a9f80e1fd4585987ec36777caa2c8732ad58cfff3ee28045c27027abe` |
| [parity/lcd/drm_modes_port.c](../../src/drivers/gpu/i915/parity/lcd/drm_modes_port.c) | 1 | 0 | `display/edid.c` | `54abc11445b8c8cb82b897e0786d01b80c622b13cba2485458ce1fbb473871fd` |
| [parity/lcd/drm_probe_detect_port.c](../../src/drivers/gpu/i915/parity/lcd/drm_probe_detect_port.c) | 2 | 0 | `display/hotplug.c` | `16f0933380f3c9db4eaccd3ff8216a70229fbe129475e360e40c3ea1328fc425` |
| [parity/lcd/edid_ref_types.h](../../src/drivers/gpu/i915/parity/lcd/edid_ref_types.h) | 0 | 79 | `display/internal.h（型/inlineを所有headerへ再配分）` | `6914403ab60d4dce990ef3c6007b0176b7785428cb7801aef76572d8842cfeab` |
| [parity/lcd/hpd_compat.h](../../src/drivers/gpu/i915/parity/lcd/hpd_compat.h) | 8 | 116 | `display/hotplug-internal.h`（macroの意味を保存し明示loopへ置換） | `26aef150d34e644d650eaadeaeda79f90256c3b88dd5a2052bfd674280b750d2` |
| [parity/lcd/hpd_drm_connector_status.h](../../src/drivers/gpu/i915/parity/lcd/hpd_drm_connector_status.h) | 0 | 4 | `display/internal.h（型/inlineを所有headerへ再配分）` | `8576184484bfb0f1821f44879a9bc39a1e515e4859a006ce4ce78a5c308f75fe` |
| [parity/lcd/hpd_for_each_pin.h](../../src/drivers/gpu/i915/parity/lcd/hpd_for_each_pin.h) | 0 | 2 | `data/display-hpd-for-each-pin.inc` | `855d75fab14d56c8971ee1bd1c89f090febbefe172e040893c90a0c4b77006e2` |
| [parity/lcd/hpd_hotplug_state.h](../../src/drivers/gpu/i915/parity/lcd/hpd_hotplug_state.h) | 0 | 1 | `display/internal.h（型/inlineを所有headerへ再配分）` | `cf0224ac879c72b6963538ac4c8d86d61d200efa9e46a04d40bf7c4cd149b929` |
| [parity/lcd/hpd_hotplug_types.h](../../src/drivers/gpu/i915/parity/lcd/hpd_hotplug_types.h) | 0 | 1 | `display/internal.h（型/inlineを所有headerへ再配分）` | `52d4608e903a10c2e2740db192cd793b6f8aca195895b02b1631caa32f998b28` |
| [parity/lcd/hpd_ktest.c](../../src/drivers/gpu/i915/parity/lcd/hpd_ktest.c) | 5 | 0 | `tests/display/hpd-ktest.c` | `1cb392995006812151ae04ca02dd1b9578ac582eefb30101850a474ef478e4ac` |
| [parity/lcd/hpd_mreg_drm_dp.h](../../src/drivers/gpu/i915/parity/lcd/hpd_mreg_drm_dp.h) | 0 | 2 | `data/display-hpd-mreg-drm-dp.inc` | `8fba74625e5309f81ec04a5db1a87925f908a4b1bb1906323a54414fe9a02e3e` |
| [parity/lcd/hpd_mreg_gmbus.h](../../src/drivers/gpu/i915/parity/lcd/hpd_mreg_gmbus.h) | 0 | 30 | `data/display-hpd-mreg-gmbus.inc` | `02bdae392aee177019cfd015837c240fc1ed535174a32100cac50e4345e74cc1` |
| [parity/lcd/hpd_mreg_gmbus_pins.h](../../src/drivers/gpu/i915/parity/lcd/hpd_mreg_gmbus_pins.h) | 0 | 3 | `data/display-hpd-mreg-gmbus-pins.inc` | `65b6d5696a5cdbb7c487e7a5f577d6ca61f5ae1548643b1752eaae62a0a58e9e` |
| [parity/lcd/hpd_mreg_i915_reg.h](../../src/drivers/gpu/i915/parity/lcd/hpd_mreg_i915_reg.h) | 0 | 13 | `data/display-hpd-mreg-i915-reg.inc` | `54e554e869262f8bb6ef6005d84178e73a7cf4fb93a96b839a1fe2c36ea3960e` |
| [parity/lcd/hpd_pin_enum.h](../../src/drivers/gpu/i915/parity/lcd/hpd_pin_enum.h) | 0 | 1 | `data/display-hpd-pin-enum.inc` | `7511532e5d63ae826f965b464f7ac2d7424900fcd1341d87e388adfcf75d3c62` |
| [parity/lcd/intel_acpi_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_acpi_port.c) | 2 | 18 | `display/panel.c` | `99f2b09a3e91358093f31d8c878b490804664459fa5832156f57f28e6b8c40b7` |
| [parity/lcd/intel_atomic_plane_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_atomic_plane_port.c) | 5 | 0 | `display/plane.c` | `0376de7413a2303eddee79e70206446a9d1fdd1d63d183b458726046519a4d34` |
| [parity/lcd/intel_backlight_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_backlight_port.c) | 30 | 0 | `display/panel.c` | `c07050890b1431c9bd5999fbb0f8be2feed09ea4a9e1b550d02ae3d1a3d02dff` |
| [parity/lcd/intel_bw_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_bw_port.c) | 4 | 0 | `display/watermark.c` | `4b042ec0b256ddd8312a3b41800e7c283342fcfa7fc8b48d383982ac5d25988a` |
| [parity/lcd/intel_cdclk_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_cdclk_port.c) | 7 | 0 | `display/clock.c` | `2265a98de680d2c401052440952adc4209c6abfd3d78657835b2cd9cb70e31c7` |
| [parity/lcd/intel_color_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_color_port.c) | 10 | 0 | `display/color.c` | `2957f064c0c001f16d341bc20dca92bec04fa3f83eb417cfd52f9a9728107d4c` |
| [parity/lcd/intel_combo_phy_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_combo_phy_port.c) | 1 | 0 | `display/phy.c` | `fd82ce387a760890fca9de30f8c6011eede21d3d2bf530faf087498ef20e5260` |
| [parity/lcd/intel_crtc_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_crtc_port.c) | 9 | 0 | `display/pipe.c` | `bf1e7c997b81cc63c3dcfd5bdb4447db95a33511d7b71dad1f57505b8cf4bf25` |
| [parity/lcd/intel_ddi_buf_trans_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_ddi_buf_trans_port.c) | 10 | 0 | `display/phy.c` | `c921061f2e85e88db5ca7637c3257938187611e26d5c15edb7da22ae5020c94a` |
| [parity/lcd/intel_ddi_hotplug_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_ddi_hotplug_port.c) | 2 | 0 | `display/hotplug.c` | `d2754ed56661c41b70a8a8136668d4a540a696630d08226357d7f14fed167d62` |
| [parity/lcd/intel_ddi_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_ddi_port.c) | 77 | 0 | `display/ddi.c` | `405c1e5581ea7b9bc5e0fad837e7f595af68aa113ab21ee533abcb0dda2550d8` |
| [parity/lcd/intel_display_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_display_port.c) | 56 | 0 | `display/pipe.c` | `9249d2f8c22ae3005d9aee838ea8e2c46ce6d7e97e334871b37552a3585769ce` |
| [parity/lcd/intel_display_power_set_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_display_power_set_port.c) | 2 | 0 | `display/power.c` | `e3459aea0a5853848c4bb870507657424ee03c42682669b0400a3f4289c1d098` |
| [parity/lcd/intel_dmc_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_dmc_port.c) | 3 | 0 | `display/dmc.c` | `dfb75349b30648abc8568305939af05f5b373719d47022b99e9d901be368af0a` |
| [parity/lcd/intel_dp_connected_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_dp_connected_port.c) | 1 | 0 | `display/dp.c` | `c4a277466ef7a21389f4d9cf826f3297e733399be18b1841de155b7770e66328` |
| [parity/lcd/intel_dp_link_training_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_dp_link_training_port.c) | 45 | 0 | `display/dp.c` | `0f5ac94db019ad0458408343ec6dcf2e2279a4ef5b09ab5cd75a1389be47a767` |
| [parity/lcd/intel_dpll_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_dpll_port.c) | 35 | 0 | `display/clock.c` | `2071c6afeca1ee35c41d82dec74d85403d31ade816787edd2ccfcc7008b65655` |
| [parity/lcd/intel_gmbus_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_gmbus_port.c) | 17 | 2 | `display/gmbus.c` | `1bde3e65c720833eeaa32e7f44045d99c882947309673099b66a4ace7a9e0e05` |
| [parity/lcd/intel_hdmi_detect_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_hdmi_detect_port.c) | 3 | 0 | `display/hdmi.c` | `53041468b6342579bbdea7be6a610a70fef95555d3340fcfc5b7df6a6c66115b` |
| [parity/lcd/intel_hdmi_mode_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_hdmi_mode_port.c) | 4 | 0 | `display/hdmi.c` | `ad16d772021a25bec71a6195965462d7a2485668da6a23915921ca2ea6c008e5` |
| [parity/lcd/intel_hotplug_irq_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_hotplug_irq_port.c) | 4 | 0 | `display/hotplug.c` | `81946a8bd4db210fce22062cb3ca5186b44e4523a132dde99b4fbcb3d02c64e2` |
| [parity/lcd/intel_hotplug_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_hotplug_port.c) | 12 | 4 | `display/hotplug.c` | `e4d2d466f6a43e3796cd97e69caa77b7224abe464af9b0984ac0294c5e70f3a6` |
| [parity/lcd/intel_link_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_link_port.c) | 20 | 0 | `display/dp.c` | `81d827ba21a06a62920a60230f223425cc4516cb77466fbb1f594199719eaae8` |
| [parity/lcd/intel_modeset_setup_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_modeset_setup_port.c) | 25 | 0 | `display/takeover.c` | `e6c7d6d26616abfdba96bda9561fb3bc7b6a92c2b06aa1646e32d33fe4d3a83c` |
| [parity/lcd/intel_opregion_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_opregion_port.c) | 28 | 100 | `display/opregion.c` | `08a15b2fda8ceff9dce0519c4e1f293a28c74d871977e383ddbbb9e6fafca179` |
| [parity/lcd/intel_vblank_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_vblank_port.c) | 9 | 0 | `display/vblank.c` | `123785b5518362faf9a2a093110a2c7cfee0b58a1fa51d93b09860d4a2021d63` |
| [parity/lcd/intel_vrr_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_vrr_port.c) | 2 | 0 | `display/pipe.c` | `70b40610d688b3b8498af5908d2f31c53ca8c0aa3fe109d7ab09fcb0caf88ac6` |
| [parity/lcd/intel_wm_port.c](../../src/drivers/gpu/i915/parity/lcd/intel_wm_port.c) | 1 | 0 | `display/watermark.c` | `f1351fff7d1f9b417a2c4023bf18ca8b5fac08e051930c61e574ddbfca154e03` |
| [parity/lcd/lcd_buf_trans_types.h](../../src/drivers/gpu/i915/parity/lcd/lcd_buf_trans_types.h) | 0 | 1 | `display/internal.h（型/inlineを所有headerへ再配分）` | `1a3a02a88a8b666e55e8c3534944d32810e4a718bba0241a1d9fe7063b08c5de` |
| [parity/lcd/lcd_compat.h](../../src/drivers/gpu/i915/parity/lcd/lcd_compat.h) | 14 | 92 | `display/modeset-internal.h`（macroの意味を保存し明示loopへ置換） | `cf93bd187b8f1f20d704af2e6b1d9cf7ab660484bd83c5fb99b4679307c92a9f` |
| [parity/lcd/lcd_dbuf_slice_enum.h](../../src/drivers/gpu/i915/parity/lcd/lcd_dbuf_slice_enum.h) | 0 | 1 | `data/display-lcd-dbuf-slice-enum.inc` | `8910c3dff49cf9e1d806cb5c42bccc3e89328360bd8ddc7363f572fc83315e01` |
| [parity/lcd/lcd_dbuf_types.h](../../src/drivers/gpu/i915/parity/lcd/lcd_dbuf_types.h) | 0 | 1 | `display/internal.h（型/inlineを所有headerへ再配分）` | `0a998a825d840d29a8aeae85ab0c8b689f72ae118836fef8bd14fae58ca3b6c9` |
| [parity/lcd/lcd_ddi_regs.h](../../src/drivers/gpu/i915/parity/lcd/lcd_ddi_regs.h) | 0 | 231 | `data/display-lcd-ddi-regs.inc` | `5f4eb3401349de81bb1d9bda952d2d0bdd5e2012a8fbe217f2cf8e9b9d053f94` |
| [parity/lcd/lcd_ddi_types.h](../../src/drivers/gpu/i915/parity/lcd/lcd_ddi_types.h) | 0 | 1 | `display/internal.h（型/inlineを所有headerへ再配分）` | `7a0e8c3865818a057c7015f92db5e5dc2196f39cb7d22caf9157b0ebb0ef951f` |
| [parity/lcd/lcd_dp_compat.h](../../src/drivers/gpu/i915/parity/lcd/lcd_dp_compat.h) | 5 | 25 | `display/dp.c` | `c9fdca799abb222a2aaa343706d484f808ce2ee67ec3d6bd6ecfbc3d22c7de9c` |
| [parity/lcd/lcd_dp_helper_inlines.h](../../src/drivers/gpu/i915/parity/lcd/lcd_dp_helper_inlines.h) | 3 | 1 | `display/dp.c` | `003e9274c4b1f38f016579e716f21dbcfa8a75f2ebe751af170e1f8376390083` |
| [parity/lcd/lcd_dp_msa.h](../../src/drivers/gpu/i915/parity/lcd/lcd_dp_msa.h) | 0 | 37 | `data/display-lcd-dp-msa.inc` | `67cbfae4cd30feeadfce7f69165a010514cad6c63c57c6605bc50aecd2e3552d` |
| [parity/lcd/lcd_dp_phy_enum.h](../../src/drivers/gpu/i915/parity/lcd/lcd_dp_phy_enum.h) | 0 | 1 | `data/display-lcd-dp-phy-enum.inc` | `e6377c52df03c582d69d1806880943cbd920ef8ff147b39cbe64279135f55727` |
| [parity/lcd/lcd_dpll_id_enum.h](../../src/drivers/gpu/i915/parity/lcd/lcd_dpll_id_enum.h) | 0 | 1 | `data/display-lcd-dpll-id-enum.inc` | `3d9ffc3915269ae1df69153679ec88b45bef73b74997dadc02d13b17379b7512` |
| [parity/lcd/lcd_drm_colorspace.h](../../src/drivers/gpu/i915/parity/lcd/lcd_drm_colorspace.h) | 0 | 1 | `display/internal.h（型/inlineを所有headerへ再配分）` | `31b7a85aa9b1b18b0a96660a18976af04bc2c8803ece90ba05e65cfdc49a9db1` |
| [parity/lcd/lcd_drm_fourcc.h](../../src/drivers/gpu/i915/parity/lcd/lcd_drm_fourcc.h) | 1 | 286 | `display/state.c` | `648daf8a9b7ccc38b8c22a1fae4b56cae3b7c1d3203b87ef63dad7b3ca8bba9f` |
| [parity/lcd/lcd_drm_plane_defs.h](../../src/drivers/gpu/i915/parity/lcd/lcd_drm_plane_defs.h) | 1 | 12 | `display/plane.c` | `b269cfd9287ba0e47e3715e3a45e8946bf23599b01098ed4627b1a13022a9da5` |
| [parity/lcd/lcd_fake_hw.c](../../src/drivers/gpu/i915/parity/lcd/lcd_fake_hw.c) | 36 | 33 | `tests/display/lcd-fake-hw.c` | `0ceba01737b5d1a02aa912609ec18e7a18c6bd50157c7275108af6ee68e40f4e` |
| [parity/lcd/lcd_fake_hw.h](../../src/drivers/gpu/i915/parity/lcd/lcd_fake_hw.h) | 0 | 3 | `tests/display/lcd-fake-hw.h` | `d8c52e3d18b8b50424fb5ca4749ce0144033d1e57605b7cbef0d9bd18c649254` |
| [parity/lcd/lcd_flip_compat.h](../../src/drivers/gpu/i915/parity/lcd/lcd_flip_compat.h) | 0 | 49 | `display/internal.h（型/inlineを所有headerへ再配分）` | `1c1881f2a938968b11196f2ff8f54fe83cdca3174dc6b0996b096d0df129fee3` |
| [parity/lcd/lcd_hw_check.c](../../src/drivers/gpu/i915/parity/lcd/lcd_hw_check.c) | 1 | 1 | `tests/display/scanout-hw-check.c` | `ea42bfb09abf110dd0d3323b8f359b018ad122d75513e75f3d9fd6195332a9ea` |
| [parity/lcd/lcd_hw_check.h](../../src/drivers/gpu/i915/parity/lcd/lcd_hw_check.h) | 0 | 1 | `tests/display/scanout-hw-check.h` | `1153938134826aff52b691503a28f69b69dbf745659dd48f245dda85ddd615f1` |
| [parity/lcd/lcd_i915_colorkey.h](../../src/drivers/gpu/i915/parity/lcd/lcd_i915_colorkey.h) | 0 | 3 | `display/internal.h（型/inlineを所有headerへ再配分）` | `54b6da04e9911fd6c3a59e69ff13b573524e975b49b96edc597f2ce164624ad3` |
| [parity/lcd/lcd_i915_fixed.h](../../src/drivers/gpu/i915/parity/lcd/lcd_i915_fixed.h) | 15 | 2 | `display/internal.h` | `c67d7f2f50220150ad1e71e2bfce3fbaa251e1ed3817383d5eb8806cd8ec4f01` |
| [parity/lcd/lcd_link_training_inlines.h](../../src/drivers/gpu/i915/parity/lcd/lcd_link_training_inlines.h) | 1 | 1 | `display/dp.c` | `43af3f4ced4ed20867b0b92d23a7652aebf9df243bb981500f994032edabca04` |
| [parity/lcd/lcd_modeset_compat.h](../../src/drivers/gpu/i915/parity/lcd/lcd_modeset_compat.h) | 2 | 94 | `display/modeset-internal.h`（macroの意味を保存し明示loopへ置換） | `91b2efdbb689d6ba43a1614147e21a934d622b6266f84ba69920c5a22713584d` |
| [parity/lcd/lcd_modeset_ktest.c](../../src/drivers/gpu/i915/parity/lcd/lcd_modeset_ktest.c) | 3 | 0 | `tests/display/lcd-modeset-ktest.c` | `5444606c2715a45b4f58aa6572e2737edc74e04def5b0a6d1076982d05e0a3db` |
| [parity/lcd/lcd_modeset_ktest.h](../../src/drivers/gpu/i915/parity/lcd/lcd_modeset_ktest.h) | 0 | 1 | `tests/display/lcd-modeset-ktest.h` | `51330e80ff193650edfb1058b031b2e37930437f63eb8b4ce13bdf6eaf43aeba` |
| [parity/lcd/lcd_mreg_backlight.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_backlight.h) | 0 | 12 | `data/display-lcd-mreg-backlight.inc` | `2b22e9202d2fdead34b7d82b866cfaebb2313644e1a25b425018967f34bb3bec` |
| [parity/lcd/lcd_mreg_color.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_color.h) | 0 | 18 | `data/display-lcd-mreg-color.inc` | `0dc03234e46f66c90f45c4e0605883ac1ff2f8e6f5c7f48ac720bb6c0a2263a3` |
| [parity/lcd/lcd_mreg_combo_phy.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_combo_phy.h) | 0 | 60 | `data/display-lcd-mreg-combo-phy.inc` | `0665167d334695c94bcf1df30bbac1d10e1b05dd23119931e3c36543be40588b` |
| [parity/lcd/lcd_mreg_cx0.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_cx0.h) | 0 | 8 | `data/display-lcd-mreg-cx0.inc` | `d19086952aad2249bf976f9833a5206179ee791ed80931eef7a8462189e079bf` |
| [parity/lcd/lcd_mreg_display.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_display.h) | 0 | 4 | `data/display-lcd-mreg-display.inc` | `572c2cda50498056eef5fea4af9006d14f3e909a539a198f599b45dc4f699f0d` |
| [parity/lcd/lcd_mreg_display_device.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_display_device.h) | 0 | 5 | `data/display-lcd-mreg-display-device.inc` | `8f0e2c33792265b04c281575d6e2e005033e490a8a5487b892ee8a9753922b72` |
| [parity/lcd/lcd_mreg_display_reg_defs.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_display_reg_defs.h) | 0 | 5 | `data/display-lcd-mreg-display-reg-defs.inc` | `616bc3f9ffdd4ad2cc89428e5879d74c78bf0137eb1d4f9e8bfd2f3eb3b0e3ec` |
| [parity/lcd/lcd_mreg_display_types.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_display_types.h) | 0 | 3 | `data/display-lcd-mreg-display-types.inc` | `7bfcd84e44d2cbc83fcd2058314180fc83c9968a69b637026f8c552fafc31f0a` |
| [parity/lcd/lcd_mreg_dmc.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_dmc.h) | 0 | 7 | `data/display-lcd-mreg-dmc.inc` | `1c0c9f48546b0aedabdcbcef60e792968def3050bb21489452d8df84b8c347e7` |
| [parity/lcd/lcd_mreg_dmc_c.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_dmc_c.h) | 0 | 2 | `data/display-lcd-mreg-dmc-c.inc` | `203fea11ac6d788fef20eb5cbdd500076d18fa0381d06721114b5afc5e28bb1c` |
| [parity/lcd/lcd_mreg_drm_dp.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_drm_dp.h) | 0 | 103 | `data/display-lcd-mreg-drm-dp.inc` | `441105b4ec8e6cdf5036a8e67a67be248e0f88fa2094c85c57ba3cd0c30d34b1` |
| [parity/lcd/lcd_mreg_hdmi_dip.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_hdmi_dip.h) | 0 | 8 | `data/display-lcd-mreg-hdmi-dip.inc` | `995863907f1e3d59d8f820dca00456c76b599a8f528824de3999f6c88f0ba3d3` |
| [parity/lcd/lcd_mreg_i915_reg.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_i915_reg.h) | 0 | 169 | `data/display-lcd-mreg-i915-reg.inc` | `b4a447fa8250f8d5445de0959696f63c2a8f986445c50bbe089e0df78eb60b1f` |
| [parity/lcd/lcd_mreg_link_training.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_link_training.h) | 0 | 15 | `data/display-lcd-mreg-link-training.inc` | `52811588233fba45de1fd59149fcce0e281b08913a2a9d57cc48c80f4a539b03` |
| [parity/lcd/lcd_mreg_power.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_power.h) | 0 | 4 | `data/display-lcd-mreg-power.inc` | `c9c505343bc97374ad7524ef918793b99b5f01bffa9045cbbefdb5d004d1f4bf` |
| [parity/lcd/lcd_mreg_reg_defs.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_reg_defs.h) | 0 | 2 | `data/display-lcd-mreg-reg-defs.inc` | `309c848d89c76a082ac3d016e1982f37d6c15d3263e347460886215396c1450a` |
| [parity/lcd/lcd_mreg_vdsc.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_vdsc.h) | 0 | 10 | `data/display-lcd-mreg-vdsc.inc` | `3183b4617292eec1ceaadd34f35547b30b446ff126d3293eabd596ff2ed83607` |
| [parity/lcd/lcd_mreg_wm.h](../../src/drivers/gpu/i915/parity/lcd/lcd_mreg_wm.h) | 0 | 93 | `data/display-lcd-mreg-wm.inc` | `3f9fbe3f74a35707cef6019ebbc91f1c30b1fe5ce728d2efc865186ec45e5fd4` |
| [parity/lcd/lcd_pattern.c](../../src/drivers/gpu/i915/parity/lcd/lcd_pattern.c) | 5 | 6 | `tests/display/lcd-pattern.c` | `95258aeeac38ffd3ce3468c28f5184f89765cd602b3424be77f1b8afbe4981be` |
| [parity/lcd/lcd_pattern.h](../../src/drivers/gpu/i915/parity/lcd/lcd_pattern.h) | 0 | 1 | `tests/display/lcd-pattern.h` | `b605b2279589dd330a0110d1844bbcb373b938c69c593ce0c413a325ac41b13b` |
| [parity/lcd/lcd_pch_enum.h](../../src/drivers/gpu/i915/parity/lcd/lcd_pch_enum.h) | 0 | 1 | `data/display-lcd-pch-enum.inc` | `b7e3d10d82eda018f38a3d4ca662650cf933a20c90950b135f33a326661bf0b8` |
| [parity/lcd/lcd_plane_compat.h](../../src/drivers/gpu/i915/parity/lcd/lcd_plane_compat.h) | 0 | 21 | `display/internal.h（型/inlineを所有headerへ再配分）` | `36d87e1873704faf1649b8e7940dbf35bc45d2885aa6719c4e89b0411516b33c` |
| [parity/lcd/lcd_plane_regs.h](../../src/drivers/gpu/i915/parity/lcd/lcd_plane_regs.h) | 0 | 273 | `data/display-lcd-plane-regs.inc` | `04fb1f4ca137e969bce518cab7cae0b85b2a49baf69ed1b5b708e863660e72ec` |
| [parity/lcd/lcd_plane_types.h](../../src/drivers/gpu/i915/parity/lcd/lcd_plane_types.h) | 0 | 1 | `display/internal.h（型/inlineを所有headerへ再配分）` | `71da31d1361999ee1115343c3c795fc1b96cfb36dfd2566108561b9e7c79adae` |
| [parity/lcd/lcd_power_domain_enum.h](../../src/drivers/gpu/i915/parity/lcd/lcd_power_domain_enum.h) | 0 | 1 | `data/display-lcd-power-domain-enum.inc` | `aae8d5a4500bc8912916bf0e49a0556745fec153dbd2d30c0c94249d49ef616d` |
| [parity/lcd/lcd_power_domain_set_types.h](../../src/drivers/gpu/i915/parity/lcd/lcd_power_domain_set_types.h) | 0 | 2 | `display/internal.h（型/inlineを所有headerへ再配分）` | `134085c15a2fcb283ea394f864d15683b688145e0cf54deaa2b2cefa68864c94` |
| [parity/lcd/lcd_psr_selfetch_regs.h](../../src/drivers/gpu/i915/parity/lcd/lcd_psr_selfetch_regs.h) | 0 | 22 | `data/display-lcd-psr-selfetch-regs.inc` | `eecfe427e07f4ce9af19390fd3913f1dcf99a2e6829b705c6138c38cd8b460f0` |
| [parity/lcd/lcd_ref_inlines.h](../../src/drivers/gpu/i915/parity/lcd/lcd_ref_inlines.h) | 4 | 1 | `display/internal.h` | `ed36c370b1a091496f94836c267581f6e4778e204694173e34ecf62a9b69c133` |
| [parity/lcd/lcd_ref_types.h](../../src/drivers/gpu/i915/parity/lcd/lcd_ref_types.h) | 0 | 14 | `display/internal.h（型/inlineを所有headerへ再配分）` | `7d3a3e8fb9ad10bf3c56e5ef280b61217899e8c84a2d2076b714fc4aee924252` |
| [parity/lcd/lcd_seq_compat.h](../../src/drivers/gpu/i915/parity/lcd/lcd_seq_compat.h) | 0 | 85 | `display/internal.h（型/inlineを所有headerへ再配分）` | `ad8e3a1c041be4a0f8cf225f32e7d9d0bca40bf055029a3c943da47e910d5a06` |
| [parity/lcd/lcd_show_ktest.c](../../src/drivers/gpu/i915/parity/lcd/lcd_show_ktest.c) | 7 | 4 | `tests/display/lcd-show-ktest.c` | `8035340d310e504d1b428245c76326b2260cecef07957e7b9141349aa0d6acdc` |
| [parity/lcd/lcd_show_ktest.h](../../src/drivers/gpu/i915/parity/lcd/lcd_show_ktest.h) | 0 | 1 | `tests/display/lcd-show-ktest.h` | `f5a31c0a432d3b37f536042b7debe3a5f86bbeef22dfe002bc4d71055c8f9d14` |
| [parity/lcd/lcd_trans_regs.h](../../src/drivers/gpu/i915/parity/lcd/lcd_trans_regs.h) | 0 | 101 | `data/display-lcd-trans-regs.inc` | `d3f417aada15c03d39f1edaf743759cf630d743d48a1d0e06d6ab3bf87811778` |
| [parity/lcd/lcd_wm_compat.h](../../src/drivers/gpu/i915/parity/lcd/lcd_wm_compat.h) | 1 | 33 | `display/watermark-internal.h`（macroの意味を保存し明示loopへ置換） | `9dd3dfd64d0cd04c211e5dd5d09cd40e54d48d9ad593681b0718d227117abdc1` |
| [parity/lcd/lcd_wm_ddb_types.h](../../src/drivers/gpu/i915/parity/lcd/lcd_wm_ddb_types.h) | 2 | 1 | `display/watermark.c` | `cefa6e118511166ef549fdf2bbcbd5507000a7013111ca167c0831efeca97fc5` |
| [parity/lcd/lcd_wm_types.h](../../src/drivers/gpu/i915/parity/lcd/lcd_wm_types.h) | 0 | 1 | `display/internal.h（型/inlineを所有headerへ再配分）` | `b0bbdbdf9f74f0c03465a9bd90fbfb2c17927690709d06bac9120a01ef7bc00b` |
| [parity/lcd/lcdg_ktest.c](../../src/drivers/gpu/i915/parity/lcd/lcdg_ktest.c) | 8 | 2 | `tests/display/lcdg-ktest.c` | `f1892c9e391a31a0e4fbab49605fdf03d60ef48789270129e9dbf8ec76ec9f98` |
| [parity/lcd/lcdg_ktest.h](../../src/drivers/gpu/i915/parity/lcd/lcdg_ktest.h) | 0 | 1 | `tests/display/lcdg-ktest.h` | `35a2ef28d55dafd11c35aa3b8ed7f0b9470758f5ea172480e4dfcca8b0cae31b` |
| [parity/lcd/n1_compat.h](../../src/drivers/gpu/i915/parity/lcd/n1_compat.h) | 4 | 125 | `display/takeover-internal.h`（macroの意味を保存し明示loopへ置換） | `5cd04f358e00ca98031800991123ca03e6f92e0ee04cbe522d7becc59590149a` |
| [parity/lcd/opreg_pci_config.h](../../src/drivers/gpu/i915/parity/lcd/opreg_pci_config.h) | 0 | 6 | `display/internal.h（型/inlineを所有headerへ再配分）` | `0b684695693e7c6692372fc84436384f61d30424f58b1f60d04f6aff5f7662e0` |
| [parity/lcd/opreg_struct.h](../../src/drivers/gpu/i915/parity/lcd/opreg_struct.h) | 0 | 2 | `display/internal.h（型/inlineを所有headerへ再配分）` | `53022defa93c098ff7b0a18bf9d8e0168aaf8a9ce5cdfd16292116ee140c244c` |
| [parity/lcd/opregion_compat.h](../../src/drivers/gpu/i915/parity/lcd/opregion_compat.h) | 2 | 74 | `display/opregion-internal.h`（macroの意味を保存し明示loopへ置換） | `d60271e342f6879bc7d8e100071d4173b72b7547b93168a32bfa8ad2bdd50921` |
| [parity/lcd/opregion_fwtest.c](../../src/drivers/gpu/i915/parity/lcd/opregion_fwtest.c) | 4 | 4 | `tests/display/opregion-fwtest.c` | `3ae3a9b09dcd23d39857380948d1db8c14eb08819d8ce7ab42cf302382c50c99` |
| [parity/lcd/opregion_fwtest.h](../../src/drivers/gpu/i915/parity/lcd/opregion_fwtest.h) | 0 | 1 | `tests/display/opregion-fwtest.h` | `a7488cf24d04dbdc6d5be9375c2d3410e612487cf1b6f00110a80cbb3d05fd89` |
| [parity/lcd/opregion_ktest.c](../../src/drivers/gpu/i915/parity/lcd/opregion_ktest.c) | 18 | 23 | `tests/display/opregion-ktest.c` | `efe1cbebd3b80981afc1e9021f874705547a77020218c5f9653dfa20727c103c` |
| [parity/lcd/opregion_ktest.h](../../src/drivers/gpu/i915/parity/lcd/opregion_ktest.h) | 0 | 1 | `tests/display/opregion-ktest.h` | `7ecb00194b13151a471679d376e870d90a376d42f524f2e75d29b79eddb87fd6` |
| [parity/lcd/parity_acpi_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_acpi_glue.inc) | 0 | 0 | `data/parity-acpi-glue.inc` | `144df508063ac0e32f0057fb623d7b534d1fe349059ef18729bbdc64423ac853` |
| [parity/lcd/parity_atomic_plane_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_atomic_plane_glue.inc) | 1 | 0 | `display/plane.c` | `5c4576f41fe6bafd0efa4545056638c5f5a03ae8358baf4721023d5f00bf6a76` |
| [parity/lcd/parity_backlight_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_backlight_glue.inc) | 5 | 0 | `display/panel.c` | `69e38f0e82282135d88e40f50bb41aa5ef0fbef610d8785425b2e94b80d01fe6` |
| [parity/lcd/parity_buf_trans_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_buf_trans_glue.inc) | 1 | 0 | `display/phy.c` | `3b0095f4301b4309bb9ccc0c2f6d11d17933bf861d597423fb9b27dd0dd1665f` |
| [parity/lcd/parity_bw_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_bw_glue.inc) | 2 | 0 | `display/watermark.c` | `c308cbe37c8554f10f792aea7e69330845d9f1df72bd895b26262f20d6082fad` |
| [parity/lcd/parity_cdclk_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_cdclk_glue.inc) | 1 | 0 | `display/clock.c` | `27600f0d664bde63a0ba58e5b6051c9d8c44d0eaa98ffa328e0ab0133ec370b5` |
| [parity/lcd/parity_color_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_color_glue.inc) | 2 | 0 | `display/color.c` | `42be1240c9a27341ce3138a9ac1cd0764bdf8a37137195097aa412c8f5b5cca3` |
| [parity/lcd/parity_ddi_emit_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_ddi_emit_glue.inc) | 13 | 1 | `display/ddi.c` | `d348a853c07a2186b6db2e1d6487a49c04f98c39ceac69fb94e51c33471344ed` |
| [parity/lcd/parity_ddi_hotplug_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_ddi_hotplug_glue.inc) | 6 | 0 | `display/hotplug.c` | `788efd23302315bb01256b599c295d60033c5c97198b299de0491d24006ee36b` |
| [parity/lcd/parity_display_emit_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_display_emit_glue.inc) | 5 | 0 | `display/pipe.c` | `07252b30aee5f9f53a2f0730cf34066467117b602044fb957dbf4f837c08817f` |
| [parity/lcd/parity_dpll_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_dpll_glue.inc) | 10 | 0 | `display/clock.c` | `831df0438cd1ff0cd49c99a4a161ed2da6fdc51af1a811e579d8abd4e9deb5e5` |
| [parity/lcd/parity_edid_mode_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_edid_mode_glue.inc) | 3 | 0 | `display/edid.c` | `b48daa7732a89674e93f923f36247c6f9bdc5c0efa0d21d6d9a6131a4f2377e0` |
| [parity/lcd/parity_flip_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_flip_glue.inc) | 8 | 0 | `display/vblank.c` | `d070b452970f7c30b228e6bcb6ef2c7a6000b92ee8c91bab2dfd5fea8b26d378` |
| [parity/lcd/parity_gmbus_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_gmbus_glue.inc) | 4 | 0 | `display/gmbus.c` | `25ecbb29bb7585a5e706ef193b2beb8784ad407275c5fce2dc12be32198cb0ec` |
| [parity/lcd/parity_hdmi_detect_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_hdmi_detect_glue.inc) | 10 | 1 | `display/hdmi.c` | `1d8d8d1487855e162ff37b934936b8e353f37e1018582d6923a70d86cf90f8b9` |
| [parity/lcd/parity_hdmi_mode_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_hdmi_mode_glue.inc) | 1 | 0 | `display/hdmi.c` | `ef576057175005d0f125e9f84e9140d46165bbc55a68115f2ac6928ff901bd33` |
| [parity/lcd/parity_hotplug.h](../../src/drivers/gpu/i915/parity/lcd/parity_hotplug.h) | 0 | 4 | `display/hotplug.h` | `74cc23bff1d86b9d1c99347b53aef95c5d775b22edb4dbc2260b380bbba393c0` |
| [parity/lcd/parity_hotplug_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_hotplug_glue.inc) | 42 | 0 | `display/hotplug.c` | `3bb56fee65426cd82f7323e0e3ed4d87b325ffe451a7763f402cbbe2485f10a9` |
| [parity/lcd/parity_hpd_test.c](../../src/drivers/gpu/i915/parity/lcd/parity_hpd_test.c) | 3 | 0 | `tests/display/parity-hpd-test.c` | `56bd650313c6f2ed9125b4842ebb28e0f0bcc3306657efeb9f96ea30703c7ec2` |
| [parity/lcd/parity_lcd_calc.c](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_calc.c) | 17 | 0 | `display/state.c` | `5df9cc21e48853258b1df59eca80e2a3a849ed517c4f79eecda7ca142f9d2b82` |
| [parity/lcd/parity_lcd_calc.h](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_calc.h) | 0 | 2 | `display/state.h` | `9e1c3af92441dac1fe8c51ef11197a013a7cfd94934703bad97a14166d2e506d` |
| [parity/lcd/parity_lcd_kernel.c](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_kernel.c) | 98 | 15 | `display/aux.c`<br>`display/diagnostics.c`<br>`display/modeset.c`<br>`display/panel.c`<br>`display/power.c`<br>`display/present.c`<br>`display/scanout.c`<br>`display/state.c`<br>`display/takeover.c`<br>`display/vblank.c`<br>`display/watermark.c`<br>`irq.c`<br>`mmio.c`<br>`sync.c`<br>`tests/display/kernel-scenarios.c` | `04b4b651d92de73097a1e978337e5af85c95ecf6e4665d905d22dd52fc558875` |
| [parity/lcd/parity_lcd_kernel.h](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_kernel.h) | 0 | 6 | `display/modeset.h` / `display/scanout.h` / `display/state.h` / `tests/display/kernel-scenarios.h` | `d892b6360580fca86e6be956b7bfdfd88dccddf27afa54e4c0a22b5ec80bc431` |
| [parity/lcd/parity_lcd_modeset.c](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_modeset.c) | 31 | 8 | `display/modeset.c`<br>`display/panel.c`<br>`display/present.c`<br>`display/vblank.c`<br>`tests/display/modeset.c` | `f2af27fef6ad43e0ec1541b70645859f1e819d405d6b934808b38abe0cb8ef44` |
| [parity/lcd/parity_lcd_modeset.h](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_modeset.h) | 0 | 10 | `display/modeset.h` / `display/panel.h` / `display/present.h` / `display/vblank.h` / `tests/display/modeset.h` | `2f8efe8101b1124dd995f42c09153f9ff2c4f2e7c95874f9db0204303f38c5ce` |
| [parity/lcd/parity_lcd_modeset_int.h](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_modeset_int.h) | 0 | 1 | `display/internal.h` | `d5370f1aa8243fc51c006e98b1bbe326f987618bec7dc3c89f939eb749626417` |
| [parity/lcd/parity_lcd_observe.c](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_observe.c) | 9 | 0 | `display/diagnostics.c` | `5406cdb452bcc244f9fc9c857efc20a17770795438e73d64b50e1505c09b9bc7` |
| [parity/lcd/parity_lcd_observe.h](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_observe.h) | 0 | 3 | `display/diagnostics.h` | `d2183a0290671904518de686ed75c71823d2746039f3dc88b7ce2f32716b3572` |
| [parity/lcd/parity_lcd_ops.h](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_ops.h) | 0 | 5 | `display/internal.h` / `tests/fixtures/display-io.h（実HW操作とfakeの境界を照合）` | `d6a1df5f73177404a5d1e86653f57cd5e02128592d150b70a6842582d77957f9` |
| [parity/lcd/parity_lcd_regs.c](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_regs.c) | 4 | 1 | `display/diagnostics.c` | `05300d32194faffb93af573e97063c1354895d1b946064befce340bec4ba2586` |
| [parity/lcd/parity_lcd_show.c](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_show.c) | 15 | 0 | `display/modeset.c` | `c635a8d26dde6bdcf8e3d1e0328aa938e288e140be19df9299ad01f3b8139d12` |
| [parity/lcd/parity_lcd_show.h](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_show.h) | 0 | 1 | `display/modeset.h` | `043407563d28616ea9303324ba36e73cb2549eb81e428a09d396f2f3bf733549` |
| [parity/lcd/parity_lcd_trace.c](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_trace.c) | 34 | 1 | `display/diagnostics.c` | `e8a3b6025d7c481c86086eaea75c552d6abf49fc4915661728a1de1e8e7ad11c` |
| [parity/lcd/parity_lcd_trace.h](../../src/drivers/gpu/i915/parity/lcd/parity_lcd_trace.h) | 0 | 2 | `display/diagnostics.h` | `aea91f30eefd9cba7f2b8497083e1d7dc8ade61c629c12d3afd0f1eab4a6c3f8` |
| [parity/lcd/parity_modeset_setup_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_modeset_setup_glue.inc) | 18 | 2 | `display/takeover.c` | `f17acd8801d77e329e385ec39440a096ea8f53bcfe6a04b905ce676f9d3a8699` |
| [parity/lcd/parity_n1.h](../../src/drivers/gpu/i915/parity/lcd/parity_n1.h) | 0 | 1 | `display/takeover.h` | `0f911b4dc59247e1efaa294c592f402447adcb728cae995b6e3eb67e1cbe145f` |
| [parity/lcd/parity_opregion.h](../../src/drivers/gpu/i915/parity/lcd/parity_opregion.h) | 0 | 11 | `display/opregion.h` | `d55ceb0fd5eeca42e9499d34eaabc1ea2925bfe3df700c86e3b8ce9245c6ef29` |
| [parity/lcd/parity_opregion_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_opregion_glue.inc) | 36 | 4 | `display/opregion.c` | `a8d6bf2068bdffcc90b014debfe7d49008eb957acc870f1dc91f9e3ed0e7f530` |
| [parity/lcd/parity_plane_emit_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_plane_emit_glue.inc) | 7 | 0 | `display/plane.c` | `9fd9353ff1e43a64832a04e4c2b75c8228d6da0872199d3f83377c46aab88188` |
| [parity/lcd/parity_wm_glue.inc](../../src/drivers/gpu/i915/parity/lcd/parity_wm_glue.inc) | 6 | 0 | `display/watermark.c` | `3e27a7108911d1705706f2e3046b6c50fff32e87edc3822ddae2f1e883f1850d` |
| [parity/lcd/port_lcd_calc.manifest.json](../../src/drivers/gpu/i915/parity/lcd/port_lcd_calc.manifest.json) | 0 | 0 | `data/provenance/port_lcd_calc.manifest.json` | `3c8f0dc7870569f5fa923bd839dc94849a2d625c4851c987364adc1733157f78` |
| [parity/lcd/scanout.c](../../src/drivers/gpu/i915/parity/lcd/scanout.c) | 8 | 4 | `display/scanout.c` | `4aaaef4b08c5dfaa2e1319697d4d6edfd2ffa96ac2072a0107dc3e70e5ec18a2` |
| [parity/lcd/scanout.h](../../src/drivers/gpu/i915/parity/lcd/scanout.h) | 0 | 3 | `display/scanout.h` | `deabd9e41191c88491f663d0a79d79572a44b2d5359c4d56e7fc4a79fde6a576` |
| [parity/lcd/scanout_ktest.c](../../src/drivers/gpu/i915/parity/lcd/scanout_ktest.c) | 5 | 3 | `tests/display/scanout-ktest.c` | `e369523382889ec8eb2ac8b2aad524ae65547cf12f50bb031d99a82b64a1b9c8` |
| [parity/lcd/scanout_ktest.h](../../src/drivers/gpu/i915/parity/lcd/scanout_ktest.h) | 0 | 1 | `tests/display/scanout-ktest.h` | `757a11b2807bf2c1921f4ea1c4ee82fb95dc3b003ee009fef57fd10bbed6416f` |
| [parity/lcd/skl_plane_port.c](../../src/drivers/gpu/i915/parity/lcd/skl_plane_port.c) | 30 | 0 | `display/plane.c` | `689f0ffe26ab3b72e23b6e78675c324889bfb092e54aad6a1f06a2c449d9e5fb` |
| [parity/lcd/skl_watermark_port.c](../../src/drivers/gpu/i915/parity/lcd/skl_watermark_port.c) | 69 | 0 | `display/watermark.c` | `360dcb8c21c8a0df175bb612185064b0cc22914362b7b3d78fbb273c83201d1e` |
| [parity/legacy_shim.c](../../src/drivers/gpu/i915/parity/legacy_shim.c) | 24 | 11 | `context.c`<br>`device.c`<br>`display/display.c`<br>`display/present.c`<br>`display/scanout.c`<br>`request.c`<br>`reset.c`<br>`sync.c` | `1529e1ec58680f1593cfd699fdc07e5aa6206b53c543e64f7bbd9404c7e763f7` |
| [parity/legacy_shim.h](../../src/drivers/gpu/i915/parity/legacy_shim.h) | 0 | 7 | `context.h` / `request.h` / `reset.h（名前差替えを解消）` | `0dbdaef3a547d8e7cf029a897554d3d77f706920d226bfb08a75af8ae88d020d` |
| [parity/native_decide.c](../../src/drivers/gpu/i915/parity/native_decide.c) | 1 | 0 | `display/takeover.c` | `2a0668afcdcfa6ad4d53dcb94fd4efa49c2017a7899a02a91466cf6052f213ea` |
| [parity/native_precheck.c](../../src/drivers/gpu/i915/parity/native_precheck.c) | 10 | 5 | `display/takeover.c` | `5e5b06eb4ff1767f70ea1fc342620978ab4e4fdee621c82eff62e384462f0382` |
| [parity/native_precheck.h](../../src/drivers/gpu/i915/parity/native_precheck.h) | 0 | 10 | `display/takeover.h` | `9fe9bece7abb522c737d521df449f4da60b2a524ce595a4676aaf40d064c0dd6` |
| [parity/opregion_service.c](../../src/drivers/gpu/i915/parity/opregion_service.c) | 5 | 0 | `display/opregion.c` | `4dac7e7ea8e779dee6d15bc7eb6c9482eee152f026dbfa790ec7b09e95e1a2ae` |
| [parity/opregion_service.h](../../src/drivers/gpu/i915/parity/opregion_service.h) | 0 | 5 | `display/opregion.h` | `a0249580f8c5ec7045ec0f2db485d9194aa4fdfec691411b12829a803b09290d` |
| [parity/opregion_vbt.c](../../src/drivers/gpu/i915/parity/opregion_vbt.c) | 3 | 0 | `display/vbt.c` | `ef768e424729c0e965eebeb043997ec669f92dfc81e79654de8ed05a38f3b649` |
| [parity/opregion_vbt.h](../../src/drivers/gpu/i915/parity/opregion_vbt.h) | 0 | 5 | `display/vbt.h` | `f2fa38f86b52b6ed66dc8b3b453e5ce907b786ef18ebe4d748a571982d00f3a5` |
| [parity/osdep/address_types.h](../../src/drivers/gpu/i915/parity/osdep/address_types.h) | 7 | 2 | `memory.h` | `1664a4692bbf8896a96b9b93ba29c3283057b81525fe8471537260c125d2307d` |
| [parity/osdep/dma.c](../../src/drivers/gpu/i915/parity/osdep/dma.c) | 18 | 5 | `memory.c` | `5371124b0401c322b9c125919a307293575f6d71c687a61cc328f53801d42f59` |
| [parity/osdep/dma.h](../../src/drivers/gpu/i915/parity/osdep/dma.h) | 0 | 3 | `memory.h` | `57258d433b69b38d0c7c01cda656471764cd248eabb89c20c1a145cfd1630288` |
| [parity/osdep/firmware.c](../../src/drivers/gpu/i915/parity/osdep/firmware.c) | 4 | 0 | `firmware.c` | `9d49f8586ad83b7da6247fea0730a82e6467c3191682d605d7d22c46ab337b60` |
| [parity/osdep/firmware.h](../../src/drivers/gpu/i915/parity/osdep/firmware.h) | 0 | 1 | `firmware.h` | `828641daee1e01d31245c1afa6fc7ac5017e120a540cf97d9d0d00d72b21ec3c` |
| [parity/osdep/mmio.c](../../src/drivers/gpu/i915/parity/osdep/mmio.c) | 18 | 3 | `mmio.c` | `0b9f5d858abfe6cf95b29416b2ec9cae05e61ae3b3fbf8ac34245d3b584771bc` |
| [parity/osdep/mmio.h](../../src/drivers/gpu/i915/parity/osdep/mmio.h) | 0 | 2 | `mmio.h` | `64b3c6b4206c8a2001a350fcc55cbc0146f51d83e86b9be1f676ab52eae50378` |
| [parity/osdep/pci.c](../../src/drivers/gpu/i915/parity/osdep/pci.c) | 22 | 5 | `device.c` | `d55a77c92fe5e4df02f5a803c3bd6ed112e525d10fcdd1c97ed62421c80844d9` |
| [parity/osdep/pci.h](../../src/drivers/gpu/i915/parity/osdep/pci.h) | 0 | 22 | `device.h` | `a567dbd8761b8371085dda592d065440652dab0061c49987745c4338cc14d379` |
| [parity/osdep/runtime_pm.c](../../src/drivers/gpu/i915/parity/osdep/runtime_pm.c) | 11 | 0 | `power.c` | `77fa2c307827937b4783cacd2ec5a4ac55003da169b6586be1933f4d5ddc9bbc` |
| [parity/osdep/runtime_pm.h](../../src/drivers/gpu/i915/parity/osdep/runtime_pm.h) | 0 | 1 | `power.h` | `9ecf2f5ca074b23e24f918ae03597ef5d60569278fa4ff9f7603a3dc24ea197a` |
| [parity/osdep/sync.c](../../src/drivers/gpu/i915/parity/osdep/sync.c) | 17 | 0 | `sync.c` | `9e358e004625f4b794fd09699bf96925ca10dd14dcdab7ef460232188b99dc00` |
| [parity/osdep/sync.h](../../src/drivers/gpu/i915/parity/osdep/sync.h) | 0 | 2 | `sync.h` | `c5a10c3079983e122a1e3d065a7647daf89aa33b37a677b00ce1b669555f5945` |
| [parity/osdep/trace.c](../../src/drivers/gpu/i915/parity/osdep/trace.c) | 5 | 0 | `trace.c` | `e0f025458b8d1d5f24f5cf7b934fb0629adb648943ecadf8314380389cba762c` |
| [parity/osdep/trace.h](../../src/drivers/gpu/i915/parity/osdep/trace.h) | 0 | 2 | `trace.h` | `ec142b97cc0fcb169f24b5a6878e602c720d4c0a4e1740092c6297c2e0ecf075` |
| [parity/parity.h](../../src/drivers/gpu/i915/parity/parity.h) | 0 | 1 | `device.h` / `tests/execution/runner.h` | `f2c374268dea4d8230520367f327faa405655378cdbdc44f011d4d89e1de617d` |
| [parity/pch.c](../../src/drivers/gpu/i915/parity/pch.c) | 6 | 34 | `device-info.c` | `9a8be7f9f12fc6642962ada4b9eabea517458ddd38a5648da3247e34684c1266` |
| [parity/pch.h](../../src/drivers/gpu/i915/parity/pch.h) | 0 | 1 | `device-info.h` | `99469f359af0ccf51d6e6d5c84f92001197422e87c343af6b7b68459dd17a3aa` |
| [parity/pcode.c](../../src/drivers/gpu/i915/parity/pcode.c) | 8 | 7 | `power.c` | `2ef0ff834b5577d533b3bc25cf510ca9639c04ff0406b60a3f19946964781670` |
| [parity/pcode.h](../../src/drivers/gpu/i915/parity/pcode.h) | 0 | 1 | `power.h` | `80d05194865fc4368f4cd14f0a7e30262c09e5448520de3864af55c4506bd906` |
| [parity/power_domains.c](../../src/drivers/gpu/i915/parity/power_domains.c) | 46 | 48 | `display/power.c` | `71cce8123b3a4be37faee43abcf6df12a7b3741fbebdf9078fee88ecafb98945` |
| [parity/power_domains.h](../../src/drivers/gpu/i915/parity/power_domains.h) | 0 | 11 | `display/power.h` | `b516608f7192e3b8ac19686722c30b47e4907c44690890a6073fbae1cb7b8c51` |
| [parity/probe.c](../../src/drivers/gpu/i915/parity/probe.c) | 7 | 1 | `device-info.c`<br>`device.c`<br>`display/takeover.c`<br>`trace.c` | `8b6d57b88fca20b2723b4ec1e53fd8c376a04929fedc0f04c9e9452e11c93cdf` |
| [parity/pte.c](../../src/drivers/gpu/i915/parity/pte.c) | 4 | 0 | `ppgtt.c` | `1ab04e74a61040e6e034500349491ada532187db2babb62a02d21af37df1f499` |
| [parity/pte.h](../../src/drivers/gpu/i915/parity/pte.h) | 0 | 5 | `ppgtt.h` | `f09c7a543be2ddc75f82f52871bfffbc100e59b05fc792fd0cc878715e0d55ad` |
| [parity/pxp.c](../../src/drivers/gpu/i915/parity/pxp.c) | 3 | 0 | `device.c` | `4147d788bd6ba9aba7906e556edceda0f7008e63b165eb96cb8d2a565557bb6b` |
| [parity/pxp.h](../../src/drivers/gpu/i915/parity/pxp.h) | 0 | 3 | `device.h` | `244fa33dea6e20b86454438becdc2afb9f7e6afdb8303fb8b2e6e3b9dd7b21ca` |
| [parity/reset.c](../../src/drivers/gpu/i915/parity/reset.c) | 1 | 4 | `reset.c` | `3ab3e3862a7ca09ecc33ba0ddc1f97400eb71f64bd6c313bff66b10454c14347` |
| [parity/reset.h](../../src/drivers/gpu/i915/parity/reset.h) | 0 | 1 | `reset.h` | `ecfa6f7a9e6c7599573813f4ac89396947d08fd94b892c22cd79bf3cee64ae1e` |
| [parity/resident.h](../../src/drivers/gpu/i915/parity/resident.h) | 0 | 3 | `device.h` / `request.h` / `display/display.h` | `8e48eb03b995ac8e6ba9fadc9d108c6bc624fe07600770901e46067a38bc9588` |
| [parity/resident_display.c](../../src/drivers/gpu/i915/parity/resident_display.c) | 15 | 3 | `display/display.c`<br>`display/hotplug.c`<br>`display/present.c`<br>`display/scanout.c`<br>`tests/render/readback.c` | `4bc0a66eb92bc5ef751eb9e9b00c0fff75be890233b295567daa9b7f52f6937e` |
| [parity/resident_display.h](../../src/drivers/gpu/i915/parity/resident_display.h) | 0 | 1 | `display/display.h` / `display/scanout.h` / `display/present.h` | `190c21527cc24ad87ced05271d120406cd7fd1eca5e2e313a2357e33129066e2` |
| [parity/runner.c](../../src/drivers/gpu/i915/parity/runner.c) | 6 | 0 | `device.c`<br>`tests/execution/runner.c`<br>readiness/device保持。test結果部分はtests | `4ef5b4ced98c70da8ee33d25eb6dd5f4ee0d0c204e9e7572593812328bd8218f` |
| [parity/runner.h](../../src/drivers/gpu/i915/parity/runner.h) | 0 | 1 | `device.h`（登録/readinessの本番責務） | `3c1f224339d1d977bbd0fa6ed53a48eb4c5b4d8fe9977041853a0b7954944204` |
| [parity/tests/dma_contract_test.c](../../src/drivers/gpu/i915/parity/tests/dma_contract_test.c) | 2 | 2 | `tests/fixtures/dma-contract-test.c` | `5a9f8cfdbc34f39b809e997711c7284ab3e290b4d6c154ab6b67b812b43b942c` |
| [parity/tests/mmio_contract_test.c](../../src/drivers/gpu/i915/parity/tests/mmio_contract_test.c) | 1 | 1 | `tests/fixtures/mmio-contract-test.c` | `010fac02bdd4b92a778e71645fe829c0bdd6953ebddb6ffac165ba0194f8b3f4` |
| [parity/tests/mock_dma.c](../../src/drivers/gpu/i915/parity/tests/mock_dma.c) | 12 | 1 | `tests/fixtures/mock-dma.c` | `abe5eba4d698de15b5e1b8f6e8ba9d4aa1c0b2ba2e07fd3bb792e21f7b4e51dc` |
| [parity/tests/mock_dma.h](../../src/drivers/gpu/i915/parity/tests/mock_dma.h) | 0 | 1 | `tests/fixtures/mock-dma.h` | `fe2257177d3911d26ecb5cfa71618d3a047411a576a46522bb799067e11d4afa` |
| [parity/tests/mock_mmio.c](../../src/drivers/gpu/i915/parity/tests/mock_mmio.c) | 10 | 0 | `tests/fixtures/mock-mmio.c` | `a3cac4d7e9fbf91c98cc843d6649acf20b1d391281c5abea3d50ca5968573066` |
| [parity/tests/mock_mmio.h](../../src/drivers/gpu/i915/parity/tests/mock_mmio.h) | 0 | 2 | `tests/fixtures/mock-mmio.h` | `881cad0d522a740c6b234663befecb366257f3dce9736f1ecdd30fbf80645b09` |
| [parity/tests/mock_pci.c](../../src/drivers/gpu/i915/parity/tests/mock_pci.c) | 19 | 0 | `tests/fixtures/mock-pci.c` | `b9356292986cf708d2febf045c2737baf384c808196c7f02fb9a268d018a6cb7` |
| [parity/tests/mock_pci.h](../../src/drivers/gpu/i915/parity/tests/mock_pci.h) | 0 | 1 | `tests/fixtures/mock-pci.h` | `c28ed0d16ee9aa0775bc0f24b1d81652832395967bd386aaa8ffe5acbb170af9` |
| [parity/tests/pci_contract_test.c](../../src/drivers/gpu/i915/parity/tests/pci_contract_test.c) | 1 | 1 | `tests/fixtures/pci-contract-test.c` | `da0ad532ba577b5d0763c81797b73691d7b7c6c071e3b84c080ff961841a3bd7` |
| [parity/tests/pte_contract_test.c](../../src/drivers/gpu/i915/parity/tests/pte_contract_test.c) | 1 | 2 | `tests/fixtures/pte-contract-test.c` | `4bc80c99648d42d71f13463b79d353b406d69eeddd1358fdec34132378485d8b` |
| [parity/tests/rpm_contract_test.c](../../src/drivers/gpu/i915/parity/tests/rpm_contract_test.c) | 3 | 1 | `tests/fixtures/rpm-contract-test.c` | `69ec1f1a09cbc89bacb47da5bbbd26d406b2037f88b47bbb390902668461e5b9` |
| [parity/tests/run.sh](../../src/drivers/gpu/i915/parity/tests/run.sh) | 0 | 0 | `tests/contracts/run.sh` | `98ce714bafb063f29e1e6f159e321b2e9ef230206aebcc196c4e8bb2b7ba776d` |
| [parity/tests/sync_contract_test.c](../../src/drivers/gpu/i915/parity/tests/sync_contract_test.c) | 5 | 1 | `tests/fixtures/sync-contract-test.c` | `75c693b8d1dd1692e0c5070639671412391f0194a4c12edfca7f1e8b31f17bd7` |
| [parity/timer_calc.c](../../src/drivers/gpu/i915/parity/timer_calc.c) | 5 | 0 | `sync.c` | `2b49fa2681333c16034fb0e151b1f18343582cb8f9a84ccdc5d98ceb9f66ff58` |
| [parity/timer_calc.h](../../src/drivers/gpu/i915/parity/timer_calc.h) | 0 | 1 | `sync.h` | `0641cd013e59720178629a72d31b90d26a4aa3cabda19fec359b8564a4fd2e9f` |
| [parity/vbt/intel_bios.h](../../src/drivers/gpu/i915/parity/vbt/intel_bios.h) | 0 | 25 | `display/vbt.h` | `0792e61362256848d94f0f250a62760f1f783245fb23bcdb632a20d99c9985ed` |
| [parity/vbt/intel_bios_port.c](../../src/drivers/gpu/i915/parity/vbt/intel_bios_port.c) | 91 | 3 | `display/vbt.c` | `96a43862dc4e62aa107272193328207e3c454b342ff27a93bdc3a18eb2af10b1` |
| [parity/vbt/intel_vbt_defs.h](../../src/drivers/gpu/i915/parity/vbt/intel_vbt_defs.h) | 0 | 166 | `data/display-intel-vbt-defs.inc` | `83f6ae7efa97a4c0b6822172cf1926220b9e1983c94c2233b4c85360d004ec05` |
| [parity/vbt/parity_vbt.h](../../src/drivers/gpu/i915/parity/vbt/parity_vbt.h) | 0 | 2 | `display/vbt.h` | `f778f4aeeaaf6609f719d6658598c034ec7dafb070d02e0e3f2a1ba995509bcc` |
| [parity/vbt/parity_vbt_glue.inc](../../src/drivers/gpu/i915/parity/vbt/parity_vbt_glue.inc) | 12 | 1 | `display/vbt.c` | `827e844029b058133f0a9a932f9e531724aa35d8897cfce81cc2aefa0f247d74` |
| [parity/vbt/vbt_compat.h](../../src/drivers/gpu/i915/parity/vbt/vbt_compat.h) | 11 | 117 | `display/vbt.h` | `5079da40c16d984e63d60138f5e5825e42f5ccab56d013f77ecbc07bbb57fca6` |
| [parity/vbt/vbt_ref_types.h](../../src/drivers/gpu/i915/parity/vbt/vbt_ref_types.h) | 0 | 1 | `display/vbt.h` | `8ce5b0b20f919a144ef1a2f080281e2a4a3dd4ddee4dd62197ea40a3c394ab4c` |
| [parity/vga.c](../../src/drivers/gpu/i915/parity/vga.c) | 12 | 13 | `display/takeover.c` | `d8af3638d32142f6b2274b6f5d601063f6a42d24720cfc970a0f2e7f21ee04ba` |
| [parity/vga.h](../../src/drivers/gpu/i915/parity/vga.h) | 0 | 5 | `display/takeover.h` | `f98a971c00595dcb700b45ca09bc31fd0269781c7e0dbf5cff52260588f3de1b` |
| [parity/wait.c](../../src/drivers/gpu/i915/parity/wait.c) | 11 | 4 | `sync.c` | `785788344f613b96cd5c201e84f5b5fd54c3555e9e7497d70a8b3c7bfd2fa75d` |
| [parity/wait.h](../../src/drivers/gpu/i915/parity/wait.h) | 0 | 1 | `sync.h` | `45f6b66f7a004ede6df900fa1e374e225d7a2d957196b34f56f8bff441c99a41` |
| [ppgtt.c](../../src/drivers/gpu/i915/ppgtt.c) | 12 | 4 | `ppgtt.c` | `e680f217ca1647f53d6a3ab924869ff839873b21e55d61a0956748c592cec332` |
| [request.c](../../src/drivers/gpu/i915/request.c) | 11 | 5 | `request.c` | `73b21609c72bdf19ae34db8fc5746759fa2781c4042912d31c45830919f26903` |
| [selftest.c](../../src/drivers/gpu/i915/selftest.c) | 42 | 36 | `tests/execution/selftest.c` | `3107f2c492b292d508058a928a41c3f6ed73261bfb3a59dc474b9aba22182ded` |
| [tex_fixture_fhd_gen.inc](../../src/drivers/gpu/i915/tex_fixture_fhd_gen.inc) | 0 | 32 | `tests/fixtures/tex-fixture-fhd-gen.inc` | `d3dbf64cfb554eb6cbcf802de4d7d40dc0bce3eb74df8cd855e5debe8e06bd0a` |
| [tex_fixture_gen.inc](../../src/drivers/gpu/i915/tex_fixture_gen.inc) | 0 | 26 | `tests/fixtures/tex-fixture-gen.inc` | `796b48e3a2f45b306940f93d5f6131b28426da89b6853506d3cc0da2ee56364d` |
| [uncore.c](../../src/drivers/gpu/i915/uncore.c) | 13 | 2 | `mmio.c`<br>`reset.c` | `24240facd955e78a541a50a1b06f4f942b2a922ea9f3e07c15d4aa61f605de35` |
| [vk/cmd.c](../../src/drivers/gpu/i915/vk/cmd.c) | 17 | 0 | `render/codec.c`<br>`render/dispatch.c`<br>`render/object.c`<br>`render/transport.c` | `a19f6fb3bc0cef97905869a150c7aacac4fda6f1f97fe40d6e7b9fd0b8bf1091` |
| [vk/cmd.h](../../src/drivers/gpu/i915/vk/cmd.h) | 0 | 1 | `render/codec.h` / `render/object.h` / `render/dispatch.h` | `ae15cb66aaa828ee41fba88b8e8a711eb2e306fcd2650ba0980e53f3511f60cb` |
| [vk/cmdbuf.c](../../src/drivers/gpu/i915/vk/cmdbuf.c) | 27 | 5 | `render/command.c` | `664409b9f2dd91fe55455cb2b892ad6450acf6b3cb2ea00196bba1c6582395ed` |
| [vk/cmdbuf.h](../../src/drivers/gpu/i915/vk/cmdbuf.h) | 0 | 1 | `render/command.h` | `2e96f1c77de7697a723de4cb779f6ef45534e3a7d44eccada7c2d68b33ed596b` |
| [vk/codec-generated.inc](../../src/drivers/gpu/i915/vk/codec-generated.inc) | 147 | 0 | `data/vulkan-codec.inc` | `7d82ffe7be4c82a60f0cce3345a8d9848d9056cce1853fbe6282a1fe0f1d47e1` |
| [vk/compile.c](../../src/drivers/gpu/i915/vk/compile.c) | 12 | 17 | `compiler/compile.c` | `2a595957d29c66f6c01680754c0b0a7e1d695cc1df2c80907f01aac63dfba79c` |
| [vk/compile.h](../../src/drivers/gpu/i915/vk/compile.h) | 0 | 1 | `compiler/compiler.h` | `444c0958bff61052b06a711e857f39b67f9e0e417bb24e3afd707147cb6424df` |
| [vk/display.c](../../src/drivers/gpu/i915/vk/display.c) | 4 | 0 | `render/wsi.c` | `fbde710630bcb28e5735baa506f83864d72427d510563dd20b90179bc6f5e6d5` |
| [vk/display.h](../../src/drivers/gpu/i915/vk/display.h) | 0 | 1 | `render/wsi.h` | `bc364b6c2276da22bae39f4a6650b97eea11da3bd5f5d95ea191c0f5fb46fe0c` |
| [vk/eu.c](../../src/drivers/gpu/i915/vk/eu.c) | 25 | 1 | `compiler/eu.c` | `8d62cc592a8d7a76650c42905bef095edb2bf90ec978ff5ee25da535b1a6e430` |
| [vk/eu.h](../../src/drivers/gpu/i915/vk/eu.h) | 0 | 1 | `compiler/eu.h` | `0e728165720bde0976e4ba479d1610e205bb3c88e66bb7cf06c96a3e37d2130c` |
| [vk/gfx-draw.c](../../src/drivers/gpu/i915/vk/gfx-draw.c) | 38 | 32 | `render/batch.c`<br>`render/blit.c`<br>`render/draw.c`<br>`render/math.c`<br>`render/memory.c`<br>`render/pipeline.c`<br>`render/state.c`<br>`tests/render/readback.c`<br>`tests/render/reference-shaders.c`<br>`data/render-eot.inc`、render内部layout型、device-info | `d258aeb0fbd44a09be8260696985866ce8ac7f8406c3856b655301e09569ba0f` |
| [vk/gfx-obj.c](../../src/drivers/gpu/i915/vk/gfx-obj.c) | 32 | 0 | `render/codec.c`<br>`render/descriptor.c`<br>`render/dispatch.c`<br>`render/image.c`<br>`render/memory.c`<br>`render/object.c`<br>`render/pipeline.c`<br>`render/render-pass.c`<br>`render/sync.c` | `7b3a8aea8c4f7b709a531589a293f54d15e3b5f810535aedb0669520bbaa4d22` |
| [vk/gfx-rec.c](../../src/drivers/gpu/i915/vk/gfx-rec.c) | 26 | 1 | `render/blit.c`<br>`render/codec.c`<br>`render/command.c` | `0cf6fde6ef3ae1432d8865669f658659882acd5350f0bb32f14ace98a3650f01` |
| [vk/gfx.h](../../src/drivers/gpu/i915/vk/gfx.h) | 0 | 6 | `render/internal.h` / `render/object.h` / `render/image.h` / `render/pipeline.h` / `render/blit.h` | `34225c5ba17014f979b7136a7bf485bb74332d569883613c8d144c361419f25d` |
| [vk/inst.c](../../src/drivers/gpu/i915/vk/inst.c) | 16 | 6 | `render/instance.c`<br>`render/transport.c` | `02ea7d01935e14890d92c83e201c2ab0a518096163b35ec88ee38ad7d4dba49c` |
| [vk/linux/3dstate-gen12.inc](../../src/drivers/gpu/i915/vk/linux/3dstate-gen12.inc) | 0 | 167 | `data/3dstate-gen12.inc` | `ec51dbba50c64e45518ad2461205bba41e6b0262c80e6ca941f562a77414f40b` |
| [vk/linux/eu-encoding-gen12.inc](../../src/drivers/gpu/i915/vk/linux/eu-encoding-gen12.inc) | 0 | 93 | `data/eu-encoding-gen12.inc` | `e00d91387f41a14018d2beb6bc347ec16dacc9d4ad2895e83c3d0af22a51bb39` |
| [vk/linux/surface-state-gen12.inc](../../src/drivers/gpu/i915/vk/linux/surface-state-gen12.inc) | 0 | 24 | `data/surface-state-gen12.inc` | `a9ffcd5a97b7791408f8f591df5c01cb1d81a04c519dd66d85f22fb5fcf50d31` |
| [vk/pipe.c](../../src/drivers/gpu/i915/vk/pipe.c) | 19 | 2 | `render/batch.c`<br>`render/pipeline.c`<br>`render/state.c` | `7cc259b669a2e6e1c50c58a328a2f441b63912674fd54ab35cfac696b33e6daa` |
| [vk/pipe.h](../../src/drivers/gpu/i915/vk/pipe.h) | 0 | 1 | `render/batch.h` / `render/pipeline.h` / `render/state.h` | `ebc1e0fdcb8f0335ee08f96255e14293626cb6cf05481496cc9548ee42dea578` |
| [vk/res.c](../../src/drivers/gpu/i915/vk/res.c) | 36 | 1 | `render/descriptor.c`<br>`render/dispatch.c`<br>`render/image.c`<br>`render/memory.c`<br>`render/state.c` | `e150ff0acab25f33044508d32cc3c3f16302e6aef3c341ca15fcd46362ff1f9e` |
| [vk/res.h](../../src/drivers/gpu/i915/vk/res.h) | 0 | 1 | `render/memory.h` / `render/image.h` / `render/descriptor.h` | `d0e66b5b35fbae6ecf198934a0a646c6b341e72dd4e2300a310eee3ac8cf93a4` |
| [vk/spirv.c](../../src/drivers/gpu/i915/vk/spirv.c) | 14 | 78 | `compiler/spirv.c` | `24a21950f293ac822b00aec446df61d0a149a4c0ba66d1c3bb92c29aae218e57` |
| [vk/spirv.h](../../src/drivers/gpu/i915/vk/spirv.h) | 0 | 2 | `compiler/compiler.h` / `compiler/ir.h` | `facba6deb7d005dac4399402edf6d59e7eb2407eb4714f5d17af1fa0c0bfa393` |
| [vk/sync.c](../../src/drivers/gpu/i915/vk/sync.c) | 17 | 0 | `render/sync.c` | `a8fd647b096c6cb26f8482db6b34d1398b53f9057ff846e70f10d9b1458d528e` |
| [vk/sync.h](../../src/drivers/gpu/i915/vk/sync.h) | 0 | 1 | `render/sync.h` | `21004836da9f8a9536e070b61383da208db2ab7579280ede07d969c6402b30d6` |
| [vk/vk-internal.h](../../src/drivers/gpu/i915/vk/vk-internal.h) | 0 | 3 | `render/internal.h` / `compiler/ir.h` | `0e8bfe345084c563a15993b00cb527ab277bc2d135a8e4fc6260fafa810222bb` |
| [vk/vk.c](../../src/drivers/gpu/i915/vk/vk.c) | 7 | 1 | `render/vulkan.c` | `95d3dfd60225144cdc9940bb88eb2b2ea479f3b700e8fc6746ea9a977ef9f99c` |
| [vk/vk.h](../../src/drivers/gpu/i915/vk/vk.h) | 0 | 1 | `render/render.h` | `5d2c4ed2a526fb709d343984db8c2934f60fb186820c511bb5824cc208115226` |
| [vk/vkc.c](../../src/drivers/gpu/i915/vk/vkc.c) | 7 | 0 | `render/codec.c` | `65ed9a35d7d477bf64fd9a67fa4b0ff0159d2f84b0811c5499e06ebfeee3f7c4` |
| [vk/vkc.h](../../src/drivers/gpu/i915/vk/vkc.h) | 0 | 1 | `render/codec.h` | `0cc921824f7ecf0df2edc11e39ea8aebe8f4c6b2892ea2ecfa98f3ccc8304c2f` |
| [vk/vkref-generated.inc](../../src/drivers/gpu/i915/vk/vkref-generated.inc) | 0 | 16 | `tests/fixtures/vkref-generated.inc` | `68d87669fc2a9c82aafce3eefcc3da2205c8284ca7b1eb0c66d85cebcafc3c1c` |
| [vk/wsi.c](../../src/drivers/gpu/i915/vk/wsi.c) | 5 | 2 | `render/wsi.c` | `0534c3cde202ba10fe4e8a09c834e2ec15e9350869e26e256313afbf2a8f3b0e` |
| [vk/wsi.h](../../src/drivers/gpu/i915/vk/wsi.h) | 0 | 1 | `render/wsi.h` | `16281b82fabcfeb6103fbae815faec2ac63ce97307767242d5e27fbd5018ebfb` |

## ツリー外の追従対象

ファイル移動時の変更候補であって、今回変更したファイルではない。
過去の実験patch/転送スクリプトは履歴として保持し、移行のために再実行しない。

| 現行パス/役割 | 必要な扱い |
| --- | --- |
| [include/drivers/i915.h](../../include/drivers/i915.h) / [i915-parity.h](../../include/drivers/i915-parity.h) | 登録とreadinessのE契約。旧parity名廃止時は本番callerごと置換 |
| [src/kern/main.c](../../src/kern/main.c) | scheduler等の準備完了後の呼出位置とconfiguration guardを保持 |
| [platform/amd64/vmunix.mk](../../platform/amd64/vmunix.mk) | production/test source list、生成include、条件付きlinkを再構成 |
| [gen_vk_server_codec.py](handover/tools/gen_vk_server_codec.py) | kernel codec出力先とvkc型/includeを追従。libvulkan側codecを入力として使う |
| [port_lcd_calc.py](handover/tools/port_lcd_calc.py) / [port_lcd_modeset.json](handover/tools/port_lcd_modeset.json) | generator/spec、出典notice、manifestと新所有ファイルの対応を記録 |
| [check_generated.sh](handover/tools/check_generated.sh) / [notice_map.py](handover/tools/notice_map.py) | 再生成比較対象とnotice対象の新pathへ追従。旧上書きscriptを盲目的に実行しない |
| [gen_fw_ranges.py](handover/tools/gen_fw_ranges.py) / [gen_lrc_offsets.py](handover/tools/gen_lrc_offsets.py) | GT tablesの出力先・参照を追跡 |
| [vk_opcode_survey.py](handover/tools/vk_opcode_survey.py) | 新dispatch分割を考慮して調査対象を追従 |
| [libvulkan context.c](../../userland/desktop/libvulkan/context.c) / [memory.c](../../userland/desktop/libvulkan/memory.c) / [sync.c](../../userland/desktop/libvulkan/sync.c) / [wsi-display.c](../../userland/desktop/libvulkan/wsi-display.c) | 互換契約の照合元。driverの配置変更のためにプロトコルを変更しない |
| `plan/ws029/tests`（git の履歴、2026-10-08 の WS の完了で削除） | old .c/.incを直接includeするfixtureとanalyzer/source list。旧テストを捨てず、新本番入口へ接続 |
| plan/ws031/handover/tools以下のlcd-e*/vk-e*等の実験patch | 旧pathを含んでも歴史的入力。現在の移行scriptと誤認せず保存。新構成へ自動再適用しない |

## 再確認方法と限界

`rg --files src/drivers/gpu/i915`の集合とこの表の旧パス集合を比較し、
missing/extra/duplicateが0であること、SHA256一致、関数台帳とのファイル別件数一致を確認する。
関数定義はGNU Emacs ctagsのC抽出と本体有無検査で列挙し、別の
`name(args) { ... }`字句抽出を突合した。macro候補は通常関数とは区別した。

この台帳は消失を検知する基準であり、変数/型/マクロの最終配置、
全構成でのコンパイル、生成物再現、動作互換を単独で証明しない。
実装では元内容の残余をゼロにするstatement単位の差分照合が必要。
