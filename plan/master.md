# zedBSD master plan

Last updated: 2026-09-10
Status: active

このファイルは現在の優先順位・WS状態・未解決事項の索引です。
詳細設計と受け入れは各WS/Phase、実行履歴はQueueと結果資料に置きます。
新しい経緯を本文へ追記し続けず、状態と参照先を更新します。

## 現在地

- [q184](queue-q184.md)でamd64のテキスト／グラフィカルインストーラを受け入れ完了。正常系を完了基準とするユーザー判断でp029をcleared。追加の[WS019-p050](ws019-installation/phase050-pc98-graphic-fat/phase.md)もq186で完了。PC98・IDE HDD 2台でグラフィカルFATインストール後、ターゲット単独起動・rootログインを確認。GPT/nativeは対象外。

- WS025の必須p001〜p026とWS024は完了。p031はq188で手動レビュー・既存デバイス動作・修復テストのユーザー受け入れにより完了。p032は現行PC98バイナリの実機不動作というユーザー報告で再開。過去のQEMU合格は維持する。
- WS011のcommit confirmedはユーザー確認により完了。追加の実機受け入れ待ちは解除。
- Priority全件の自走をユーザー承認済み。進められないPhaseはunclearedとし、次の実行可能なWSへ進む。
- WS025-p027はq139で完了、p029はq230で完了、p028/p030は未完了。実機不要の実装とQEMU検証を継続対象とする。QEMUにusb-uasがあり、利用不能と確認した場合のみUASをFutureへ移す。WS006はq147、WS022はq128で完了、WS019はamd64と追加のPC98 FAT対応まで完了。

## Priority

2026-09-09のユーザー指定順。旧Priority wavesとp027〜p030の継続見送りを置き換える。

| 順位 | 対象 | 次に行う内容 |
| --- | --- | --- |
| 1 | [WS025](ws025-io-memory-cache/ws.md) p027〜p030、再開p032 | p027/p029とHAL共通化p037完了。p028は専門家レビュー待ちで性能作業を保留、既定無効。ユーザー指示で[HAL固定入口p038](ws025-io-memory-cache/phase038-hal-fixed-entry/phase.md)を優先。固定入口移行・amd64/i386実例外テスト済み、SPARC初回フレーム方針は確認待ち（入力CPU 2.82秒/3.21秒、出力0.86秒/0.96秒：無効/有効、q291カーソル出力。q287の間欠的login/exec ENOSPCは未解決）。[直接I/O結果](ws025-io-memory-cache/phase028-direct-user-io/results.md)。p030はUSB2/3の3設定比較済み、[残る実機/WLAN・回復比較](ws025-io-memory-cache/phase030-imod-measurement/remaining-evidence.md)を継続。p032はPC98実機起動の観測待ち。 |
| 2 | [WS006](ws006-input/ws.md) | 完了（q147）。旧consoleイベントUAPI撤去、paired USB修正とXzed/Noct/BeUI受け入れ。 |
| 3 | [WS022](ws022-elf-tls/ws.md) | 完了（q128）。ELF TLS/TCB、exec、pthread、両x86受け入れ。 |
| 4 | [WS019](ws019-installation/ws.md) | 共存経路p004/p005、元選択p027・属P性付きコピーp028・GPT処理p030完了。p006/p049で両モードの実インストールを受け入れ済み。p007のUFSスワップ負荷・断片化も受け入れ済み。p029も完了。追加p050のPC98グラフィカルFATインストールとターゲット単独ログインもq186で完了。 |
| 5 | [WS002](ws002-services/ws.md) | 完了。p021は現行試験合格とユーザー判断でcleared。過去の未再現事象はBUG-012に保持。 |
| 6 | [WS009](ws009-documentation/ws.md) | p008をq190で完了し、現行実装の文書整備完了。DOC-54のみWS014の手動保留に依存し、WSは未完了のまま保持。実行可能Phaseなし。 |

