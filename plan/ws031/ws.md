<!-- awesome-plan project=zedbsd record=ws031 -->

# WS031: i915ネイティブVulkan実行器

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし
Resume point: p001〜p014 cleared、p015〜p048 planning。WS035 で見つかった GPU 基盤の問題も受ける
<!-- awesome-plan-current:end -->

## 単一目標

WS029で実機動作したi915ネイティブGPU driverの上に、**GPU driver内のnative Vulkan実行器**を実装し、libvulkan（WS030）が既存のdrv_gpu UAPIへ送るVulkanコマンドを、ホスト（QEMU/Venus/Mesa ANV）に依存せず、i915のGENコマンドとして実機で実行・表示する。到達点は実機ネイティブでの`/bin/vkdemo`（textured rotating cuboid）の描画とscanout。ホストLinuxのANVを使うVM経路（WS014）とは別の成果として、新しいWSで持つ。

## 順序・依存

前提はWS029（[#386](https://github.com/awemorris/zedBSD/issues/386)、i915 driver: PCI attach/forcewake/GGTT/PPGTT/GEM/engine RCS0・BCS0/LRC/execlists/request/native stream、実機copy/fill/store/job動作）と、WS030（[#388](https://github.com/awemorris/zedBSD/issues/388)、標準libvulkan.so: Vulkan 1.0 core 137 + WSI）。libvulkanは既に`/dev/gpu0`をdrv_gpu UAPI（`GPU_GET_INFO`/`GPU_GET_CAPSET`/`GPU_BLOB_CREATE`/`GPU_COMMAND_SUBMIT`）で直接叩いており、送信payloadはVenus wire形式（virglrenderer 1.1.0のop番号を再利用したVulkanコマンドの直列化）である。これは**ホストVenusランタイムへの依存ではなく、単なるVulkanコマンドの直列化**として扱う。本WSは、その直列化を**i915 driver内でデコードし、GENへ変換して実行するnative実行器**を新規に持つ。

WS029のdisplay/scanout後続（[ws029-f003](https://github.com/awemorris/zedBSD/issues/386)）とuserland Vulkanのnative経路（[ws029-f004](https://github.com/awemorris/zedBSD/issues/386)）は、本WSが正式に引き取る。WS014/WS030の実装責任はこの計画更新だけで移管しない。

## 範囲・受け入れの具体化

対象機はWS029と同じDell Latitude 5330（Alder Lake-P、Gen12 Xe-LP、`8086:46a8`）。native実行器はi915 driver（in-kernel）に置き、次の要素で構成する。

- Vulkanコマンドdecoderとobject/handle table（instance/device/queue/memory/buffer/image/pipeline/descriptor等をi915資源へ対応付ける）。
- image/sampler/descriptor/binding tableのi915資源モデル（GEM/GGTT/PPGTT、tiling、surface state）。
- **in-kernel SPIR-V→GEN baseline compiler**（1対1・最適化なし。SIMD8、SSA値ごとの素朴なGRF割当。Mesaと公開Intel PRMを参照し、命令encoding表は出典付き`.inc`へ転記、論理は新規実装）。
- Gen12 3Dパイプラインのstate emission（`3DSTATE_*`、URB、binding table、sampler state、render target、depth）。
- command buffer（`vkCmd*`）→GENバッチ変換と、WS029のRCS0 execlists/LRC/request経路での実行。RCSの3D利用を有効化する。
- fence/semaphore/query/timelineの同期を`drv_gpu_complete`へ接続。
- KMS/modeset + scanout（display plane、EDID/mode）。native WSI（`VK_KHR_display`/`swapchain`）を実表示へ接続。WS029のframebuffer保持work（[ws029-f003](https://github.com/awemorris/zedBSD/issues/386)）の延長。

最適化・完全なVulkan適合・全shader機能・複数engine並列は目標にしない。段階は増分A（三角形1枚、texture無し・定数色）→増分B（texture＋depth）→増分C（`vkdemo`の実vertex/fragment shader）。受け入れは実機ネイティブでの各増分の描画とscanout、および独立oracleでの画像照合とする。GuC/HuC不使用、HAL/UAPI不変を維持する。

## 外部設計と委譲構造

本WSは**外部設計（WS単体で自足）**として、全体アーキテクチャ・モジュール分解・モジュール間インタフェース契約の大枠・ファイル境界を [external-design.md](external-design.md) に規定する。実装は**Phase = 1 モジュール**で下位モデルへ委譲する。各 Phase の詳細設計（確定インタフェース、内部関数ガイド、触れる/触れないファイル、依存する前段 doc、受け入れ）は、その Phase の計画時に `plan/ws031/phaseNNN/phase.md` へ書く（テンプレは external-design.md §10）。前段 Phase doc が確定インタフェースの正本で、後段はそれを参照する。設計の背景・段階は [native-vulkan-design.md](native-vulkan-design.md)。

## Phase registry

各 Phase はモジュール単位。実装する公開インタフェースの大枠は external-design.md §4、委譲仕様（触れる/触れないファイル・依存・受け入れ）は §8。

| Combined ID | Phase / Module | Status | 見積 |
| --- | --- | --- | --- |
| ws031-p001 | 設計固め: 外部設計確定・`vk/*.h`枠・ライセンス監査・capset方針（[phase001](phase001/phase.md)） | cleared | 240 分 |
| ws031-p002 | top+cmd: 入口・wire decoder・object/handle table・dispatch（[phase002](phase002/phase.md)） | cleared | 300 分 |
| ws031-p003 | res: memory/buffer/image/sampler/descriptor→i915資源・surface/sampler state（[phase003](phase003/phase.md)） | cleared | 300 分 |
| ws031-p004 | spirv: SPIR-Vパーサ→baseline IR（[phase004](phase004/phase.md)） | cleared | 240 分 |
| ws031-p005 | eu: Gen12 EU命令エンコーダ（出典付き`.inc`＋論理新規）（[phase005](phase005/phase.md)） | cleared | 300 分 |
| ws031-p006 | compile: IR→GEN baseline codegen（素朴レジスタ割当、sampler send）（[phase006](phase006/phase.md)） | cleared | 360 分 |
| ws031-p007 | pipe: Gen12 3Dパイプラインstate emission（`3DSTATE_*`、URB、binding、sampler、RT、depth）（[phase007](phase007/phase.md)） | cleared | 360 分 |
| ws031-p008 | cmdbuf: command buffer→GENバッチ変換・draw・WS029 RCS0投入（[phase008](phase008/phase.md)） | cleared | 300 分 |
| ws031-p009 | sync: fence/semaphore/query/timeline→completion接続（[phase009](phase009/phase.md)） | cleared | 180 分 |
| ws031-p010 | wsi: KMS/modeset・scanout・swapchain present（`VK_KHR_display`/`swapchain`）（[phase010](phase010/phase.md)） | cleared | 300 分 |
| ws031-p011 | 統合: build-passing 達成／実機描画はビッグバンテスト（[phase011](phase011/phase.md)） | big-bang待ち | 360 分 |
| ws031-p012 | レビュー: 静的解析・規約全文確認・回帰・制限整理（[phase012](phase012/phase.md)） | cleared | 180 分 |
| ws031-p013 | Wayland モデルビューア（Venus）: FBX 変換・zwl の seat/pointer/keyboard・mview（[phase013](phase013/phase.md)） | cleared（Venus） | 3 日 |
| ws031-p014 | モデルビューアを i915 で・シェーダー一通り: executor（索引描画・mip・push・blend）と compiler（制御フロー・discard・行列・UBO）（[phase014](phase014/phase.md)） | cleared（A0・A・B・C・D・E1・E2・E3・性能第 2 回。残りは p015〜p018 へ） | 約 7.5 日（A–E） |
| ws031-p015 | 準正常系・異常系の確認と小さな欠落の修正（[phase015](phase015/phase.md)） | planning / canceled（2026-09-23 p022〜p029 へ分割） | — |
| ws031-p016 | executor の未実装機能（[phase016](phase016/phase.md)） | planning / canceled（2026-09-23 p030〜p037 へ分割） | — |
| ws031-p017 | compiler の未実装機能（[phase017](phase017/phase.md)） | planning / canceled（2026-09-23 p038〜p043 へ分割） | — |
| ws031-p018 | 性能の構造改善（[phase018](phase018/phase.md)） | planning / canceled（2026-09-23 p044〜p047 と p027 へ分割） | — |

### 残課題のブレークダウン（2026-09-23）

p015〜p018は大きすぎるため、1 Queueのスロットで終わる大きさに分割した。元のPhaseは分割による取消し
（canceled）とし、確認項目の正本は元の `phase015`〜`phase018/phase.md` に残す。新しいPhaseの範囲は
その該当項目である。executor・compiler・性能の実装Phaseの前には、方針（デバイスドライバには設計Phase）に
従って設計Phaseを置く。設計はOpus 5 Highで自動実行し、敵対的レビューも自動で行う。

| Combined ID | Phase | Status | 依存 | 元 | 主なファイル範囲 |
| --- | --- | --- | --- | --- | --- |
| ws031-p022 | 確認と修正: 画像・samplerの境界（小さいmip、非正方・奇数寸法、15 level、`maxLod < minLod`、端数LOD） | planning | WS035のrefactor（p002〜p004、p023） | p015 | `render/` |
| ws031-p023 | 確認と修正: blend・UBOの境界（`SRC_ALPHA_SATURATE`等、float target、動的定数、dynamic offset範囲外、短いUBO range） | planning | 同上 | p015 | `render/` |
| ws031-p024 | 確認と修正: compilerの境界（0除算・`INT_MIN/-1`、mod・FRem、ループ内discard・sample、shift、入れ子、SBE属性0、spillの組合せ） | planning | 同上 | p015 | `compiler/` |
| ws031-p025 | 異常系: 範囲外index/offset、command buffer 65536超、descriptor上限、終わらないループ（hangの扱いの記録） | planning | 同上 | p015 | `render/`、`vk/` |
| ws031-p026 | 小さな欠落: uint8 index、非整列 `vkCmdCopyBuffer`、viewport index>0・負の高さ、compile失敗時のpipeline漏れ、discardのHALT、host試験（ws031のdisplay 6件とws029）が `perf.c` 未linkで `drv_i915_perf_*` のlinkに失敗する件（ws035-p002で発見） | planning | 同上 | p015 | `render/`、`compiler/` |
| ws031-p027 | present mode（FIFO/MAILBOX/IMMEDIATE）でvsyncを選ぶ。UAPIで運べなければ変更を事前に提示 | planning | 同上 | p015・p018 | `libvulkan`、`zwl`、i915 display |
| ws031-p028 | 入力とmview: PS/2 keyboardのkeyがzwlに届かない件、QMP abortの回避記録、mviewの再現性・blend material・pixel shadingのLCD写真 | planning | 同上 | p015 | `zwl`、input |
| ws031-p029 | WS031の統合回帰（p014の回帰一覧を1回） | planning | p022〜p028 | p015 | 試験のみ |
| ws031-p019 | 設計: executorの未実装機能（p030〜p037） | planning | p022, p023, p025 | 新規 | 文書 |
| ws031-p020 | 設計: compilerの未実装機能（p038〜p043） | planning | p024 | 新規 | 文書 |
| ws031-p021 | 設計: 性能の構造（p044〜p047）。schedulerの扱い（本WSか新WSか）の判断を含む | planning | p029 | 新規 | 文書 |
| ws031-p030 | executor: mip level 0以外・array layerへの描画とattachment clear | planning | p019 | p016 | `render/` |
| ws031-p031 | executor: 複数colour attachment（MRT） | planning | p019 | p016 | `render/` |
| ws031-p032 | executor: blendのlogic op・dual source | planning | p019 | p016 | `render/` |
| ws031-p033 | executor: image viewのformat読替え（MUTABLE_FORMAT）・component swizzle・usage照合 | planning | p019 | p016 | `render/`、`vk/` |
| ws031-p034 | executor: sampler（anisotropy、depth compare、border colour、unnormalized座標）、mirrored blit | planning | p019 | p016 | `render/` |
| ws031-p035 | executor: descriptor配列・`vkUpdateDescriptorSets` のcopy・VSのsampled image | planning | p019 | p016 | `render/`、`vk/` |
| ws031-p036 | executor: UBOのdataport読み出し（push dataの上限超え）とdraw間の順序 | planning | p019 | p016 | `render/`、`compiler/` |
| ws031-p037 | executor: tiling（Y-tile/Tile4のoptimal image、copy・blit・sampling）。設計で更に分けてよい | planning | p019 | p016 | `render/`、gem |
| ws031-p038 | compiler: 整数varying（Flat）と整数頂点属性 | planning | p020 | p017 | `compiler/`、`render/` |
| ws031-p039 | compiler: 16 bit・64 bitの整数と浮動小数 | planning | p020 | p017 | `compiler/` |
| ws031-p040 | compiler: localの配列・構造体、動的index、行列の`OpPhi`、ループ内で初めてstoreするlocal | planning | p020 | p017 | `compiler/` |
| ws031-p041 | compiler: `OpSwitch`、関数呼出し（inline化）、ループ内return、trip count 0の形 | planning | p020 | p017 | `compiler/` |
| ws031-p042 | compiler: SWSBを依存に基づく指定へ、命令の並べ替え | planning | p020, p038〜p041 | p017 | `compiler/` |
| ws031-p043 | compiler: spillの改善（rematerialization、cost重み付きvictim、再lowerの削減） | planning | p020 | p017 | `compiler/` |
| ws031-p044 | 性能: 完了待ちをCSBのbusy-pollからuser interruptとwaitqへ | planning | p021 | p018 | engine、request |
| ws031-p045 | 性能: 非同期executor（submitを即座に返す、fence/semaphoreはGPU完了でsignal、heapの多重化と寿命） | planning | p021, p044, p030〜p037 | p018 | `render/`、`vk/` |
| ws031-p046 | 性能: frame copyの削減（swapchain imageのaliasing、zdesktopの拡大copyをplane scalerかzero-copy flipへ） | planning | p021, p045, p027 | p018 | `libvulkan`、zdesktop、display |
| ws031-p047 | 性能: scheduler wakeupの遅延（p021で本WSに収まると判断した場合だけ。収まらなければ新WSへ） | planning | p021 | p018 | kern（範囲はp021で決める） |
| ws031-p048 | 最終確認: 変更したsource全体の全文規約確認・静的確認・統合回帰 | planning | p022〜p047 | 新規 | 全体 |
| ws031-p049 | 失敗する GPU の host 試験 3 件の原因を調べて直す（2026-09-24 ws034-p049 の一掃で発見。今の source に対して build でき、結果が失敗する）: `plan/ws014/tests/run-venus-edid-test.sh`（`mode.count == 3` の assert）、`plan/ws030/tests/run-libvulkan-job-race-test.sh`（`race_wait` の `status == 0` の assert、112 秒で止まる）、`plan/ws030/tests/run-libvulkan-external-fence-test.sh`（`vulkan_sync_job_reserve` の確保 72 byte が LeakSanitizer で漏れ）。製品の不具合か試験の古さかを切り分け、試験が古いだけなら削除する | planning | — | 新規 | libvulkan、venus |
| [ws031-p050](phase050/phase.md) | （2026-09-26 ユーザー指示）session の close で残った Vulkan の object を解放、descriptor pool の破棄でその set を解放 | cleared（q467-i01、2026-09-26） | ws035-p067 | 新規 | `render/` |

並行の目安: `render/` 系と `compiler/` 系のPhaseは同じQueueで並行できる。実機（5330）を使う試験は
`flock /tmp/i915-hw.lock` で1つずつ流れるので、並行しても実機の実行は直列になる。

段階的な受け入れの単位は Phase 境界と一致しない。増分A（三角形）は複数モジュールの最小経路を横断する最初の実機到達点で、Phase 計画時に「増分Aで必要な関数」を先行実装対象として明示する。増分B・Cで texture/depth・実shader を足す。

## 引き継ぎ（2026-09-23、次のエージェントへ）

### 現状
- p001〜p014 cleared。p015〜p018 は planning（後回し、未着手）。**統合回帰は未実行**（p015 の最後に 1 回、一覧は下）。
- WS031 は未完了（2026-09-23 ユーザー判断で閉じない）。GitHub Issues には未公開（`plan/records.json` に ws031 は無い）。
  完了時に `plan/tools/README.md` の手順で同期する。
- 作業ツリーは centris の `~/zedBSD-gpu`。未コミットの変更がある（ユーザーがレビューしてコミットする）。**push はしない。**
- 主な資料: [phase014](phase014/phase.md)（進捗 A0〜E3・性能・後回し一覧）、[results-ws031.md](results-ws031.md)（E-ledger、E-134 まで）、
  [handover/README.md](handover/README.md)、[i915-rebuild-plan.md](i915-rebuild-plan.md)、`src/drivers/gpu/i915/tests/render/README.md`
  （shader 試験の索引）。専門家レビュー待ちの再構築報告は `handover/expert-reports/ws031-report-36.md`。

### ホスト
| 役割 | 名前・場所 | 接続 |
| --- | --- | --- |
| 作業・build | centris | `ssh awe@10.0.10.2`、tree `~/zedBSD-gpu`、passwordless sudo |
| 試験機（KVM host 兼、LCD が試験対象） | Dell Latitude 5330、hostname `chaos`、IP `10.0.10.25`。centris の ssh alias `solaris10-man`（`~/.ssh/config`: HostName 10.0.10.25、ssh-rsa/aes256-cbc/group1 を許可） | `ssh awe@10.0.10.25`（centris から `ssh solaris10-man`）、passwordless sudo。CPU i5-1245U（ADL-P）、iGPU `8086:46a8`（subsystem `1028:0b02`）、TSC 2.496 GHz |
| カメラ（LCD 撮影） | Windows workstation `C:\Work\qemu-work` | `tools\capture_lcd.ps1`（写しは [tests/host/capture_lcd.ps1](tests/host/capture_lcd.ps1)） |

### 5330 の iGPU の割当て
- boot の既定は vfio-pci（`/etc/modprobe.d/vfio-igd.conf` の `options vfio-pci ids=8086:46a8`、`/etc/modules-load.d/vfio-igd.conf`）。
- 実行時の切替は `~/bigbang/igpu-mode.sh host|vfio|show`（写し [tests/host/igpu-mode.sh](tests/host/igpu-mode.sh)）。
  `host` は host の i915 に付け替え、awe に `/dev/dri/renderD128` と `/dev/kvm` の実行時 ACL を付ける（Venus の QEMU は awe で動く）。
  `vfio` は passthrough 用。QEMU が動いている間は切替えを拒否する。
- `vkloop-hw.sh` は開始時に vfio へ戻す。Venus の runner（`plan/ws014/tests/run-venus-remote.py`）は host にしてから走らせ、終わると vfio に戻す。

### QEMU + i915 passthrough のコマンドライン
5330 の `~/bigbang/run-parity-vk.sh`（写し [tests/host/run-parity-vk.sh](tests/host/run-parity-vk.sh)）。直接ではなく
`vkloop-hw.sh` から使う（image を `~/bigbang/guest-parity.img` へ scp してから起動する）。要点:
```
sudo -n timeout 360 qemu-system-x86_64 \
  -machine q35,accel=kvm,memory-backend=mem -cpu host,host-phys-bits-limit=39 -m 4096 -smp 4 \
  -object memory-backend-memfd,id=mem,size=4G,share=on \
  -device vfio-pci,host=0000:00:02.0,x-igd-opregion=on,rombar=0 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/home/awe/bigbang/vg-parity.fd \
  -drive file=/home/awe/bigbang/guest-parity.img,format=raw,if=none,id=zd0 -device nvme,drive=zd0,serial=zedbsd0 \
  -vga std -display none -monitor none -serial file:/home/awe/bigbang/run-parity-serial.log -nic none \
  -debugcon file:/home/awe/bigbang/run-parity.log -no-reboot
```
- `QMP=1` で `-qmp unix:/home/awe/bigbang/qmp.sock,server=on,wait=off` と `qemu-xhci`＋`usb-tablet`＋`usb-kbd` を足す（capture と入力用）。
- `VK_STOP_RE` は終了検出の正規表現（既定 `VKDEMO DONE|VKDEMO FAILED|oneshot vkprobe1|resident: stopping`）。見つけて数秒後に QEMU を止める。
- guest からは OpRegion が見えない（ASLS=0）。そのため試験 build は `vendor/intel-vbt/` の 5330 の VBT を `I915_TEST_VBT=y` で取り込む
  （`vkloop-hw.sh` が常に指定。XXX: ベアメタルで GPU 試験ができるようになったら削除）。

### 日常のコマンド（centris の `~/zedBSD-gpu` で）
**実機を使う run は必ず `flock /tmp/i915-hw.lock ...` で 1 つずつ。** 1 run は build・転送・boot 込みで 3〜5 分。結果の serial 全文は
`/tmp/vkloop-last.log`。

| 目的 | コマンド | 期待値 |
| --- | --- | --- |
| vkdemo offscreen | `flock /tmp/i915-hw.lock plan/ws031/tests/vkloop-hw.sh` | frame 1 `rgb_sha256=7523debe5f925b33c94af8b0a306d6bcf62c78d520a9091d42d53ba032b105ff` |
| vkdemo を LCD に | `... vkloop-hw.sh display` | `rgb_sha256=94615464…19b1`、`resident display: ended PASS` |
| kernel 試験 | `... vkloop-hw.sh test <scenario>` | scenario: `ktest`（最後は 383/0/12）、`eu`、`draw`、`r1`、`tex`、`t3`、`bl`、`vkx`（8/8）、`vkc`（9/9）、`vke1`（3/3）、`vke2`（9/9）、`lcdb`、`display_ktest` ほか（`src/drivers/gpu/i915/tests/execution/runner.c` の表） |
| Wayland | `... vkloop-hw.sh wayland` | `WLTEST DONE wl1 frames=600` / `wl2 frames=60`、`ZWL EXIT ... error=0` |
| mview を LCD に | `... vkloop-hw.sh mview`、`MVIEW_ARGS="--spin=30"`（fps 計測）、`MVIEW_ARGS="--spin=30 --shading=pixel"`、`MVIEW_MODEL=test`（blend のある test model） | `MVIEW SPIN ... fps=`、`ended PASS` |
| vsync なし | `... vkloop-hw.sh "mview -DI915_PRESENT_NO_VSYNC=1"`（引数の後ろに `-D...` を書ける） | 190 fps（per-vertex）/ 130 fps（pixel） |
| capture display（LCD なし、guest RAM から画像） | `CAPTURE=<vkdemo\|wayland\|mview> ... vkloop-hw.sh <display\|wayland\|mview>` | `/tmp/capture-last/`（`result.json`・PPM・`sheet.png`）。mview は 6 検査と p013 Venus 画像との PSNR |
| build dir を分ける | `BUILD=build/xxx`（既定 `build/resident`） | flag の変更は自動で rebuild される |
| Venus（5330 の host i915 で virtio-gpu + venus） | `python3 plan/ws031/tests/run-mview-remote.py --attempt <未使用の名前> --timeout 300 --render-server /home/awe/zedbsd-q306-venus/dependencies/q312-quiesce/install/libexec/virgl_render_server --renderer-library-dir /home/awe/zedbsd-q306-venus/dependencies/q312-quiesce/install/lib/x86_64-linux-gnu --output-root plan/ws031/temp/remote`（同じ flag で `plan/ws014/tests/run-wayland-remote.py`・`run-vkdemo-remote.py`） | `"status": "pass"`。**q312 の renderer が必須**（stock の 1.1.0-2 では物理 device が 0） |
| host 試験 | `plan/ws031/tests/run-vk-host-tests.sh`、`run-lcd-modeset-host-test.sh`、`run-dp-host-test.sh`、`run-lcd-host-test.sh`、`run-opregion-host-test.sh`、`run-native-decide-host-test.sh`、`run-capture-host-test.sh`、`run-mview-host-test.sh`、`run-i915-firmware-package-test.sh`、`src/drivers/gpu/i915/tests/contracts/run.sh` | 全 PASS |
| EU encoding を Mesa と照合 | `BRW_TOOLS=~/p014-c/mesa/build-asm/src/intel/compiler plan/ws031/tests/run-vk-gentool-test.sh` | PASS（Mesa 25.0.7 の assembler/disassembler） |
| 1 file だけ compile | `plan/ws031/tests/i915-cc.sh <file.c>`（追加の flag は `I915_CC_CPPFLAGS`） | `i915-cc: ok` |
| LCD の写真（Windows 側） | `powershell -ExecutionPolicy Bypass -File C:\Work\qemu-work\tools\capture_lcd.ps1 out.jpg` | 新しい QEMU が起動し `serving presentation` が `~/bigbang/run-parity-serial.log` に出てから撮る（古い log に反応しない） |
| 性能の内訳 | mview の `MVIEW SPIN`・`MVIEW STAGES`、kernel の `i915: perf:`、zwl の `ZWL PERF`。scheduler の wakeup 遅延は `-DSCHED_WAKE_LATENCY=1` | phase014 §性能 第 2 回 |
| モデルの再生成 | `python3 userland/base/mview/tools/fbx2mview.py userland/base/mview/models/qs40/source/qs40-r4.fbx userland/base/mview/models/qs40 --max-texture-side 1024 --date 2026-09-22` | 出力は byte 一致 |

### 統合回帰の一覧（p015 の最後に 1 回）
offscreen hash、display `ended PASS`（写真）、`wayland`、`CAPTURE=vkdemo` / `CAPTURE=wayland` / `CAPTURE=mview`（per-vertex と `--shading=pixel`）、
`test ktest`・`eu`・`draw`・`tex`・`t3`・`bl`・`vkx`・`vkc`・`vke1`・`vke2`、host 試験一式、Venus の `run-mview-remote.py`・`run-wayland-remote.py`
（libvulkan の reap 変更と kernel tick 1 kHz は Venus 側にも効く）。

### 注意点（ハマりどころ）
- centris 経由の ssh で引用符が二重になると壊れやすい（特に heredoc 内の `'` と `\n`）。編集 script は手元で書いて scp してから実行する。
- 実機は 1 台。並列に run しない。build dir を共有すると flag の違いで毎回 rebuild になる（`BUILD=` で分ける）。
- 5330 の serial log は guest の console 出力と kernel の log が混ざる。行の完全一致に頼らない（capture は write_count で判定している）。
- Venus も同じ 5330 で動く。iGPU の切替えは QEMU が止まっている時だけ。
- LLVM toolchain は zedbsd6（commit "lldb works" 以降）。`build/llvm` は `~/zedBSD/build/llvm` からの写し。`build/llvm.zedbsd5-old` は削除してよい。
- QMP 経由の key は PS/2 keyboard だと zwl に届かない（usb-kbd なら届く、原因未調査は p015）。`input-send-event` に `device` を付けると
  QEMU 10.0.11 が egl-headless で abort する。
- `src/drivers/gpu/i915-old/` は専門家レビューの参照用に残してある（レビュー対応後に削除）。触らない。
- 規約: `plan/coding-style.md`、`plan/ws031/i915-rebuild-rules.md`。
- ユーザーの進め方: agent を並列に走らせない。正常系の疎通を先に、回帰は最後に 1 回。準正常系・異常系は後回しの一覧へ。commit は
  ユーザーが行い、push はしない。governance 文書（`AGENTS.md`、`plan/AGENTS.md`、`plan/master.md`、`plan/queue.md`）は指示なく編集しない。

### 次の作業
1. p015（確認・小修正・統合回帰）。
2. p016（executor）・p017（compiler）・p018（性能の構造）は計画のみ。着手はユーザーの指示で。
3. 再構築の専門家レビュー（report 36）への対応、その後 `i915-old/` を削除。
4. WS031 の完了判断はユーザー。完了したら GitHub Issues へ同期（WS 本体と p001〜p018 の Issue はまだ無い）。

## 適用規約・実行境界

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカル`plan/coding-style.md`の全文を実装前に読む。HAL責務/`hal.h`の変更は別途適用承認が必要。UAPI（`include/drivers/gpu.h`のlayout/ioctl番号）は不変を既定とし、必要になれば事前に提示して承認を得る。aggregate make checkは禁止、対象buildは`make -j16`と意味のある限定確認を用いる。無関係な変更を保護する。

Mesaを参照するファイルは、実装前にライセンス（MIT系）を機械監査し、値・テーブル・命令encodingの転記は出典・SHA・変換規則を付けた`.inc`へ分離する（WS029の`gen-inc.py`と同じ規律）。ロジックはzedBSD規約で新規実装する。Intelの公開PRM（GEN命令encoding/3Dパイプライン）も出典として使う。firmwareは不要（GuC/HuC不使用）だが、必要になれば`userland/firmware/<機種>/`へ置く。

本WSは計画のみ。有限Queue、実行範囲・調査上限は未選択。コード実装/build/実機は未実行。source/docのgit add/commit/pushはユーザーが行う。GitHub公開（Issue/Project同期）は別途の指示による。

## p001 完了: 設計固め・header 契約枠（2026-09-14）

外部設計（[external-design.md](external-design.md)）を正本化し、`src/drivers/gpu/i915/vk/` に `vk-internal.h` と 11 モジュール header（vk/cmd/res/spirv/eu/compile/pipe/cmdbuf/sync/wsi/display）を**契約枠として作成、全て単体コンパイル確認**。p001 決定は [phase001/decisions.md](phase001/decisions.md)（capset は libvulkan 無改造で整合、in-kernel compiler 制約＝FPU 不使用・kern_calloc・SIMD8・spill 最小、tiling は当初 linear、対象 subset）。実行承認は [phase001/approval.json](phase001/approval.json)、Queue は [queue-ws031.md](queue-ws031.md)。

運用調整（設計変更の一言記録）: ライセンス監査は外部設計で p001 実行想定だったが、Mesa 転記が p005/p007/p010 で発生するため、機械監査は各転記 Phase が `.inc` 生成直前に実行する方針へ変更（p001 は方針・参照範囲の確定に留める）。source は未 build 配線（p002 から `vmunix.mk` へ追加）。git add/commit/push はユーザー。

## p002 完了: top+cmd（2026-09-14）

native 実行器の入口・コマンドフレームワーク・drv_gpu 統合を実装・検証。`vk/cmd.c`（object table、LE wire reader/writer、opcode レンジ routing、builtin）、`vk/vk.c`（attach/detach/open/close、capset、command 入口、errno）、モジュール dispatch stub（p003+ で置換）。kernel 統合: `internal.h` に `i915_device.vk`/`i915_session.vk`、`i915.c` で vk attach/detach、`i915_open/close` で vk session、drv_gpu ops に `get_capset` 配線＋`GPU_CAP_CAPSET`、`i915_command`/`i915_command_submit` で native magic 0x31394958 を見て非 native を vk 実行器へ routing（submit は同期完了）。`vmunix.mk` に vk 7 source。検証: vk cmd host fixture（通常＋ASan/UBSan）PASS、i915 kernel build PASS（vmunix check、warning 0、FPU 不使用制約クリア）、WS029 host fixture 全 PASS（fixture に vk stub と capability assert 更新＝WS031 統合の最小変更）。残: libvulkan の完全 open には `GPU_CAP_BLOB|MAPPING` と blob_create/resource_map が必要で、これは res（p003）が memory/blob と共に提供。per-command handler は各モジュール Phase で肉付け。

## 引き継ぎ（2026-09-18）: Gen12 EU スレッド実行ハングの調査を専門家へ

p011 の実機ビッグバンで残った **EU スレッド実行ハング**（PS/compute 共通、Linux i915 では同一 GPU・同一バイトで完走）について、Linux 6.8.12 の通常初期化を parity 経路として完走させた上でも同一署名で再現した（台帳 E-97）。原因特定と問題箇所の修正を新規の専門家に引き継ぐ。**着手に必要な情報はすべて [handover/README.md](handover/README.md) にまとめてある**（問題定義・署名・除外済み事項・残る候補・コード地図・ビルドと QEMU 起動パラメータ・ログの読み方・再現手順・資料索引・規約）。

- 時系列の全記録: [results-ws031.md](results-ws031.md)（E-16〜E-30 が big-bang 期の EU 調査、E-31〜E-97 が parity 移植と EU 試験）
- 前任専門家の指示書と進捗報告: [handover/expert-reports/](handover/expert-reports/)
- 設計メモ・増分結果・生成ツール・Linux 陽性対照 VM 資材: [handover/notes/](handover/notes/), [handover/increment-results/](handover/increment-results/), [handover/tools/](handover/tools/), [handover/linuxvm/](handover/linuxvm/)

修正後は元の担当（Claude）に戻し、parity の残作業（DRM object model 要の部分、runtime suspend/resume、描画）を継続する。git add/commit/push はユーザ。
