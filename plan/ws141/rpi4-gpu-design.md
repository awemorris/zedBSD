<!-- awesome-plan project=zedbsd record=ws141-design -->
# Raspberry Pi 4 の GPU の設計（ws141-p001）

作成: 2026-10-04、P2（q691）。状態: 設計の案（code は無い）。design-reviewer の review（2026-10-04、高 3・中 11・低 13）を反映した版。

この文書は GPL の code・comment・Linux の識別子（関数・構造体・register・macro の名前）を含まない。Linux の vc4・v3d から読み取った手順は、事実だけを自分の言葉で書く。register の offset と bit の値はハードウェアの事実として使ってよい（2026-10-04 ユーザー「事実として使う」、[Guardrail](../guardrail.md) の WS141 の節）が、この文書には段の設計に要る最小限しか書かない。値の一覧と手順の詳細は commit しない作業の文書にある。

| 作業の文書（`plan/ws141/temp/`、commit しない） | 中身 |
| --- | --- |
| `v3d-init-and-submit.md` | V3D 4.2 の電源・clock・MMU・割り込み・CL/TFU/CSD の投入・reset・cache の手順と register の表、Mesa の CL の中身 |
| `vc4-display-init.md` | HVS・pixelvalve の初期化と mode set、display list の形、firmware の画面の読み方 |
| `vc4-hdmi-2711.md` | HDMI0/1 の clock・reset・PHY・timing・CSC・DDC（EDID）・hotplug、1920x1080@60 の値の例 |
| `fwdtb/` | 固定の firmware（`vendor/raspberrypi-firmware` の commit `3d301dd9`）の `bcm2711-rpi-4-b.dtb` と overlay の dts への戻し |
| `fw-wiki/` | Raspberry Pi の firmware の wiki（mailbox の property の文書） |

正本の版・hash・license は [rpi4-gpu-license-audit.md](rpi4-gpu-license-audit.md)。

## 2026-10-09: VC4起動経路の設計変更

ユーザーがLinux順の照合・修正を指示し、画面が一度消える再初期化を説明した上で「Linuxと同じ再初期化へ変更する」と回答した。旧§3の「Linuxとは違う道」、N1コピー後のN2、P4後回し、underrun後だけのclock引上げは、今回の初期化〜初回scanoutでは現行方針ではない。以前の判断と実装成果は履歴として残す。

初期化前にfirmware modeとboot framebufferを出すHDMIを取得し、同じ出力先で表示pipelineを構成する。hardware操作の順序は固定Linux v6.19の実経路と照合する。DRMやclock frameworkのソース構造は移植せず、zedBSDの所有・APIで独立実装する。firmwareの出力先以外を自主的に点灯しない制約、既存modeだけを最初に扱う制約、GPL作業資料の非commit、HAL APIの具体差分事前承認は保持。