p027〜p030のq122での見送りは過去の実績として保持する。
追加必須項目（2026-09-09ユーザー指示）：[WS004-p050](ws004-hardware/phase050-nvme-multiple-controllers/phase.md)
はq187で完了。NVMeの1コントローラ制限を解除し、両列挙順の起動、
独立・同時I/O、永続化、検出失敗の分離を検証済み。
WS019は現行テキスト画面で完成を先行し、続いて[p029](ws019-installation/phase029-graphic-installer/phase.md)
で共通処理を使うBeUI版 `/sbin/zedinst-graphic` を追加する。テキスト版の最終配置も
`/sbin/zedinst` とする。添付の背景・デザインによる640x480 RGB24画面を事前合成する。
今回の優先指定を、実機や性能改善の証拠が既にそろったという意味にはしない。
USB haltの既知バグは下記へ登録し、この指定順へ無断で割り込ませない。

## Workstream registry

Future Listへ移したWS013・WS015は次節で管理する。完了WSの詳細証拠は各WSを参照。

| WS | 内容 | 状態 | 残作業・参照点 |
| --- | --- | --- | --- |
| [WS001](ws001-posix/ws.md) | POSIX準拠 | 継続 | 準拠性台帳・コード規約の残件。 |
| [WS002](ws002-services/ws.md) | システムサービス | 完了 | p021をユーザー判断でcleared。p023/p024完了、POSIXの引継ぎと既知バグの再発条件は保持。 |
| [WS003](ws003-bringup/ws.md) | x86・PC-98実機対応 | 継続 | V13起動、実機SMP時計、NVMeインストール・起動の残件。 |
| [WS004](ws004-hardware/ws.md) | ハードウェア拡張 | 継続 | 主要USB/WLAN経路完了。NVMe実機・転送・ドライバ共通化等の後続項目を保持。 |
| [WS005](ws005-networking/ws.md) | ネットワーク・WLAN | 完了 | p012まで完了。追加回帰・二台同時実機検証はWS025-p033〜p035で完了。 |
| [WS006](ws006-input/ws.md) | 入力・evdev | 完了 | q147。両USB構成の通常ビルドで実Xzed/PTYとUSB-root/HID受け入れ。 |
| [WS007](ws007-graphics/ws.md) | グラフィックス・デスクトップ | 一部未クリア | p004の正確なGUI再現条件、amd64残件、統合試験。 |
| [WS008](ws008-noct/ws.md) | Noct・BeUI | 完了 | q063。 |
| [WS009](ws009-documentation/ws.md) | ドキュメント | 手動保留項目待ち | p001〜p008完了。残るDOC-54はWS014 GPUの保留解除後にPhase化。 |
| [WS010](ws010-scripting/ws.md) | スクリプト・イメージツール | 完了 | q063。 |
| [WS011](ws011-net-config/ws.md) | ネットワーク設定コンソール | 完了（ユーザー確認） | commit confirmed完了。VLANキャンセル、bridgeはF-001へ移管。 |
| [WS012](ws012-service-console/ws.md) | サービス管理コンソール | 完了 | q018。 |
| [WS014](ws014-gpu/ws.md) | ネイティブGPU | 明示保留 | GPU設計の保留解除時に再開。 |
| [WS016](ws016-swap-control/ws.md) | 実行時swap制御 | 完了 | q021。 |
| [WS017](ws017-lfb-graphics/ws.md) | LFB描画高速化 | 依存待ち | WS022後にmmap・Xzed高速描画・受け入れ。 |
| [WS018](ws018-kernel-architecture/ws.md) | カーネル所有権・構成統一 | 完了 | p001〜p020。I/O後続はWS025。 |
| [WS019](ws019-installation/ws.md) | インストール・ディスク管理 | Priority 4 | 完了（q186）。amd64共存・専用・グラフィカル版とPC98 FATインストールを受け入れ済み。 |
| [WS020](ws020-intel-mac/ws.md) | Intel Mac UEFI・Variant | 完了 | 2026-09-05ユーザー実機確認。 |
| [WS021](ws021-llvm-toolchain/ws.md) | LLVM・sysroot | 完了 | q064。 |
| [WS022](ws022-elf-tls/ws.md) | ELF TLS | 完了 | q128：p001〜p003、両x86 QEMU・dynamic回帰、PC98確認。 |
| [WS023](ws023-x86-hal-style/ws.md) | x86 HALコーディング規約 | 完了 | q067。 |
| [WS024](ws024-unified-ufs/ws.md) | 単一64-bit UFS | 完了 | q102。 |
| [WS025](ws025-io-memory-cache/ws.md) | I/O・キャッシュ・物理メモリ | Priority 1 | p001〜p027、p031、p033〜p035完了。p028/p030未完了、p032実機対応。p029はq230でdirty媒体喪失回復を含むHS/SS QEMU受け入れ完了。[現状・設計](ws025-io-memory-cache/phase029-uas/lost-media-teardown.md)。 |
| [WS026](ws026-test-maintenance/ws.md) | テスト資産の整理 | 計画のみ | 不要・重複テストの整理と現行ソースへの追随。Phase未詳細化・Queue未投入。 |

