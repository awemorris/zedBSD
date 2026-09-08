# zedBSD master plan

Last updated: 2026-09-09
Status: active

このファイルは現在の優先順位・WS状態・未解決事項の索引です。
詳細設計と受け入れは各WS/Phase、実行履歴はQueueと結果資料に置きます。
新しい経緯を本文へ追記し続けず、状態と参照先を更新します。

## 現在地

- 最新Queue：[q124](queue.md) は finished。WS025-p032〜p035のPC-98起動・Wi-Fi回帰修正は完了。
- WS025の必須p001〜p026とWS024は完了。p031の最終リファクタリング確認は未クリア。
- WS011のcommit confirmedはユーザー確認により完了。追加の実機受け入れ待ちは解除。
- 今回は計画整理。新しい実装Queueはまだ開始していない。

## Priority

2026-09-09のユーザー指定順。旧Priority wavesとp027〜p030の継続見送りを置き換える。

| 順位 | 対象 | 次に行う内容 |
| --- | --- | --- |
| 1 | [WS025](ws025-io-memory-cache/ws.md) p027〜p030 | NVMe多重発行、user page直接I/O、UAS、IMOD実機比較。p031の残件と現ソースを照合し、各Phaseの測定・機器条件を確認して実施する。 |
| 2 | [WS006](ws006-input/ws.md) | p009：通常TTY文字入力を維持し、旧consoleイベント・キー状態UAPIを撤去してevdev移行を完結。 |
| 3 | [WS022](ws022-elf-tls/ws.md) | ELF TLS/TCB契約、execローダ、スレッドごとのTLS確保と受け入れ。 |
| 4 | [WS019](ws019-installation/ws.md) | インストーラの入力成果物・配置・公開手順を確定し、実装とQEMU受け入れ。 |
| 5 | [WS002](ws002-services/ws.md) | p021：login実行失敗時のセッション終了・プロセス回収の堅牢化。 |
| 6 | [WS009](ws009-documentation/ws.md) | 未完了の設計・UAPI・起動・インストール等の説明を整備。 |

p027〜p030のq122での見送りは過去の実績として保持する。
今回の優先指定を、実機や性能改善の証拠が既にそろったという意味にはしない。
USB haltの既知バグは下記へ登録し、この指定順へ無断で割り込ませない。

## Workstream registry

Future Listへ移したWS013・WS015は次節で管理する。完了WSの詳細証拠は各WSを参照。

| WS | 内容 | 状態 | 残作業・参照点 |
| --- | --- | --- | --- |
| [WS001](ws001-posix/ws.md) | POSIX準拠 | 継続 | 準拠性台帳・コード規約の残件。 |
| [WS002](ws002-services/ws.md) | システムサービス | 基本完了・Priority 5 | p021：login実行失敗時のセッション終了・回収。 |
| [WS003](ws003-bringup/ws.md) | x86・PC-98実機対応 | 継続 | V13起動、実機SMP時計、NVMeインストール・起動の残件。 |
| [WS004](ws004-hardware/ws.md) | ハードウェア拡張 | 継続 | 主要USB/WLAN経路完了。NVMe実機・転送・ドライバ共通化等の後続項目を保持。 |
| [WS005](ws005-networking/ws.md) | ネットワーク・WLAN | 完了 | p012まで完了。追加回帰・二台同時実機検証はWS025-p033〜p035で完了。 |
| [WS006](ws006-input/ws.md) | 入力・evdev | Priority 2 | p009：旧consoleイベント・キー状態UAPI撤去。 |
| [WS007](ws007-graphics/ws.md) | グラフィックス・デスクトップ | 一部未クリア | p004の正確なGUI再現条件、amd64残件、統合試験。 |
| [WS008](ws008-noct/ws.md) | Noct・BeUI | 完了 | q063。 |
| [WS009](ws009-documentation/ws.md) | ドキュメント | Priority 6 | 設計・UAPI・起動・導入等の残件。 |
| [WS010](ws010-scripting/ws.md) | スクリプト・イメージツール | 完了 | q063。 |
| [WS011](ws011-net-config/ws.md) | ネットワーク設定コンソール | 完了（ユーザー確認） | commit confirmed完了。VLANキャンセル、bridgeはF-001へ移管。 |
| [WS012](ws012-service-console/ws.md) | サービス管理コンソール | 完了 | q018。 |
| [WS014](ws014-gpu/ws.md) | ネイティブGPU | 明示保留 | GPU設計の保留解除時に再開。 |
| [WS016](ws016-swap-control/ws.md) | 実行時swap制御 | 完了 | q021。 |
| [WS017](ws017-lfb-graphics/ws.md) | LFB描画高速化 | 依存待ち | WS022後にmmap・Xzed高速描画・受け入れ。 |
| [WS018](ws018-kernel-architecture/ws.md) | カーネル所有権・構成統一 | 完了 | p001〜p020。I/O後続はWS025。 |
| [WS019](ws019-installation/ws.md) | インストール・ディスク管理 | Priority 4 | 管理ツール完了。p004インストーラ、p005受け入れへ。 |
| [WS020](ws020-intel-mac/ws.md) | Intel Mac UEFI・Variant | 完了 | 2026-09-05ユーザー実機確認。 |
| [WS021](ws021-llvm-toolchain/ws.md) | LLVM・sysroot | 完了 | q064。 |
| [WS022](ws022-elf-tls/ws.md) | ELF TLS | Priority 3 | p001〜p003：ABI・exec・スレッドruntime。 |
| [WS023](ws023-x86-hal-style/ws.md) | x86 HALコーディング規約 | 完了 | q067。 |
| [WS024](ws024-unified-ufs/ws.md) | 単一64-bit UFS | 完了 | q102。 |
| [WS025](ws025-io-memory-cache/ws.md) | I/O・キャッシュ・物理メモリ | Priority 1 | 必須p001〜p026、回帰p032〜p035完了。p027〜p030を次に実施。p031最終確認は未クリア。 |

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
  エラー内容・機器・発生順序を採取し、書込み完了からUSB停止までの境界を調べる。
- AX211のfirmware内部assertは残存する。無限診断・復帰不能の修正と実機復帰試験は
  [WS025-p035結果](ws025-io-memory-cache/phase035-multi-radio-selection/results.md)を参照。

## 主要な依存関係

- WS025-p027〜p030：各Phaseの既存I/O契約、p031の残件確認、測定・対象機器。
- WS022 → WS017。WS006のevdev移行はデスクトップ各consumerへ反映する。
- WS019のインストーラ受け入れ → WS003の実機NVMeインストール・起動。
  WS013の完了済みブート設定基盤を使い、将来のRuntime CPARを前提にしない。
- WS001の準拠性とWS009の文書は各実装WSから更新する。

## 方針・履歴への索引

- [設計方針・決定の参照資料](master-design-policy.md)：独立実装・ライセンス境界、モジュール設計、ツールチェーン、個別設計判断。
- [整理前master全文](master-history-2026-09-09.md)：旧優先順位、milestone、依存図、完了経緯、保留の履歴。現在の実行指示には使わない。
- [計画運用](AGENTS.md)・[governance](governance.md)・[コーディング規約](coding-style.md)。
- [q124結果](queue.md)、[q123：p031未クリア](queue-q123.md)、[q122：WS025統合受け入れ](queue-q122.md)。
- [FS関連フォローアップ](fs-report-followups.md)。

長期目標は独立実装した共通OS基盤をデスクトップ・モバイル・組込み・サーバーへ提供すること。
設計詳細や過去の結果をこの索引へ重複掲載せず、所有する資料へリンクする。
