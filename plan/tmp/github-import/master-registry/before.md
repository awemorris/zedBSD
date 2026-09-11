<!-- awesome-plan project=zedbsd record=master -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/master.md`

# zedBSD master plan

Last updated: 2026-09-10
Status: active

このファイルは現在の優先順位・WS状態・未解決事項の索引です。
詳細設計と受け入れは各WS/Phase、実行履歴はQueueと結果資料に置きます。
新しい経緯を本文へ追記し続けず、状態と参照先を更新します。

## 現在地

- [q184](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/queue-q184.md)でamd64のテキスト／グラフィカルインストーラを受け入れ完了。正常系を完了基準とするユーザー判断でp029をcleared。追加の[WS019-p050](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase050-pc98-graphic-fat/phase.md)もq186で完了。PC98・IDE HDD 2台でグラフィカルFATインストール後、ターゲット単独起動・rootログインを確認。GPT/nativeは対象外。

- WS025の旧必須Phase受け入れは履歴として保持。現行ではp023/p037のvmap構成が撤去・置換されているため、旧実装の存在を主張しない。p031のユーザー受け入れは維持。p032現行対応は未着手。
- WS011のcommit confirmedはユーザー確認により完了。追加の実機受け入れ待ちは解除。
- Priority全件の自走をユーザー承認済み。進められないPhaseはunclearedとし、次の実行可能なWSへ進む。
- WS025は67b28ce0の修正を反映。p027完了、p028キャンセル、p029修正後確認待ち、p030実装確認済み/比較確認残、p032未着手。WS006・WS022・WS019・WS002の完了は維持。今回ソース変更・実行試験・新規目標作成は行わない。

## Priority

2026-09-09のユーザー指定順。旧Priority wavesとp027〜p030の継続見送りを置き換える。

| 順位 | 対象 | 次に行う内容 |
| --- | --- | --- |
| 1 | [WS025](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/ws.md) | 67b28ce0修正後：p027完了、p028キャンセル、p029修正後確認待ち、p030現行IMOD実装確認済み（動作・比較確認残）、p032未着手。HAL名称統一は保持、旧vmap移設案は撤回、p038固定入口は現行APIでの確認残。修正後照合（ローカル資料: `ws025/post-rollback-review.md`）。 |
| 2 | [WS006](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/ws.md) | 完了（q147）。旧consoleイベントUAPI撤去、paired USB修正とXzed/Noct/BeUI受け入れ。 |
| 3 | [WS022](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws022-elf-tls/ws.md) | 完了（q128）。ELF TLS/TCB、exec、pthread、両x86受け入れ。 |
| 4 | [WS019](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/ws.md) | 共存経路p004/p005、元選択p027・属P性付きコピーp028・GPT処理p030完了。p006/p049で両モードの実インストールを受け入れ済み。p007のUFSスワップ負荷・断片化も受け入れ済み。p029も完了。追加p050のPC98グラフィカルFATインストールとターゲット単独ログインもq186で完了。 |
| 5 | [WS002](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/ws.md) | 完了。p021は現行試験合格とユーザー判断でcleared。過去の未再現事象はBUG-012に保持。 |
| 6 | [WS009](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws009-documentation/ws.md) | p008をq190で完了し、現行実装の文書整備完了。DOC-54のみWS014の手動保留に依存し、WSは未完了のまま保持。実行可能Phaseなし。 |

p027〜p030のq122での見送りは過去の実績として保持する。
追加必須項目（2026-09-09ユーザー指示）：[WS004-p050](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws004-hardware/phase050-nvme-multiple-controllers/phase.md)
はq187で完了。NVMeの1コントローラ制限を解除し、両列挙順の起動、
独立・同時I/O、永続化、検出失敗の分離を検証済み。
WS019は現行テキスト画面で完成を先行し、続いて[p029](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase029-graphic-installer/phase.md)
で共通処理を使うBeUI版 `/sbin/zedinst-graphic` を追加する。テキスト版の最終配置も
`/sbin/zedinst` とする。添付の背景・デザインによる640x480 RGB24画面を事前合成する。
今回の優先指定を、実機や性能改善の証拠が既にそろったという意味にはしない。
USB haltの既知バグは下記へ登録し、この指定順へ無断で割り込ませない。

## Workstream registry

Future Listへ移したWS013・WS015は次節で管理する。完了WSの詳細証拠は各WSを参照。

| WS | 内容 | 状態 | 残作業・参照点 |
| --- | --- | --- | --- |
| [WS001](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/ws.md) | POSIX準拠 | 継続 | 準拠性台帳・コード規約の残件。 |
| [WS002](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/ws.md) | システムサービス | 完了 | p021をユーザー判断でcleared。p023/p024完了、POSIXの引継ぎと既知バグの再発条件は保持。 |
| [WS003](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws003-bringup/ws.md) | x86・PC-98実機対応 | 継続 | V13起動、実機SMP時計、NVMeインストール・起動の残件。 |
| [WS004](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws004-hardware/ws.md) | ハードウェア拡張 | 継続 | 主要USB/WLAN経路完了。NVMe実機・転送・ドライバ共通化等の後続項目を保持。 |
| [WS005](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws005-networking/ws.md) | ネットワーク・WLAN | 完了 | p012まで完了。追加回帰・二台同時実機検証はWS025-p033〜p035で完了。 |
| [WS006](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/ws.md) | 入力・evdev | 完了 | q147。両USB構成の通常ビルドで実Xzed/PTYとUSB-root/HID受け入れ。 |
| [WS007](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws007-graphics/ws.md) | グラフィックス・デスクトップ | 一部未クリア | p004の正確なGUI再現条件、amd64残件、統合試験。 |
| [WS008](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws008-noct/ws.md) | Noct・BeUI | 完了 | q063。 |
| [WS009](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws009-documentation/ws.md) | ドキュメント | 手動保留項目待ち | p001〜p008完了。残るDOC-54はWS014 GPUの保留解除後にPhase化。 |
| [WS010](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws010-scripting/ws.md) | スクリプト・イメージツール | 完了 | q063。 |
| [WS011](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/ws.md) | ネットワーク設定コンソール | 完了（ユーザー確認） | commit confirmed完了。VLANキャンセル、bridgeはF-001へ移管。 |
| [WS012](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws012-service-console/ws.md) | サービス管理コンソール | 完了 | q018。 |
| [WS014](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws014-gpu/ws.md) | ネイティブGPU | 明示保留 | GPU設計の保留解除時に再開。 |
| [WS016](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws016-swap-control/ws.md) | 実行時swap制御 | 完了 | q021。 |
| [WS017](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws017-lfb-graphics/ws.md) | LFB描画高速化 | 依存待ち | WS022後にmmap・Xzed高速描画・受け入れ。 |
| [WS018](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws018-kernel-architecture/ws.md) | カーネル所有権・構成統一 | 完了 | p001〜p020。I/O後続はWS025。 |
| [WS019](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/ws.md) | インストール・ディスク管理 | Priority 4 | 完了（q186）。amd64共存・専用・グラフィカル版とPC98 FATインストールを受け入れ済み。 |
| [WS020](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws020-intel-mac/ws.md) | Intel Mac UEFI・Variant | 完了 | 2026-09-05ユーザー実機確認。 |
| [WS021](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws021-llvm-toolchain/ws.md) | LLVM・sysroot | 完了 | q064。 |
| [WS022](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws022-elf-tls/ws.md) | ELF TLS | 完了 | q128：p001〜p003、両x86 QEMU・dynamic回帰、PC98確認。 |
| [WS023](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws023-x86-hal-style/ws.md) | x86 HALコーディング規約 | 完了 | q067。 |
| [WS024](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws024-unified-ufs/ws.md) | 単一64-bit UFS | 完了 | q102。 |
| [WS025](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/ws.md) | I/O・キャッシュ・物理メモリ | 未完了 | p028キャンセル。p029修正後確認待ち、p030実装確認済み/比較残、p032未着手。p023/p037の旧vmap構成は現行修正で撤去・置換。p038確認残。現行状態（ローカル資料: `ws025/post-rollback-review.md`）。 |
| [WS026](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws026-test-maintenance/ws.md) | テスト資産の整理 | 計画のみ | 不要・重複テストの整理と現行ソースへの追随。Phase未詳細化・Queue未投入。 |

## Future List(やりたいことリスト)

現在のPriority・実行Queueから外した将来項目。着手対象として選び直した時点で計画を更新する。
WS/Phase IDと完了済みの実績は消去・再利用しない。

| ID | 項目 | 保持する内容・再開点 |
| --- | --- | --- |
| F-001 | bridge | [旧WS011-p004](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase004-vlan-bridge/phase.md)からbridgeだけを移管。L2転送、member所有権、設定・永続化を独立して設計する。VLANは含めない。 |
| F-002 | [WS013：CPAR](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws013-containers/ws.md) | Runtimeの名前空間・隔離、CLI/build、サービスコンテナを将来へ。完了済みp002〜p006のブート設定基盤は現行機能として維持する。 |
| F-003 | [WS015：μITRONリアルタイム領域](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws015-muitron-rt/ws.md) | 互換プロファイル、RT/POSIX境界、常駐実行、通信・障害・時間保証の設計を将来へ移管。 |

VLANは2026-09-09ユーザー指示によりキャンセル。Future Listにも残さない。
旧MB-010のVLAN/bridge一括保留は終了し、bridgeはF-001として扱う。

## Known bugs

正本は [既知バグ台帳](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/known-bugs.md)。再現済み・ユーザー報告・解決済みを区別して記録する。

- **BUG-010：実機USBブート後にhaltするとUSBエラーが出る**。ユーザー報告、未修正。
  [WS002-p023](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/phase023-usb-boot-halt/results.md)でq135にQEMU再現・修正済み。実機とEHCI/UHCIの追加確認は残る。
- AX211のfirmware内部assertは残存する。無限診断・復帰不能の修正と実機復帰試験は
  [WS025-p035結果](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/phase035-multi-radio-selection/results.md)を参照。

## 主要な依存関係

- WS025-p027〜p030：各Phaseの既存I/O契約、測定・対象機器。p031の先行条件はq188で充足。
- WS022 → WS017。WS006のevdev移行はデスクトップ各consumerへ反映する。
- WS019のインストーラ受け入れ → WS003の実機NVMeインストール・起動。
  WS013の完了済みブート設定基盤を使い、将来のRuntime CPARを前提にしない。
- WS001の準拠性とWS009の文書は各実装WSから更新する。

## 方針・履歴への索引

- [設計方針・決定の参照資料](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/master-design-policy.md)：独立実装・ライセンス境界、モジュール設計、ツールチェーン、個別設計判断。
- [整理前master全文](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/master-history-2026-09-09.md)：旧優先順位、milestone、依存図、完了経緯、保留の履歴。現在の実行指示には使わない。
- [計画運用](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/AGENTS.md)・[governance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/governance.md)・[コーディング規約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/coding-style.md)。
- [q124結果](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/queue-q124.md)、[q123：p031未クリア](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/queue-q123.md)、[q122：WS025統合受け入れ](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/queue-q122.md)。
- [FS関連フォローアップ](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/fs-report-followups.md)。

長期目標は独立実装した共通OS基盤をデスクトップ・モバイル・組込み・サーバーへ提供すること。
設計詳細や過去の結果をこの索引へ重複掲載せず、所有する資料へリンクする。

### Priority execution clarification (2026-09-09)

- 実装可能な作業を先行し、WS006 EHCI/UHCIのQEMU解析は後段へ回すが、現在のゴール内で修正を目指す。
- WS025 p027–p030は実機不在を実装の停止条件にしない。QEMU機能検証と実機性能の未測定を分離する。
- ユーザーはUAS実機を所有していない。ローカルQEMUには`usb-uas`があるため検証経路を確認する。QEMU検証が不可能と確認された場合にUASをFuture Listへ移す。
- WS019 p008の旧形式証跡に代えてp014で現行UFS formatterを再検証・必要改修し、installerの前提とする。
