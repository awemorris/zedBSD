<!-- awesome-plan project=zedbsd record=ws141 -->

# WS141: Raspberry Pi 4 のグラフィックス driver（VideoCore VI: HVS・pixelvalve・HDMI の display と V3D 4.2）

<!-- awesome-plan-current:start -->
Status: incomplete（display/V1〜V10/二device allocation-shareを実装、host/build PASS。native worker/job reservationを実装、Vulkan/compilerと実機は未完了）
Primary Milestone: MG006
Related Milestones: MG008
Parent: [Master](../master.md)
Queue: 既存履歴 q691（p001）・q695（p002）。現在の独立セッションの実行範囲は [execution-20261009.md](execution-20261009.md)
Resume point: i13のallocation/shareはmain cfb3401f7へ統合。private native worker・exact-once completion・supervised reserve/commit/cancel/capacityを実装しactual source host/rpi4 build PASS、warning0。worker checkpointを統合し、p006 kernel Vulkan実行器とV3D SPIR-V compilerへ検証済みscoped owner出力を接続する。COMMAND/CAPSET/JOB未公開。実機/console RAM寿命とp007最終監査は未達。Master/共有記録/T1投影はQ1担当。[worker結果](execution-20261009.md#i13-checkpoint-native-workerとsupervised-reservation2026-10-09)。
Target: **ベータ4 以降**（2026-10-05 user「WS037, WS044,WS048,WS141, WS112, WS118, WS124, WS125, WS126, WS119, WS096, WS097, WS039, WS038, WS144, WS143, WS146,WS147, WS152,  WS119, WS080, は、ベータ4以降としてください。…WS027, WS015, WS047, WS028, WS017,  WS077, はキャンセルします。」）
<!-- awesome-plan-current:end -->

## 独立セッションの担当（2026-10-09）

- ユーザー「WS141をあなたが作業します。P1,P2とは別なセッションです。ws141/ws.mdはあなたがそのセッションが排他的に更新しますが、master.mdは更新しません。同じソースツリーを使いますが、作業は別なディレクトリで行い、パッチをあなたに提供するので、Q1がマージします。」に基づく。
- 2026-10-09のユーザーの再確認: パッチの提供先はQ1、統合もQ1。この分担を保持する。
- 追加承認（2026-10-09）: ユーザー「パッチの影響範囲が狭いので、あなたがマージしてOKです。」により、今回のN1/V5準備のmerge担当をCodexへ変更。mainへmerge commit `a326c5e24`で統合済み。最新mainとの統合版でもhost4試験PASS・rpi4 y/n build warning/error 0。詳細は[実行記録i05](execution-20261009.md#i05-mainへの統合2026-10-09)。
- 継続のV7 noop生成も、同じ狭いWS141の変更としてCodexがmerge `16024f1b9`でmainへ統合済み。統合版のnoop/XML照合・既存4host試験とrpi4 y/n buildは全てPASS・warning/error 0。[実行記録i07](execution-20261009.md#i07の統合結果2026-10-09)。V7の実投入・実機clearanceは保持。
- 担当: このCodexセッション。`plan/ws141/ws.md` は担当が排他的に更新する。共有のMaster・Queue・Guardrail・他WSは読み取りだけ。共有記録の投影・T1依頼は引き続きQ1。
- 独立worktree: `/home/awe/zedBSD-claude1/.claude/worktrees/ws141-codex`、branch `codex/ws141-rpi4-gpu`、開始commit `a05865278`。sourceと成果のbuildはこのworktree内で行う。共有LLVMは読み取り専用のsymlinkで参照し、変更・再buildはしない。
- ユーザー回答「実機確認は後で行う」。p002のP0・V0、p003のN0以降の実機確認は未実施のまま保持し、実機観測が必要な依存は満たした扱いにしない。
- ユーザー「Q1でのマージは遅らせます。続きをお願いします。」により未マージの変更をこのbranchに積み上げる。N1の配置/コピーとV5のページ表を純粋な準備処理として実装しhost/buildを確認。詳細は[実行記録](execution-20261009.md)。
- 最初の確認は終了（driver y/n build・stage/list host PASS）。続いて作業資料を復旧した。範囲: p002骨格・p003/N0の既存実装と現在のbuildの整合を調べ、rpi4のdriver有効／無効build、既存の短いhost試験を実施。N1の前に必要な実機N0の写真は後続の再開条件。
- Q1へ渡す物: WIP commit、対象pathだけのbinary対応patch、基点・検証・実機待ち・残件の記録。

## 単一目標

Raspberry Pi 4（BCM2711、VideoCore VI）で、zedBSD の自前の GPU driver により display（firmware の framebuffer からの引き継ぎ、HDMI の mode set、scanout）と 3D（V3D 4.2 の job の実行）を成立させ、zedBSD の GPU の interface（`struct drv_gpu_interface`、i915 と同じ口）に載せて desktop を表示する。

## ユーザーの指示（2026-10-04）

「完全に独立した作業として、Raspberry Pi 4のグラフィックドライバを作成します。i915のときと同じで、Linuxドライバの初期化順やコマンド投入順などをまずドキュメントにして、正本も参照しながら、i915と同じように、我々のインタフェースに適合させていきます。フレームバッファにどこまで進んだかのメッセージを表示することで、少しずつデバッグしながら先に進めます。DRM部分の書き換えは、i915を参考にします。これは独立したWSで、17時以降に着手します。」

## 進め方（i915 の WS029・WS084 と同じ型）

1. **文書が先**: Linux の vc4（display: HVS・pixelvalve・HDMI・firmware KMS）と v3d（V3D 4.2）の driver の**初期化の順と command の投入の順**を、まず文書にする（p001）。正本（Linux の source の tag と path、Mesa の broadcom、Broadcom の公開の文書、device tree の binding）を版と hash 付きで記録し、参照しながら進める。
2. **我々の interface に合わせる**: DRM の部分（mode set・plane・buffer object・job の submit・fence）は、i915 の zedBSD の書き換え（`src/drivers/gpu/i915/`、`drv_gpu_interface`、resident display）を手本にする。
3. **段ごとの印を framebuffer に**: firmware が用意した framebuffer（`src/hal/arm64/bsp-rpi4/framebuffer.c`）に、driver が「どの段まで進んだか」の印（例 `v3d: P2 clocks ok`、`hvs: N1 readout done`）を表示し、実機で少しずつ debug する（i915 の N0・N1・P2 の段の印と同じ考え）。

## ライセンスの扱い（2026-10-04 ユーザーの決定、前の「ライセンスの境界」を置き換える）

2026-10-04 user「vc4/v3dドライバのソースから作る文書は作業ファイルにして、リポジトリにコミットせず、GPLコードを参考に書き写してもいいことにします。基本は手順を書き写しますが、表現が難しいときはコードを書き写してもいいです。我々のコードを生成する前に、定数はすべて、一括で、独自の名前に変更します。最後にライセンスに問題がないか、コードに類似がないかを監査します。これにより字面でも設計でもGPLコードを含めず、独自ライセンスとします。ファームウェアに相当しそうなBLOBがソースコード中にある場合は、userland/firmwareに移してファイルからロードすることにします。」

1. **作業の文書は repository に入れない**: vc4・v3d の GPL の source から作る文書（初期化の順・command の順の書き写し）は `plan/ws141/temp/`（`.gitignore` の `plan/ws*/temp/`）に置き、**commit しない**。手順の書き写しが基本、表現が難しい所は code の書き写しも可。
2. **定数の一括の改名**: zedBSD の code を書く前に、作業の文書の定数（register・bit・field の名前）を**全て一括で独自の名前に変える**（対応表も temp に置き、commit しない）。zedBSD の code は改名の後の文書から書く。
3. **最後の監査**: WS の最後に、license の問題が無いか、GPL の code と**字面でも設計でも類似が無いか**を監査する（類似の検出の道具と目視、[p007](../ws.md)）。結果を commit できる形（類似の検出の結果の要約、GPL の file の一覧と hash）で残す。
4. **BLOB**: source の中に firmware に相当しそうな BLOB（byte の配列など）があれば、`userland/firmware/` に移し、file から load する（RTL8822B・i915 の firmware と同じ形）。license は個別に確かめる。
5. **register の定義と packet の形の出典**（2026-10-04 ユーザー「これはそうしたいですね。」）: zedBSD の code の register の定義・command の packet の形は、**MIT の Mesa（`src/broadcom/`、例 `cle/v3d_packet.xml`）、Broadcom の公開の文書、device tree の binding** から取り、**file ごとに license を監査**する（path・SHA-256・license の表、[i915-license-audit](../ws029/i915-license-audit.md) の形）。GPL の vc4・v3d は手順の理解と作業の文書のためだけに使う。
6. zedBSD の code は独自の license（Zlib）。repository には GPL の code・作業の文書を入れない。

## 範囲

- display: firmware の framebuffer の引き継ぎ、HVS（plane の合成）、pixelvalve（timing）、HDMI（mode・EDID・audio は範囲の外）、vblank、scanout の page flip。
- 3D: V3D 4.2 の power・clock（firmware の mailbox）、MMU、bin/render の control list、CSD（compute）の job、reset、fence。
- zedBSD の GPU の interface への統合と、desktop（Keiland の compositor）の表示。Vulkan の実行器とSPIR-V compilerはp006で本WSに実装する（2026-10-09ユーザー確定）。
- 範囲の外: Raspberry Pi 5、DSI の LCD、HDMI の音、video の decode。

## 前提・関係

- [WS048](../ws048/ws.md)（rpi4 の FDT・PCIe・mailbox）、[WS044](../ws044/ws.md)（rpi4 を開発に使える形に、console）。firmware の mailbox は `src/drivers/platform/rpi4/rpi4-firmware.c`。
- HAL の API の変更が要るときは、差分を plan に置いて承認を得る（AGENTS.md）。
- 試験: 実機（Raspberry Pi 4、ユーザー）。QEMU の raspi4b は HVS・V3D を emulate しないので、display の印と host の試験（command list の生成など）で補う。

## Phase 一覧

| Phase | 目的 | Status | 依存 | 目安 |
| --- | --- | --- | --- | --- |
| [p001](phase001/phase.md) | 文書: Linux の vc4・v3d の初期化の順と command の投入の順、正本の一覧と license の監査、BCM2711 の display と V3D の構成、我々の interface への対応表、段の印の設計 | in-progress（q691、文書と review 済み、判定待ち） | なし | 4〜6h |
| [p002](phase002/phase.md) | **定数の一括の改名**（作業の文書、temp）の後に、段の印の仕組み（framebuffer に進み具合を書く debug の口）と driver の骨格（FDT の attach、MMIO の map、clock・power の mailbox、IRQ） | in-progress（q695、実装済み。骨格版のT1-092 PASS、2026-10-09 y/n build・host PASS。実機待ち） | p001 | 4h |
| [p003](phase003/phase.md) | display: boot出力先/modeの特定→Linux順R0再初期化/初回scanout→vblank/flip/合成/resident統合。旧コピー引き継ぎ/P4後回しは置換 | in-progress（i11 allocator/登録/copy present/2-plane合成/起動診断を実装、host/build PASS。実機待ち） | p002の骨格・WS048 mailbox限定修正。実機のwhole acceptanceは残る | 6h〜 |
| [p004](phase004/phase.md) | V3D: power・MMU・buffer object、bin/render・TFUのjob、reset、fence（CSDはp006後） | in-progress（V5のページ表・V7のnoop CL生成を実装、固定XML照合・host/build PASS。電源/register/job投入は未実施） | p002（骨格出力でsoftware準備、hardwareは実機V0確認後） | 6h〜 |
| [p005](phase005/phase.md) | `drv_gpu_interface` への統合と desktop の表示（Keiland の compositor） | in-progress | p003・p004のsoftware出力、desktopはp006 | 4h〜 |
| [p006](phase006/phase.md) | kernel Vulkan実行器・SPIR-V compilerとKeiland描画経路（2026-10-09 scope拡張） | planned | p004・p005 | 未見積 |
| [p007](phase007/phase.md) | 規約の全文の確認と最終の確認。**license と GPL の code との類似の監査**（字面・設計、道具と目視）、BLOB の移動の確認 | planned | 全て | 3〜4h |

## 要検討・ブロック（2026-10-05）

2026-10-05のユーザー指示で要検討・ブロックしていた。2026-10-09の独立Codexセッションへの割当で作業を再開。実機観測を必要とする段階の依存は、実機確認が後になるという回答に従い未達として保持する。


## i08の設計変更と依存待ち（2026-10-09）

- ユーザーのLinux一致要求と回答「Linuxと同じ再初期化へ変更する」によりp003の初回表示手順/検証を変更。R0は通知前の検証と初回frame採用を完了条件とし、旧N1の写真後のraw-copyという手順に代わる。途中の画面消失は承認済み、boot出力先以外を点灯しない規則は保持。p005は新R0の実出力とp004のjobの受け入れが依存で、host PASSだけでは満たさない。p004のV8・p006・p007は今回の選択範囲外。
- p003の実装/host/build結果と、WS048担当source/header/testの[限定提案](proposed/firmware-empty-tag.diff)を保存。WS048実sourceは未変更、適用回答待ちでi08/p003はuncleared。詳細・exact commands・hash・残件・再開条件は[実行記録](execution-20261009.md#i08の結果と再開条件2026-10-09)と[Phase](phase003/phase.md#i08の保存結果と再開条件2026-10-09)。
- この構造変更に伴うMaster/共有Queue/Guardrailの旧初期化順と承認の投影はQ1へ保留。p001の判定、実機/QEMU、license/全文準拠p007、WS全体の完了をこの結果で置き換えない。共有mainへは依存を解決・統合検証するまで未merge。


## i09: 依存修正とmergeの承認（2026-10-09）

ユーザー「mainにマージしてOKです。mailbox修正も承認します。」でWS048 mailboxの提案3 pathとmain統合を承認。容量0tagを実sourceへ適用し、実mailbox host/新display host/rpi4 y/n buildを確認済み。i08の依存待ちを今回のattemptで解消し、最新mainへ統合を進める。p003/WS全体の実機受け入れ、Q1の共有記録/WS048投影、T1回帰、p007は残る。詳細は[実行記録](execution-20261009.md#i09-mailbox実sourceの確認2026-10-09)。


## i09の統合結果（2026-10-09）

mailbox依存と初期表示R0成果を最新mainへ統合済み（dde7c1ba7）。実mailbox host/display host/rpi4 y/n build PASS、warning/error0。i09はsoftware/統合の部分範囲でcleared、p003の実機/後続機能の受け入れは残るためin-progress、WSはincomplete。i08のuncleared履歴は保持。実機の画面・buffer寿命/IRQ、T1回帰、最終監査p007とQ1の共有記録/WS048 body投影は未実施。[exact evidence](execution-20261009.md#i09-mainへの統合結果2026-10-09)。


## i10: 同期flip部品（2026-10-09）

ユーザーの継続指示でp003/P1/P2のsoftware/runtime部品を実装。既存mode/channel0/boot HDMIを保ち、caller-owned連続RGB32 bufferをinactive listに公開し、actual selected-PV/current一致で完了する。timeout時は旧/新bufferを保持し、確認済みconsole復帰まで通常flipを拒否する。host3試験/driver y/n build PASS。起動からのflip/allocator/GPU登録・P3・実機受け入れは未実施。p003 in-progress/WS incompleteを保持。変更は同Phase内で、foreign Phaseの契約を変更しない。[詳細と再開条件](execution-20261009.md#i10の実装確認2026-10-09main統合前)。


## i10の統合結果（2026-10-09）

同期flip部品をmainへ統合済み（181339820）、統合版もhost3試験/buildがPASS。i10のsoftware/統合部分はcleared。全displayの受け入れではないためp003 in-progress/WS incomplete、残件/再開点は上記と[実行記録](execution-20261009.md#i10のmain統合結果2026-10-09)。Q1の共有投影/回帰依頼は担当から更新しない。


## 完成までの自走・p006の実装範囲確定（2026-10-09）

ユーザー「続けてください。完成まで自走してください。」、続く回答「WS141に実行器・compilerも含め、Keiland表示まで進める」で、p006を方針だけからkernel Vulkan実行器/SPIR-V compiler実装に拡張した。p005のdesktopにはp006出力が必要、p007はこの最終sourceも含める。p005/006/007を個別Phaseとして保存し、i11〜i15の有限scopeを実行記録に追加。実機の受け入れはユーザーが後で実施、未確認をcompletedにしない。Masterは変更しない。[正確な承認・境界](execution-20261009.md#完成までの継続承認2026-10-09)。


## i11: displayのruntime接続（2026-10-09）

2つの恒久buffer owner、copy present/lease/固定modeのdevice登録、2-planeの位置/alphaとclockの前後処理、P1/P2/P3の実boot診断を追加。host4 PASS、y/n build warning/error0、software部分のみcleared。p003 whole acceptanceとWS completedは実機/console RAM寿命/V3D/Vulkan/最終監査待ち。main統合とi12へ継続する。[結果/再開](execution-20261009.md#i11-display所有合成登録のsoftware結果2026-10-09)。


## i11統合とV3Dの再開（2026-10-09）

i11はmain40ce86ac0へ統合、統合版host4とrpi4 y/n build PASS。i12でfixed firmwareのnative PM/reset/clock providerを検証するV1/V2とreset部品をbootへ接続しhost/build確認。V3D registerはまだ読まず、次にV3〜V10を接続する。実機whole acceptance/Vulkan/compiler/最終監査は残る。[詳細](execution-20261009.md#i11のmain統合とi12の再開2026-10-09)。


## i12: native V3Dのruntime接続（2026-10-09）

V1〜V10とtrusted CL/TFU/CSD runnerを実装し、actual source host/clear XML/rpi4 y-n buildがPASS、warning/error0。p005へ渡すcallerのbuffer/VA保持・retired=falseのquarantine・common recovery後のrelease、p006のprivate lowering契約を各Phaseへ記録。i12のmain統合確認後にi13へ継続する。実機/console RAM寿命/Vulkan/compiler/最終監査は未達、WS incomplete/p004 in-progressを保持する。[exact scope/commands/制限](execution-20261009.md#i12-native-v3dv1v10のsoftware結果2026-10-09)。

## i12統合・i13開始（2026-10-09）

V1〜V10のsource/host/buildまとまりをmain `30350c8cba31875a01d58f804a5de13a1305a7ee`へ統合。software部分attempt i12はcleared、p004の実機条件は未実施。p005をin-progressにして二deviceのallocation/share/VA/worker統合を進める。p006 Vulkan/compiler/Keilandとp007最終適合は承認済み後続。[詳細](execution-20261009.md#i12統合確認とi13開始2026-10-09)。Master/共有計画/外部公開の投影はQ1が行う。

## i13 checkpoint（2026-10-09）

二device登録・native blob/share/VA・direct scanoutの独立referenceとfailed flush quarantineを実装し、actual source host/rpi4 build PASS、warning0。p005はin-progress、次は非同期worker/common completion/job supervisionとp006 Vulkan/compilerのbinding。Vulkan/Keilandはまだ動作可能と主張しない。実機は未実施、WS incomplete。[詳細/復帰点](execution-20261009.md#i13の二deviceallocation共有の実装2026-10-09継続中)。


## i13 worker checkpoint（2026-10-09）

native FIFO workerとsupervised reservationを実装しactual host/rpi4 build PASS。normal cancel/uncertain retain/common callback joinとDMA quarantineを区別。p005はin-progress、p006へ検証済みscoped source出力を渡す。Vulkan/Keiland/実機/最終適合は未達。[詳細](execution-20261009.md#i13-checkpoint-native-workerとsupervised-reservation2026-10-09)。

## i14 compiler checkpoint（2026-10-09）

device-independent Zlib SPIR-V frontendをread-onlyで再利用し、独立QPU4.2 encoder、scalar validation/liveness、二threadのregister管理、VPM vertex/fragment interface、uniform/TMU/float RGBA output/source-over/target swizzle/終了を追加。actual Keilandのquad/panel shaderをcoordinate/vertex/fragment programへ変換し、固定MIT Mesaの独立decoder/repackerとsource/native scalar差分試験PASS、rpi4 build warning/error0。Vulkan command/pipeline/draw runtimeへの接続は次の作業。COMMAND/CAPSETは引き続き未公開、p006/i14はin-progress、WS incomplete。実機のshader/timing/memory/cacheは未検証。[結果と復帰点](execution-20261009.md#i14-compilerのsoftware-checkpoint2026-10-09)。


## i14 Vulkan protocol checkpoint（2026-10-09）

独立typed object/sessionの所有とbounded transportを追加。actual source host・actual libvulkan wire writer試験/rpi4 y build PASS、warning/error0。まだtyped GPU runtimeは未接続、COMMAND/CAPSET/JOB未公開。次はinstance/device/query/memory/pipeline/draw/queue、p006/i14はin-progress、WS incomplete。実機とp007の受け入れは未実施。[詳細と復帰点](execution-20261009.md#i14-vulkan-streamのsoftware出力2026-10-09)。


## i14 native Vulkan root/query checkpoint（2026-10-09）

実client codecとnative sourceでinstance/device/queue graph・physical queries・domainのcallback lifetimeを接続。承認されたshared4 pathでNormal NC RAMのuser mappingを追加、private buffer/VM alias ownershipも確認。host2/rpi4 y build PASS、warning/error0。次はVkDeviceMemory/nonzero BLOB/resource/pipeline/draw/queue。p005/p006とi13/i14はin-progress、COMMAND/CAPSET/JOB未公開、Keiland/実機/p007は未達。Masterは更新しない。[結果/復帰点](execution-20261009.md#i14-vulkan-native-rootqueryとnormal-nc-owner2026-10-09)。


## i14 root/query/Normal NCの統合（2026-10-09）

Q1最新mainを保持し、object/session/transport/root/queryと承認済みNormal NC mappingを統合。統合treeのhost2/rpi4 y-n build PASS、warning/error0。実装途中のVulkan entrypointsはまだ公開しない。i13/i14はin-progress、次はVkDeviceMemory/nonzero BLOB/resource/pipeline/draw/queue。[統合evidence](execution-20261009.md#i14-rootquerynormal-ncのmain統合確認2026-10-09)。

## i14 native memory/BLOB checkpoint（2026-10-09）

lazy VkMemory/type0/export/importとactual placement後のNormal NC backing、nonzero BLOB/resourceへのprivate routingを追加。actual client headerでvoidのopcode echo要求を確認しroot destructionを修正。actual VA/MMUを使うhostでindependent owner/budget/OOM/quarantine PASS、rpi4 y build warning/error0。i13/i14とp005/p006はin-progress、COMMAND/CAPSET/JOB未公開。次はbuffer/image/bindingとtyped rendering runtime、Keiland/実機/p007は未達。Masterは更新しない。[結果/復帰点](execution-20261009.md#i14-vkdevicememoryとplaced-blobのsoftware出力2026-10-09)。

## i14 buffer/image/binding checkpoint（2026-10-09）

実client codecでtyped buffer/image・requirements・same-device memory bind・linear colour pitchを追加。actual VA/MMU hostでbinding/prepared ownerの独立保持、logical extentとrequired sizeの区別、bad bind拒否を確認。host3範囲/rpi4 y build/style PASS。次はview/sampler/shader moduleからtyped draw/runtimeへ、COMMAND/CAPSET/JOB未公開、i13/i14/p005/p006 in-progress。Keiland/実機/p007未達。Master変更無し。[結果/復帰点](execution-20261009.md#i14-bufferimagerequirementsbindingのsoftware出力2026-10-09)。

## i14 immutable input checkpoint（2026-10-09）

colour image view、single-level nearest/linear sampler、owned SPIR-V moduleを実装し、actual client handle/record codec/native hostでparent/job/referenceとarena非依存を確認。host4範囲/rpi4 y build/style PASS、次はdescriptor/pipeline layoutからnative draw/runtimeへ。COMMAND/CAPSET/JOB未公開、i13/i14/p005/p006 in-progress、Keiland/実機/p007未達。[結果/復帰点](execution-20261009.md#i14-immutable-viewsamplerspir-v-moduleのsoftware出力2026-10-09)。Masterは更新しない。

## i14 canonical layout checkpoint（2026-10-09）

descriptor/pipeline layoutのexact client decoder、canonical binding、immutable parent所有、8texture/4uniform/4set/128byte push許可を追加。actual host5範囲/rpi4 y build/style PASS。次はpool/set/updateとdraw descriptor snapshot、public runtime/Keiland/実機/p007未達。i13/i14/p005/p006 in-progress、Master変更無し。[結果/復帰点](execution-20261009.md#i14-canonical-descriptorpipeline-layoutのsoftware出力2026-10-09)。
