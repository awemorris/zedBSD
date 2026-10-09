<!-- awesome-plan project=zedbsd record=ws141 -->

# WS141: Raspberry Pi 4 のグラフィックス driver（VideoCore VI: HVS・pixelvalve・HDMI の display と V3D 4.2）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-09 ユーザーが独立CodexセッションへWS141を割り当て、再開。実機確認は後で実施）
Primary Milestone: MG006
Related Milestones: MG008
Parent: [Master](../master.md)
Queue: 既存履歴 q691（p001）・q695（p002）。現在の独立セッションの実行範囲は [execution-20261009.md](execution-20261009.md)
Resume point: 独立Codexセッションを開始（2026-10-09、基点a05865278）。骨格・N0の現行rpi4 kernelはdriver y/nともbuild exit 0、warning/error 0。stage/list host試験PASS（[実行記録](execution-20261009.md)）。N1のraw listコピー/予約範囲を避ける配置と、V5の4 KiBページ表の生成/解除を追加しhost PASS・y/n build warning/error 0。V7の1×1 noop CL生成も追加し、固定4.2 XMLとのbyte照合・host・build PASS。これらを起動からはまだ呼ばない。次のsoftware段はV8のclear/store。hardwareはN0の版のQEMU回帰をQ1経由でT1へ、ユーザーの実機P0・V0・N0の写真で観測値を確認し、N1/V1のhardware処理と統合を進める。p001のQ1判定は残る。旧temp資料は旧P2 cacheから復旧し、Linux/Mesaの監査対象121 fileのSHA256一致、630定数の旧名が現行driverに0件を確認済み。実機はユーザー回答により後で実施。
Target: **ベータ4 以降**（2026-10-05 user「WS037, WS044,WS048,WS141, WS112, WS118, WS124, WS125, WS126, WS119, WS096, WS097, WS039, WS038, WS144, WS143, WS146,WS147, WS152,  WS119, WS080, は、ベータ4以降としてください。…WS027, WS015, WS047, WS028, WS017,  WS077, はキャンセルします。」）
<!-- awesome-plan-current:end -->

## 独立セッションの担当（2026-10-09）

- ユーザー「WS141をあなたが作業します。P1,P2とは別なセッションです。ws141/ws.mdはあなたがそのセッションが排他的に更新しますが、master.mdは更新しません。同じソースツリーを使いますが、作業は別なディレクトリで行い、パッチをあなたに提供するので、Q1がマージします。」に基づく。
- 2026-10-09のユーザーの再確認: パッチの提供先はQ1、統合もQ1。この分担を保持する。
- 追加承認（2026-10-09）: ユーザー「パッチの影響範囲が狭いので、あなたがマージしてOKです。」により、今回のN1/V5準備のmerge担当をCodexへ変更。mainへmerge commit `a326c5e24`で統合済み。最新mainとの統合版でもhost4試験PASS・rpi4 y/n build warning/error 0。詳細は[実行記録i05](execution-20261009.md#i05-mainへの統合2026-10-09)。
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
- zedBSD の GPU の interface への統合と、desktop（Keiland の compositor）の表示。Vulkan の実行器（compiler）は別の Phase または別の WS（i915 の WS031 にあたる）で決める。
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
| [p003](phase003/phase.md) | display（[design](rpi4-gpu-design.md) の N0〜N2・P1〜P3・P5、P4 は後）: firmware の framebuffer の readout と引き継ぎ、HVS の plane、pixelvalve・HDMI の mode set、vblank と page flip（i915 の resident display を手本に） | in-progress（N0とN1の配置/コピー準備を実装、build・host PASS。N1のwrite/readback/pollは実機N0観測後） | p002 | 6h〜 |
| [p004](phase004/phase.md) | V3D: power・MMU・buffer object、bin/render・TFUのjob、reset、fence（CSDはp006後） | in-progress（V5のページ表・V7のnoop CL生成を実装、固定XML照合・host/build PASS。電源/register/job投入は未実施） | p002（骨格出力でsoftware準備、hardwareは実機V0確認後） | 6h〜 |
| p005 | `drv_gpu_interface` への統合と desktop の表示（Keiland の compositor） | planning | p003・p004 | 4h〜 |
| p006 | 実行器（Vulkan・compiler）の方針の決定（別 WS にするか） | planning | p004 | 2h |
| p007 | 規約の全文の確認と最終の確認。**license と GPL の code との類似の監査**（字面・設計、道具と目視）、BLOB の移動の確認 | planning | 全て | 3〜4h |

## 要検討・ブロック（2026-10-05）

2026-10-05のユーザー指示で要検討・ブロックしていた。2026-10-09の独立Codexセッションへの割当で作業を再開。実機観測を必要とする段階の依存は、実機確認が後になるという回答に従い未達として保持する。