## Future List(やりたいことリスト)

現在のPriority・実行Queueから外した将来項目。着手対象として選び直した時点で計画を更新する。
WS/Phase IDと完了済みの実績は消去・再利用しない。

| ID | 項目 | 保持する内容・再開点 |
| --- | --- | --- |
| F-001 | bridge | [旧WS011-p004](ws011-net-config/phase004-vlan-bridge/phase.md)からbridgeだけを移管。L2転送、member所有権、設定・永続化を独立して設計する。VLANは含めない。 |
| F-002 | [WS013：CPAR](ws013-containers/ws.md) | Runtimeの名前空間・隔離、CLI/build、サービスコンテナを将来へ。完了済みp002〜p006のブート設定基盤は現行機能として維持する。 |
| F-003 | [WS015：μITRONリアルタイム領域](ws015-muitron-rt/ws.md) | 互換プロファイル、RT/POSIX境界、常駐実行、通信・障害・時間保証の設計を将来へ移管。 |

VLANは2026-09-09ユーザー指示によりキャンセル。Future Listにも残さない。
旧MB-010のVLAN/bridge一括保留は終了し、bridgeはF-001として扱う。

## Known bugs

正本は [既知バグ台帳](known-bugs.md)。再現済み・ユーザー報告・解決済みを区別して記録する。

- **BUG-010：実機USBブート後にhaltするとUSBエラーが出る**。ユーザー報告、未修正。
  [WS002-p023](ws002-services/phase023-usb-boot-halt/results.md)でq135にQEMU再現・修正済み。実機とEHCI/UHCIの追加確認は残る。
- AX211のfirmware内部assertは残存する。無限診断・復帰不能の修正と実機復帰試験は
  [WS025-p035結果](ws025-io-memory-cache/phase035-multi-radio-selection/results.md)を参照。

## 主要な依存関係

- WS025-p027〜p030：各Phaseの既存I/O契約、測定・対象機器。p031の先行条件はq188で充足。
- WS022 → WS017。WS006のevdev移行はデスクトップ各consumerへ反映する。
- WS019のインストーラ受け入れ → WS003の実機NVMeインストール・起動。
  WS013の完了済みブート設定基盤を使い、将来のRuntime CPARを前提にしない。
- WS001の準拠性とWS009の文書は各実装WSから更新する。

## 方針・履歴への索引

- [設計方針・決定の参照資料](master-design-policy.md)：独立実装・ライセンス境界、モジュール設計、ツールチェーン、個別設計判断。
- [整理前master全文](master-history-2026-09-09.md)：旧優先順位、milestone、依存図、完了経緯、保留の履歴。現在の実行指示には使わない。
- [計画運用](AGENTS.md)・[governance](governance.md)・[コーディング規約](coding-style.md)。
- [q124結果](queue-q124.md)、[q123：p031未クリア](queue-q123.md)、[q122：WS025統合受け入れ](queue-q122.md)。
- [FS関連フォローアップ](fs-report-followups.md)。

長期目標は独立実装した共通OS基盤をデスクトップ・モバイル・組込み・サーバーへ提供すること。
設計詳細や過去の結果をこの索引へ重複掲載せず、所有する資料へリンクする。

### Priority execution clarification (2026-09-09)

- 実装可能な作業を先行し、WS006 EHCI/UHCIのQEMU解析は後段へ回すが、現在のゴール内で修正を目指す。
- WS025 p027–p030は実機不在を実装の停止条件にしない。QEMU機能検証と実機性能の未測定を分離する。
- ユーザーはUAS実機を所有していない。ローカルQEMUには`usb-uas`があるため検証経路を確認する。QEMU検証が不可能と確認された場合にUASをFuture Listへ移す。
- WS019 p008の旧形式証跡に代えてp014で現行UFS formatterを再検証・必要改修し、installerの前提とする。