変更の起点/新しいverification/resume条件は[p003](phase003/phase.md)、承認文と実行範囲は[実行記録i08](execution-20261009.md#i08の承認設計変更2026-10-09)。p004/V3Dの処理順やAPIは変更せず、V8は今回選択しない。p005のdisplay統合はLinux順で開始する初回scanoutを依存出力とする。

## 1. BCM2711 の display と V3D の構成

### 1.1 部品

| 部品 | 役割 | DT の compatible（binding の事実） | CPU から見た register（固定の firmware DTB の ranges から計算） | 割り込み（GIC SPI。公開の peripherals の文書の VideoCore の番号 + 64 と DT が一致。zedBSD の INTID は SPI + 32） |
| --- | --- | --- | --- | --- |
| HVS | plane の合成。display list（plane ごとの word の列）を読み、channel（3 本）へ画素を出す | `brcm,bcm2711-hvs` | 0xFE400000、32 KiB（後半の 16 KiB が display list の memory） | 97（underrun の報告だけ） |
| pixelvalve 0〜4 | timing の生成。HDMI0 は pixelvalve2、HDMI1 は pixelvalve4（どちらも 1 clock に 2 画素） | `brcm,bcm2711-pixelvalve0`〜`4` | pv2 = 0xFE20A000、pv4 = 0xFE216000（各 256 byte） | pv2 = 101、pv4 = 110（pv1 と共有）。**vblank はこの割り込み** |
| HDMI0 / HDMI1 | 符号化・PHY・DDC・hotplug | `brcm,bcm2711-hdmi0` / `-hdmi1`、DDC は `brcm,bcm2711-hdmi-i2c` | 10 個の領域（hdmi・dvp・phy・rm・packet・metadata・csc・cec・hd・intr2）。hd は 2 つで共有 | hotplug と CEC は l2 の割り込みの束を通る |
| dvp | 2 つの HDMI の reset と audio の clock の gate | `brcm,brcm2711-dvp` | 0xFEF00000 | — |
| l2 の割り込みの束 | CEC と hotplug の束ね（HDMI の intr2 と同じ register） | `brcm,bcm2711-l2-intc` | 0xFEF00100 | 96（DT では edge。zedBSD の rpi4 の GIC は全 SPI を level に設定している。使う時に注意） |
| V3D 4.2 | 3D（binner・renderer）、TFU（texture の形式の変換）、CSD（compute） | `brcm,2711-v3d` | hub = 0xFEC00000、core0 = 0xFEC04000（各 16 KiB） | 74（hub と core が 1 本を共有） |
| PM block | V3D の電源 domain と reset、AXI の bridge の停止 | `brcm,bcm2711-pm` | 0xFE100000、0xFE00A000、0xFEC11000 | — |
| firmware の mailbox | clock の rate と on/off、framebuffer、EDID、display の終了の通知 | `brcm,bcm2835-mbox`（既存の `src/drivers/platform/rpi4/rpi4-firmware.c`） | — | — |

束ね役の node（`brcm,bcm2711-vc5`）は register を持たない。

### 1.2 DMA の見え方（3 つが違う）

- **HVS（scanout）**: HVS が読む bus address は「CPU の物理 + 0xC0000000」の別名で、**物理の最初の 1 GiB だけ**が届く（soc の dma-ranges）。HVS に MMU は無いので、scanout の buffer は**物理的に連続**し 1 GiB より下に置く。CPU の cache とは一貫しないので、CPU が書いた buffer は scanout の前に clean する。
- **V3D**: 固定の firmware DTB では V3D の node の親の bus の dma-ranges が「child 0 → parent 0、16 GiB」で、V3D の bus の address は CPU の物理と同じ（恒等）。V3D はその前に**自分の MMU**（1 段の 4 GiB の page table）を持つ。V3D が届く物理の上限は MMU の物理の幅の register で決まる（未観測、8 GB の Pi 4 では要確認）。CPU の cache とは一貫しない。
- **mailbox の buffer**: 既存の driver の通り 1 GiB より下。

### 1.3 固定の firmware DTB の事実（2026-10-04 に dtc で確かめた）

zedBSD は firmware の DTB（`vendor/raspberrypi-firmware`、commit `3d301dd924bcd758a4c8cb19fe8531031f033f43`、`bcm2711-rpi-4-b.dtb` の SHA-256 `75761b73…739fcfb`）を使い、overlay は `disable-bt` だけ（`platform/arm64/config.txt`）。実行時に kernel が受け取る DTB は firmware が書き換えた物（overlay の適用、memory・cmdline などの書き込み）で、固定の file とは違う。p002 で実行時の DTB を確かめる。temp の DTB は同じ commit の URL から取った物（p2 の worktree では submodule が checkout されておらず、file の一致は未確認）。

1. HVS・pixelvalve・HDMI・DDC・V3D・束ね役の node は**全部 `status = "disabled"`**。有効にするのは `vc4-kms-v3d-pi4` の overlay（全部を okay にし、同時に firmware の framebuffer の node と firmware KMS の node を disabled にする）。
2. V3D の電源 domain と reset は **PM block**（firmware の電源 domain ではない）。clock は firmware の clock 5。V3D の node の clock の名前の property は綴りが誤っている（`clocks-names`）ので、driver は clock の名前に頼らず index で読む。
3. HVS の clock は firmware の clock 4（core）、HDMI は firmware の clock 13・14 と dvp の gate。
4. firmware KMS の node（`raspberrypi,rpi-firmware-kms-2711`）も disabled で存在する。WS141 は full KMS を取る（判断の項目 10）。
5. `config.txt` は両 port を 1920x1080@60（DMT 82）、framebuffer 1920x1080x32 に固定している。firmware の framebuffer は firmware が mode set した port に 1:1 で出る。

### 1.4 zedBSD の側の事実

- 割り込み: rpi4 の HAL は DT の割り込みの cell を GIC の INTID に直す（SPI は + 32）。driver は HAL を直接呼ばず `kern_irq_register` で登録し、`kern_irq_unmask` で開け、handler は返る前に `kern_irq_send_eoi` を呼ぶ（`include/kern/irq.h`）。handler の中では眠れない・確保できない。
- console: rpi4 の console は 80 桁 × 25 行の格子で、1920x1080 の framebuffer の中央の 640x400（x 640〜1280、y 340〜740）に描く（`src/hal/arm64/bsp-rpi4/framebuffer.c`）。`kern_logf` の文字は serial と framebuffer の両方に出る（`cons.c`）。framebuffer への書き込みを止める関数は HAL の中にあるが、どの header にも宣言されておらず driver から呼べない。
- mailbox: `drv_rpi4_firmware_property` は 1 回の値が 8 word まで。EDID の tag（128 byte の答え）は今の口では送れない。
- QEMU: 同じ rpi4 の kernel は QEMU の raspi4b でも boot する（WS044・WS048 の回帰）。QEMU は HVS・V3D・PM block を emulate しない。

## 2. 方針の要点

0. **scanout の規則（2026-10-04 ユーザー「GPUドライバはGOPの出力先以外に、scanoutを開始しない.というルールを覚えておいてください。」、Guardrail）**: driver は firmware の framebuffer の出力先（firmware が mode set した HDMI0 か HDMI1）以外に、自分の判断で scanout を始めない。N1 の引き継ぎはこの出力先の上で行う。P4 の full mode set と別の port への出力は、Keiland（compositor）の明示の指示がある時だけ。
   補い（2026-10-04 ユーザー「GPUドライバは、GOPの出力先であれば、scanoutできるようにどのインタフェースでも初期化を試みる、もまた真です。HDMIにせよDPにせよeDPにせよ。」）: firmware の出力先が HDMI0 でも HDMI1 でも、その出力先で scanout できるよう初期化を試みる（N0 で出力先を読み、その port の pixelvalve・channel を使う）。対応できない場合は log に出して firmware の画面を保つ。
1. **firmware の画面を消さずに引き継ぐ**（Linux とは違う道）。Linux の vc4 は firmware の画面を読み込まず、HVS の出力の切り替えを外し HDMI を reset してから一から mode set する。zedBSD は firmware の display をまず**読むだけ**で調べ、次に firmware の framebuffer を指す自前の display list に差し替え、すぐに firmware に display の終了を通知する（N2）。**N2 より前に firmware の持つ hardware に書くのは、N1 の display list と「次の list」の register だけ**。pixelvalve・HDMI には mode を変える段（P4）まで書かない（P1 の vblank の割り込みは N2 の後）。
2. **display と V3D は別の zedBSD の GPU device にする**（判断の項目 11）。V3D の hang と reset が display の device の fault にならないようにする。display の device は display の役、V3D の device は render の役で、互いに companion を指す（`include/uapi/gpu-scanout.h` の役と companion）。
3. **V3D は display と独立に進める**。V3D の段（V0〜V10）は display に触らないので、firmware の画面の上に印を出しながら debug できる。p003（display）と p004（V3D）は並行できる。V3D の出力を display に載せるのは p005。
4. **register の読み書きは段ごとに範囲を限る**。読むだけの段と書く段を分け、書く段は直前に読んだ値と比べてから書く（i915 の N0・N1 と同じ考え）。
5. **電源と clock より先に register を読まない**。HDMI は firmware の HDMI の state machine の clock が 0 のとき register に触ると CPU が止まる（Linux の注記）。V3D も電源 domain の前は触らない。
6. **QEMU と hardware の違いで安全に抜ける**。clock の rate が 0、識別の値の不一致、bridge の識別の値の不一致のときは書かずに「無し」で抜ける。boot の parameter で driver 全体と段ごとを止められるようにする（5 節）。
7. **CL の packet の形は MIT の Mesa（`src/broadcom/cle/v3d_packet.xml`）から**、自前の名前の生成器で C の header を作る（判断の項目 2 が前提）。

## 3. 段の一覧

段の名前は i915 に合わせる（N = firmware が残した display の扱い、P = driver の probe と display の段）。V3D は V、HDMI の mode set は H。各段の印は 5 節。

### 3.1 display（p003）

| 段 | すること | 確かめる値 | 書く物 |
| --- | --- | --- | --- |
| P0 | DT から HVS・pixelvalve2/4・HDMI0/1・dvp・l2 の割り込みの束・DDC・束ね役の node を探し、reg と割り込みを読む。disabled の扱いは判断の項目 3。HVS と pixelvalve2/4 を uncached で map | node の有無、address と割り込みの番号が 1.1 の表と一致 | 無し（map だけ） |
| N0 | firmware の display の調べ（読むだけ）。mailbox の **get の tag だけ**で framebuffer（物理・pitch・幅・高さ）と clock（core・HDMI の 2 つ）の今と最大。HDMI の clock が 0 でなければ、HVS の全体の enable、出力の切り替え（どの channel が pv2・pv4 を供給しているか）、その channel の enable・幅・高さ・状態・次と今の display list の位置、display list の解読（plane の数、format、位置、大きさ、pointer、pitch、end の位置）、pv2・pv4 の enable と timing | firmware の display list が framebuffer を 1:1 で指す plane を含むか（pointer = 0xC0000000 + framebuffer の物理、大きさ = 幅・高さ）。plane が 2 つ以上（firmware が重ねる物）でも止めずに全部記録する。firmware の list が使う word の範囲。HDMI の clock が 0 なら HVS より先は読まず「display 無し」で抜ける | 無し |
| N1 | 自前の display list で同じ画面を出す（画面を消さない引き継ぎ）。firmware の list の word を**そのまま写した**list を、firmware の list と filter の係数を避けた領域に書き、読み返して一致を確かめ、channel の「次の list」に入れる。「今の list」が自分の位置になるのを poll で待つ（数 frame、時限付き） | 読み返しの一致、「今の list」の一致、画面と印が変わらないこと（実機の目視） | display list の memory と「次の list」の register だけ |
| N2 | **N1 の直後**に firmware に display の終了を通知（mailbox）。通知の前後で HVS・pv2/4 の register、display list、clock を読み比べる | mailbox の成功、前後で値が変わらないこと。画面と印が残ること。**変わったら P1 以降に進まず、記録して止まる（設計を見直す条件）** | mailbox だけ（判断の項目 8） |
| P1 | 割り込み: pixelvalve の vblank（垂直の front porch の始まり）と HVS の underrun。pv1 と pv4 は 1 本を共有するので 1 つの handler が両方の状態を見る | vblank の回数が refresh に合う（60 Hz で 1 秒に 60±1）、underrun が立たない | pixelvalve の割り込みの enable・状態の消去、HVS の underrun の enable |
| P2 | 同期の page flip。driver が持つ 2 つの連続の buffer（1 GiB より下）に CPU で絵を描き、cache を clean して交互に出す。毎回、新しい領域に display list を書き、「次の list」を切り替え、vblank で「今の list」が新しい位置になったら完了とし、古い領域を返す | 完了の数、取りこぼし、古い領域の返却 | display list・「次の list」 |
| P3 | plane の合成。console の plane（下）の上に CPU で埋めた plane（上）。上の plane は console の領域（中央の 640x400）の外に置く。不透明でない plane には背景の fill | display list の word の和、画面の重なり、console が隠れない | display list |
| P4 | full mode set（HDMI の H0〜H10、3.2）。止める順は「vblank の割り込みを止める → 画素の流れを止める → 20 ms 待つ → HDMI の video を止める → pixelvalve の reset → HVS の channel の停止 → PHY と clock を止める → HVS の出力の切り替えを外す」。点ける順は「core clock の下限を上げる → channel の割り当てと HVS の出力の切り替え → display list を書く → vblank を開ける → 「次の list」と channel の reset・enable → HDMI の clock・PHY・scheduler・timing → pixelvalve の reset と timing → pixelvalve の enable → HDMI の CSC と FIFO → 画素の流れ → HDMI の video の enable・infoframe → vblank で「今の list」の一致 → core clock を必要な値に戻す」 | channel の状態が停止・空、pixelvalve の timing の読み返し、monitor に画が出る | pixelvalve・HVS の channel と出力の切り替え・HDMI・mailbox の clock |
| P5 | resident display（`drv_gpu_display_ops`、4 節）の統合 | present・wait・release の回数と完了の sequence | — |

- 20 ms の待ち: BCM2711 では pixelvalve と HDMI の間の FIFO に画素が 1 つ残り、mode を変えると画面全体が 1 画素右にずれることがある（Linux の注記）。省かない。
- core clock: Linux は commit の間 core clock の下限を 500 MHz 以上に上げ、終わると必要な値に戻す。zedBSD は N2 の後に core clock を読み、P3 の 2 plane で underrun が出るなら、mailbox で下限を上げる（判断の項目 15）。

### 3.2 HDMI（P4 の中、p003）

| 段 | すること | firmware の出力への影響 |
| --- | --- | --- |
| H0 | DT の解決（10 個の領域、dvp、l2 の割り込みの束、DDC） | 無し |
| H1 | firmware の clock の読み（HDMI の 2 つの clock、core の最大）。HDMI の state machine の clock が 0 なら以降の HDMI の register に触らない | 無し |
| H2 | firmware の HDMI の状態の読み（video の enable、scheduler、hotplug、timing、PHY の分周） | 無し |
| H3 | EDID。まず firmware の mailbox の EDID の tag（firmware の wiki にある）を試し、だめなら DDC（BSC 型の i2c）で 0x50 から 128 byte。DDC の Linux の driver は `drivers/i2c/busses/i2c-brcmstb.c`（計画が挙げた `drivers/i2c/busses/i2c-bcm2835.c` は誤り）。mailbox の道は `rpi4-firmware.c` の 8 word の上限を超えるので、その変更が要る（WS141 の範囲の外、判断の項目 14） | DDC は firmware と衝突しうる（未知） |
| H4 | hotplug の状態の読み | 無し |
| H5〜H10 | block の電源 → 既存の出力の停止 → clock と PHY → HDMI の timing → pixelvalve と CSC → video の有効化・HDMI mode・infoframe・FIFO の recenter | 止めて作り直す |

最初の目標は「firmware の mode のまま、HDMI は読むだけ」（N0〜N2・P1〜P3・P5）。H5 以降は mode の変更・hotplug の後の再設定・monitor 無しの boot のために要るが、後に回す（判断の項目 9）。

### 3.3 V3D（p004）

| 段 | すること | 確かめる値 |
| --- | --- | --- |
| V0 | DT と map（hub・core0）、割り込み 1 本、clock・電源 domain・reset の参照を読む。V3D の register はまだ読まない | address が 1.1 と一致、reg 2 つ、割り込み 1 つ |
| V1 | 電源と clock（mainline と同じ順）: mailbox で clock 5 の最小・最大・今を読む（今が 0 なら QEMU か未設定として抜ける）→ AXI の bridge の識別の値を読む（不一致なら抜ける）→ clock 5 を on・1 µs・off（reset の伝搬）→ PM block で V3D の reset を解く → clock 5 を on し最大に上げる → bridge の master・slave の停止要求を下ろし ack が落ちるのを待つ | clock の状態と rate、bridge の識別の値、reset の bit、停止の ack が 0 |
| V2 | （V1 に統合。clock の rate を最大に保つことだけを確かめる） | rate = 最大 |
| V3 | 識別（最初の V3D の読み）: hub と core の識別の register、MMU の幅 | 版 = 4.2、core 1、TFU・MMU あり、L3 cache 無し。core の識別の下の 24 bit が ASCII の「V3D」の並び（正しく読めている目印）。期待値は Mesa の drm-shim（MIT）の値で、実機は未観測。不一致なら抜ける |
| V4 | 不変の状態（L2T の flush の範囲を全体に）と割り込みの全 mask・消去 | mask の読み返し |
| V5 | MMU: 4 MiB の連続の page table（4 KiB の page、4 byte の entry、page 0 は使わない）、不正な access を流す 0 埋めの scratch page、MMU と MMU の cache の enable、TLB の clear。page table と scratch は uncached で map する（`kern_pmem_map_uncached`）か、PTE を書いた後 MMU の flush の前に `kern_dcache_clean_range` で clean する | page table の base の読み返し、flush と clear の完了の bit が時間内に落ちる |
| V6 | 割り込み: core（bin の終わり・render の終わり・CSD の終わり・binner の memory 不足・保護の違反）と hub（TFU の終わり・MMU の違反 3 種）。zedBSD は毎回 core と hub の両方を見る。handler は状態を読んで消し、完了の通知と worker の起床だけをする | 有効化の直後に割り込みが来ない |
| V7 | 何もしない job（Mesa の noop の job と同じ 1x1 の bin と render） | bin の終わり → render の終わりの割り込み、frame の counter、MMU の違反が無い |
| V8 | 見える render: render の CL で tile buffer を clear 色で埋め、raster の buffer へ store。CPU で invalidate して読む | buffer の中身が clear 色 |
| V9 | TFU（register だけで動く raster → tiled）。CSD は shader の binary が要るので p006 の後 | TFU の終わりの割り込み、出力 |
| V10 | reset と hang の回復: 自分へ branch する CL → 500 ms の timeout → CL の現在・戻りの address が変わらない → reset（PM block の電源 domain の off → on）→ V4〜V6 をやり直す → V7 | reset の後の識別が同じ、noop が通る |

job の投入の要点（自分の言葉で）:

- 各 queue（bin・render・TFU・CSD）は**一度に 1 job**。HW の FIFO には積まない。bin → render の順は software の依存で作り、HW の semaphore の packet は使わない。
- job の前に、job の buffer の一覧の CPU の cache を clean する（判断の項目 12 の (a)）。完了の後、CPU が読む buffer を invalidate する。
- bin: binner の overflow の pool を空にし、V3D の cache を無効化し、tile alloc の memory（address と大きさ）と tile state の配列を設定し、CL の開始を書き、**終わりの address を書くと動く**。
- render: V3D の cache を無効化し、CL の開始と終わりを書く（終わりで動く）。
- binner の memory 不足の割り込み: 割り込みの中では確保・map・MMU の flush の poll をしない。handler は worker を起こし、worker が**前もって確保した pool**（256 KiB の buffer を数個、MMU に map 済み）から 1 つを overflow の register に入れる。pool が空で確保もできないときは、その job を error で終える（binner は進まないので timeout と同じ扱いで reset）。足した buffer は対応する render の job が終わるまで生かす。
- TFU: 入出力の address・stride・大きさを書き、**設定の register を書くと動く**。V3D の cache の無効化は要らない。
- CSD: V3D の cache を無効化し、7 つの設定の register（0 番〜6 番）のうち 1 番〜6 番を書き、**0 番を書くと動く**。終わった後に必ず cache の clean（TMU の write combiner → L2T の書き戻し）。
- V3D の cache: job の前に外から内へ無効化（4.2 では L2T と slice の cache だけ）。
- tile alloc の大きさ: 64 byte × layer × tile の数を 4 KiB に切り上げ、+ 8 KiB、+ 512 KiB の余裕。tile state: 256 byte × layer × tile の数。

### 3.4 全体の順（提案）

P0 → N0 → N1 → N2 → P1 → P2 → P3（CPU で埋めた plane）→ P5（p003）。V0〜V10 は並行（p004）。V8 の buffer を上の plane に載せるのは p005。P4（H5〜H10）は最後（判断の項目 9）。

## 4. 我々の interface への対応表

計画の「`struct drv_gpu_interface`」は実在の名前では `struct drv_gpu_ops`（`include/drivers/gpu/gpu.h`、`DRV_GPU_INTERFACE_VERSION` 9）。手本は i915 の zedBSD の書き換えのうち、ops の表の組み立て（`src/drivers/gpu/i915/` の `session.c`・`resource.c`・`command.c`・`job.c`・`reset.c`・`display/display.c`・`display/present.c`・`display/scanout.c`・`display/vblank.c`）。i915 の Linux の手順の構造（段の関数の並び）は手本にしない（WS141 は GPL の構造を写さない）。

| zedBSD の口 | Raspberry Pi 4 での中身 | 段 |
| --- | --- | --- |
| `drv_gpu_register` | **2 つの device**（判断の項目 11）: display の device（display の役）と V3D の device（render の役）。互いに companion | P0・V0 |
| `capabilities` | bit は `src/drivers/gpu/gpu.c` の登録の検査に従う（scanout と recovery は bit でなく ops の表の有無）。display の device は `GPU_CAP_DISPLAY`・`GPU_CAP_DISPLAY_EVENTS`・`GPU_CAP_RESOURCE`・`GPU_CAP_TRANSFER`（複写の present の道）と scanout の表。V3D の device は i915 の node と同じ集合（`RESOURCE`・`TRANSFER`・`COMMAND`・`NOTIFICATION`・`JOB`・`JOB_CAPACITY`・`CAPSET`・`BLOB`・`MAPPING`・`SHARE`）と recovery の表（JOB には fault が必須） | P5・V7 |
| `open` / `close` | session。V3D の VA の割り当ての持ち主（最初は全 session で 1 つの page table、判断の項目 4・13） | V5 |
| `get_info` / `get_capset` | V3D の識別（版・core 数・TFU・CSD の有無）、display の有無 | V3 |
| `resource_create` / `blob_create` | buffer object。**BO ごとに物理が連続した 1 つの run**（`drv_gpu_mapping` は 1 つの範囲しか表せないため、判断の項目 12）。V3D 用は V3D の MMU の物理の幅の上限より下、scanout 用は 1 GiB より下（`kern_pmem_alloc_limited`） | V5・P2 |
| `resource_destroy` | 失敗できない。V3D の BO は、その BO を使う job が全部終わった後に、PTE を消し → MMU の cache の flush と TLB の clear の完了を待ち → page を返す。scanout 中の BO は下の release の順の後 | V5 |
| `blob_create_placed` | 連続・1 GiB 以下の要求を実際の配置で確かめて満たす（HVS の制約） | P2 |
| `resource_map` | CPU の mapping は coherent な RAM として cache 付きで user に map される。V3D・HVS とは一貫しないので、driver が job の前に clean、完了の後に invalidate、scanout の前に clean する（判断の項目 12 の (a)）。WC・uncached の mapping は今の口に無い（(b) は `gpu.h` の変更で、承認が要る） | V5 |
| `resource_read` / `resource_write` | CPU の copy と cache の掃除 | V8 |
| `command`・`commands`（`submit` / `drain`） | zedBSD の libvulkan は Venus の client なので、i915 と同じく kernel の中の Vulkan の executor が Venus の stream を decode し、CL・TFU・CSD の job を組み立てる（executor と SPIR-V の compiler は p006 の方針で決める、Guardrail の「SPIR-V の compile は kernel 空間」）。executor の内側の job の記述（CL の範囲・tile alloc・tile state・buffer の一覧・cache の flag、TFU・CSD の設定）を queue に積む。完了は割り込みの後に `drv_gpu_complete`。p004 の試験の job（V7・V8）は executor より前に kernel の中で組み立てる。UAPI を足すかは executor の方針（p006）で決め、足すなら `include/uapi/` の version・size・reserved = 0・64 bit の整列の規則に従う | V7〜V9 |
| `jobs`（`reserve` / `commit` / `cancel` / `capacity`） | 予約の領域を queue ごとに前もって確保。HW の queue の深さは 1 なので capacity は software の queue の残り | V7 |
| `recovery`（`stop_begin` / `stop_poll` / `fault` / `reset`） | 個別の job は止められないので、stop は「HW の job が終わるまで EAGAIN」。まだ HW に出していないその session の job は捨てる。500 ms の timeout（進まない）では stop_poll が EAGAIN 以外の error を返し、device 全体の fault に上げる。全 owner の退去の後に PM block の reset と V4〜V6 のやり直し。V3D の device だけが落ち、display の device は続く | V10 |
| `recovery.isolate` | 最初は**提供しない**（全 session が 1 つの VA を共有し、1 つの context だけを隔離できない）。判断の項目 4 | — |
| fence（`include/drivers/gpu/gpu-fence.h`） | 共通の fence の handle（generation・bind・遅延の signal）を queue ごとの seqno に結ぶ。割り込みの後に active な job を完了させる | V6 |
| `share` | V3D の device の BO を display の device が scanout するための共有の口（同じ driver の 2 device の間）。外の device の image は scanout の import | p005 |
| `display`（`query` / `mode` / `claim` / `release` / `present` / `wait` / `events`） | query: N0 で読んだ port と mode（firmware の mode）、EDID が読めれば preferred。mode: 最初は firmware の mode だけを列挙・検証。claim/release: 上の plane の lease（console の plane は下に残す）。**release と device の fault では、console だけの display list に戻して「今の list」の一致を待ってから scanout の buffer を返す**。present: BLOB でない present は storage を借りないので、driver が持つ連続の buffer に複写してから出す。BLOB の present は CPU の cache を clean してから出す。新しい display list を書いて「次の list」を切り替え、vblank で完了。wait・events: vblank の handler が進める sequence | P1〜P3・P5 |
| `scanout`（`query_device` / `constraints` / `import_image`） | query_device: display の役と V3D の companion。constraints: format は 32 bit の 2 種、pitch ≤ 65535 byte、幅・高さ ≤ 8191、placement は**連続**（1 GiB の上限は連続と別に要る。DMA32 では足りない）、`max_dma_address` は **CPU の物理で** 1 GiB − 1（HVS の bus address ではない）、flags は共有・複写・外の image の可否を実装に合わせて立てる。import: 借りた page が連続で 1 GiB より下かを実際の物理で確かめる | P2 |
| `present`（drv_gpu_ops の） | display の present と同じ道へ | P5 |

## 5. 段の印の設計

- **出し方**: driver は段の始めと終わりに `kern_logf` で行を出す（例 `rpi4gpu: N1 begin`、`rpi4gpu: N1 ok`、`v3d: V3 ok ver=42`）。rpi4 の console は文字を serial と firmware の framebuffer の両方に書く。i915 の `i915: N1 …` の行と同じ形。
- **80 桁に収める**: console は 80 桁 × 25 行。1 行は 80 桁以内にし、N0 のように値が多い段は「`rpi4gpu: N0 hvs …`」「`rpi4gpu: N0 pv2 …`」のように複数の行に分ける。25 行で流れるので、段の結果は最後の数行に要点（ok・失敗の理由）が残るように出す。
- **危ない書き込みの前に止まる**: 画面を消しうる書き込み（N1 の「次の list」、N2 の通知、P4 の各段、V1 の reset）の前に begin の行を出し、**数秒（既定 3 秒）待つ**。画面が消えても、その前の写真に begin の行が写る。
- **段で止める boot の parameter**: 例 `rpi4gpu.stop=N1`（その段の前で止まり、それより先の段をしない）、`rpi4gpu.off=1`（driver を attach しない）、`v3d.stop=V5`。実機で 1 段ずつ進め、QEMU や問題のある board で driver を止められる。parameter の読み方は p002 で既存の kernel の command line の口を調べて決める。
- **印を見え続けさせる**: N1 の後は自前の display list の下の plane が firmware の framebuffer を指し続けるので、console の印はそのまま出る。上の plane は console の領域（中央の 640x400）の外に置く。P4 で mode が framebuffer と違う大きさになるときは、console の plane を中央に置き背景を fill する（scaling は使わない）。
- **framebuffer の寿命（危険）**: N2 の通知の後に firmware が framebuffer の memory を解放・再利用するかは未知。その場合 HAL の console が書き続けると memory を壊す。HAL の console の framebuffer への書き込みを止める口は driver から呼べない（1.4）。N2 の前後で framebuffer の memory と mailbox の答えを確かめ、解放されるなら HAL の口（header の宣言、または kernel の側の口）が要る。`include/hal/hal.h` に及ぶなら差分を plan に置き承認を得る（判断の項目 16）。
- **画面が消えたとき**: 写真の最後の begin の行で段が分かる（上の待ち）。消えた後の状態は実機の serial で見るしかない。`plan/tools/guest/rpi4-serial.sh` は QEMU 用で実機には使えない。実機の serial の記録を debug と判定に使ってよいかはユーザーに確かめる（AGENTS.md は QEMU の log の判定を禁じている、判断の項目 6）。
- **V3D の段**は display に触らないので、印は常に見える。

## 6. driver の骨格（p002 の提案）

- 置き場所の案: `src/drivers/gpu/bcm2711/`（attach・2 つの device の ops の表・段の印・boot の parameter）、その下に `display/`（HVS・pixelvalve・HDMI・DDC・vblank・present）と `v3d/`（電源・clock・MMU・割り込み・queue・CL の生成・reset）。
- CL の packet の header は Mesa の `v3d_packet.xml` から自前の生成器で作る（名前は独自、host で生成して commit するか build で作るかは p002 で決める）。host の試験で「noop の job」と「clear の job」の byte 列を作り、Mesa の decoder（MIT、temp の中で使うだけ）で解けることを確かめる。
- 割り込み: `kern_irq_register`・`kern_irq_unmask`・`kern_irq_send_eoi`（1.4）。共有の割り込み（pv1・pv4）は 1 つの handler が両方を見る。
- MMIO: `kern_device_map`（uncached）。page table・scratch・scanout の buffer: `kern_pmem_alloc_limited`。
- 試験: 実機（判断の項目 6）と host の試験（CL の生成）。加えて「QEMU の raspi4b の boot が壊れない」（driver が安全に抜ける）を試験の担当 T1 の回帰に足す。
- **定数の一括の改名**は p002 の最初。作業の文書の register・bit・field の名前を全部、自分の命名（block と機能を表す自前の語）に置き換えた版を temp に作り、対応表も temp に置く。zedBSD の code はその版から書く。

## 7. BLOB と表

- firmware に当たる byte の配列は vc4・v3d に無い（[監査](rpi4-gpu-license-audit.md) の 6）。ARM の側で load する firmware も無い（VideoCore の firmware は boot の partition の物）。`userland/firmware/` への移動の対象は無し。
- 定数の表: HVS の scaling の filter の係数（Mitchell-Netravali、B = C = 1/3）は公式から自分で計算する。HDMI の PHY の設定の表（TMDS の rate ごとの PLL の値）と色の変換の係数は「register の offset と bit」の決定の範囲を超える表の値なので、扱いを判断の項目 17 にする（案: PHY の値は可能な限り式から計算し、残る値は事実として使う。CSC は BT.601/709 の公開の式から自分で計算する）。

## 8. 危険と未知（実機で観測していない）

1. firmware が display の終了の通知の後に hardware と framebuffer の memory に何をするか（画面・clock・memory を変えるか）。
2. firmware の display list の位置と使う channel、firmware が重ねる plane の有無（N0 で読めば分かる）。
3. N2 の前に firmware が display list・「次の list」を書き直すか（N1 と N2 の間を短くし、N2 の前後で読み比べる）。
4. HVS が「次の list」を取り込む時刻。完了は「今の list」の一致で判定し、時刻を仮定しない。
5. monitor 無しで boot すると HDMI の clock が 0 で、HDMI の register に触ると CPU が止まる。N0・H1 で必ず先に clock を確かめる。
6. DDC が firmware と衝突するか。まず mailbox の EDID を使う。
7. V3D の reset は AXI の bridge の停止の ack を 1 µs しか待たない。hang の最中に失敗しうる。
8. V3D の VA は全 session で共有され、session の間の保護が無い（どの session も他の BO を読み書きできる。Linux も未実装、判断の項目 13）。
9. V3D の MMU の物理の幅（8 GB の Pi 4 の上の RAM を指せるか）、識別の値、frame の counter の field の幅。
10. QEMU の raspi4b は HVS・V3D・PM block を emulate しない。disabled の node を使うと QEMU でも PM block に書きうるので、2 節の 6 の抜け道が要る。
11. 2 つの HDMI の共有の control register の片方の操作がもう片方に及ぶか。
12. l2 の割り込みの束（hotplug）は DT では edge、zedBSD の GIC は全 SPI を level に設定している。

## 9. 判断の項目

**2026-10-04 ユーザーの決定（クリックの回答「既定案で全部承認」）**: 項目 2〜17 は下の表の「案・既定」の列のとおり（12 は (a)、17 は事実として使う）。14 の EDID の mailbox は Q1 が決める（それまで firmware の mode だけ）。16 で hal.h に及ぶならその時に承認を取る。

| # | 問い | 状態 | 案・既定 |
| --- | --- | --- | --- |
| 1 | V3D・HVS・pixelvalve・HDMI の register の offset と bit が GPL の header にしか無い | **決定（2026-10-04 ユーザー「事実として使う」）** | 値は事実として使う。名前は一括で独自に、配置・comment・構造は自分で書き、WS の最後に類似を監査 |
| 2 | Mesa の `src/broadcom/cle/v3d_packet.xml` は file に license の表記が無い。Mesa の `docs/license.rst` は「大部分は MIT、file ごとに SPDX を見よ」 | 未決 | 同じ directory の他の file が MIT の notice を持つので、Mesa の既定の MIT として使う。不可なら Mesa の MIT の C の file（Vulkan の driver の CL の組み立て）から packet の形を読み取る |
| 3 | DTB: 固定の firmware DTB では GPU の node が全部 disabled | 未決 | `vc4-kms-v3d` の overlay は入れず（firmware の framebuffer の node を消し firmware の動きを変えうる）、この driver だけは disabled の node を使う。QEMU では 2 節の 6 の条件で抜ける。代わりに overlay を入れる道、mainline の DTB に替える道もある |
| 4 | session ごとの隔離（`isolate`）と VA の分離を当面しない | 未決 | 当面しない。hang は V3D の device 全体の fault と reset。session ごとの page table の切り替え（両 queue が空のときだけ）は後の Phase の候補 |
| 5 | WS の到達点を V8（clear + store）と TFU で止め、CSD と描画は p006（実行器の方針）に回してよいか | 未決 | そうする |
| 6 | 実機の試験の手順（誰が、どの monitor・port で、写真で段の印を読む）と、**実機の serial の記録を debug・判定に使ってよいか** | 未決 | ユーザーが実機で boot し、画面の写真を返す。各段の Phase の最後に 1 回。serial は画面が消える段の debug にだけ使う |
| 7 | 引き継ぎの道: Linux 流（firmware の display を止めて一から mode set）か、N1 の画面を消さない引き継ぎか | 未決 | N1 の画面を消さない引き継ぎ（印が見え続ける） |
| 8 | firmware に display の終了を通知するか、どの段で | 未決 | N1 の直後（P1 より前）の N2 で、前後の読み比べ付きで。通知で画面か framebuffer の memory が変わるなら、P1 以降に進まず設計を見直す |
| 9 | 最初の port と mode: firmware が出している port（HDMI0 か HDMI1）と firmware の mode（1920x1080@60）のままで P5 まで進め、mode set（H5〜H10）は後に回してよいか | 未決 | そうする |
| 10 | full KMS（自前の HVS・pixelvalve・HDMI）を前提とし、firmware KMS（mailbox に mode を頼む downstream の道）は取らない | 未決 | full KMS（ユーザーの指示「Linuxドライバの初期化順…」に沿う） |
| 11 | display と V3D を 1 つの zedBSD の GPU device にするか 2 つにするか | 未決 | 2 つ（display の役と render の役、companion で組）。V3D の hang で desktop の display の lease を失わないため |
| 12 | V3D・HVS と CPU の cache の非一貫を、今の `drv_gpu_mapping`（物理が連続の 1 範囲、cache 付きの RAM として map）でどう扱うか | 未決 | (a) BO ごとに連続の run を取り、driver が job の前に clean・完了の後に invalidate・scanout の前に clean する（`gpu.h` を変えない）。(b) `gpu.h` に page の一覧と WC・uncached の属性を足す（共有の interface の変更で main と承認が要る） |
| 13 | V3D の VA を全 session で共有し、利用者の間の保護が無い（どの session も compositor の画面を含む他の BO を読み書きできる）ことを当面受け入れるか | 未決 | 当面受け入れ、V3D の device を開ける者を限る（例 compositor と信頼する client）。session ごとの page table は後の Phase |
| 14 | EDID の mailbox の tag は今の `rpi4-firmware.c` の 8 word の上限を超える。`rpi4-firmware.c`（WS048 の物、WS141 の範囲の外）の変更が要る | 未決（計画に無い依存） | Q1 が WS048 の追加か WS141 の範囲の拡大を決める。それまでは DDC の道か EDID 無し（firmware の mode だけ） |
| 15 | core clock の管理（Linux は commit の間に下限を上げる） | 未決 | N2 の後に読み、underrun が出たら mailbox で下限を上げる。P4 では上げる |
| 16 | N2 の後の firmware の framebuffer の寿命と HAL の console（driver から書き込みを止める口が無い） | 未決 | N2 で framebuffer が残るなら今のまま。消えるなら HAL の口が要り、`include/hal/hal.h` に及ぶなら差分を plan に置いて承認を得る |
| 17 | HDMI の PHY の表と色の変換の係数（offset と bit の範囲を超える表の値）、mailbox の tag のうち公開の wiki に無い物（display の終了の通知、電源 domain の set）の値の出典 | 未決 | 項目 1 と同じく事実として使ってよいかをユーザーに確かめる。PHY は式から計算できる所は計算し、CSC は公開の式から計算する。wiki にある tag（clock・framebuffer・EDID）は wiki を出典にする |

## 10. Phase への引き継ぎ

- p002: 定数の一括の改名（temp）→ 段の印の helper（80 桁・待ち・boot の parameter）→ 骨格（P0・V0、map・割り込みの登録・mailbox の clock）と QEMU での安全な抜け。判断の項目 3・11 の答えが要る。
- p003: N0 → N1 → N2 → P1 → P2 → P3（CPU で埋めた plane）→ P5、P4（H0〜H10）は後。判断の項目 7〜9・14〜16。
- p004: V1〜V10。V7・V8 の CL の生成は host の試験を先に。判断の項目 2・4・5・12・13。
- p005: V3D の出力を display に載せ（share の口）、desktop を出す。p003・p004 に依存（ws.md の表の通り）。
- 試験: 実機（判断の項目 6）と、QEMU の boot が壊れないことの回帰（T1）。QEMU では HVS・V3D の試験はできない。


## 2026-10-09 i08の保存状態

初期表示R0を生成/実行/IRQ採用確認の独立構造で実装し、両portのhostとrpi4 y/n buildを確認した。値なしの表示終了通知をWS048 clientが拒否する必須依存は未適用のため、実kernelの再初期化はop0 EINVALで始まらない。i08/p003はuncleared。限定差分の適用回答後に依存確認・統合検証へ再開する。scope/相違/実機を含む未達とcommandsは[実行記録](execution-20261009.md#i08の結果と再開条件2026-10-09)、WS acceptanceへの影響は[WS記録](ws.md#i08の設計変更と依存待ち2026-10-09)。


## 2026-10-09 i09の依存解決

ユーザーがmailbox修正とmain mergeを承認。容量0tagの限定修正を実sourceへ適用して実mailbox hostを確認し、i08で拒否されていた通知依存を解消した。display host/rpi4 y/n build PASS、最新mainとの統合を検証中。i08のuncleared履歴を保持し、実機での初期scanout・buffer寿命/IRQと後続機能の受け入れは別に残す。[実行記録i09](execution-20261009.md#i09-mailbox実sourceの確認2026-10-09)。


## 2026-10-09 i09のmain統合完了

容量0tagの承認済み依存修正とR0初期表示成果をmainへ統合（dde7c1ba7）、統合版でも実mailbox/display hostとrpi4 y/n build PASS、warning/error0。i09はsoftware/統合範囲だけcleared。実機scanout/console RAM寿命・IRQ、後続機能・p007の受け入れは残る。i08の拒否結果は過去の結果として保存し、現在の再開点は[WS](ws.md)と[統合記録](execution-20261009.md#i09-mainへの統合結果2026-10-09)。
