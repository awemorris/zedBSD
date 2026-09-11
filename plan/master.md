# zedBSD master plan

[GitHub Project — zedBSD / Awesome Plan](https://github.com/users/awemorris/projects/2)

<!-- traceability:start -->

## Objectives

- **O1**：寛容なライセンスで企業が自由に使いやすいUNIX互換システムを作り、GPLのLinuxカーネルに依存せず、Linux/Androidを置き換え可能な水準の完成度で提供する。
- **O2**：デスクトップ、ラップトップ、SBC、タブレット、モバイルなど様々なスケールで動作するカーネルとユーザランドを提供し、開発者が独自ディストリビューションを自由にカスタマイズ・リブランディング・配布できるようにする。
- **O3**：UNIX/BSD/Linuxの歴史的遺産から現代のシステムに必要なエッセンスを抽出し、networkd、netconf、serviceなどをシンプルで一貫した仕組みとして再実装する。
- **O4**：ページベースMMUを備える32bit/64bitコンピュータへUNIX互換OSを確実に移植できる、明確で最小限のHALを定義し、人類の共有知とする。レトロコンピュータへの移植でも実証する。
- **O5**：AI時代のOSSのあり方を、大規模なAI活用開発を通じて探索・思索し、成果・失敗・人間の判断を再利用可能な知見として共有する。

## Milestone Goals

Objectives → Milestone Goals → WS → Phase → Queue試行/結果を対応付ける。番号は着手順ではない。Milestoneの達成は所属WSの完了数ではなく、到達点の証拠で判定する。

| Milestone | 対応Objective | 到達点・受け入れの核 | Primary WS |
| --- | --- | --- | --- |
| **MG001：継続開発できる基盤が揃う** | O4, O5 | 文書化した環境でビルドでき、設計境界・規約・試験・制限を追跡できる。 | [ws009](https://github.com/awemorris/zedBSD/issues/10), [ws010](https://github.com/awemorris/zedBSD/issues/11), [ws021](https://github.com/awemorris/zedBSD/issues/22), [ws023](https://github.com/awemorris/zedBSD/issues/24), [ws026](https://github.com/awemorris/zedBSD/issues/27) |
| **MG002：UNIXアプリケーションの実行基盤が成立する** | O1 | プロセス・メモリ・libc・ローダ/TLSの対応範囲を、互換性台帳と代表アプリの結果で確認できる。 | [ws001](https://github.com/awemorris/zedBSD/issues/2), [ws022](https://github.com/awemorris/zedBSD/issues/23) |
| **MG003：対象機へ導入して単独起動できる** | O2, O4 | 合意した機種・媒体でインストール後の単独起動・ログインを確認できる。実機とQEMUの証拠を区別する。 | [ws003](https://github.com/awemorris/zedBSD/issues/4), [ws004](https://github.com/awemorris/zedBSD/issues/5), [ws019](https://github.com/awemorris/zedBSD/issues/20), [ws020](https://github.com/awemorris/zedBSD/issues/21) |
| **MG004：データを保持しメモリ/ストレージを実用的に使える** | O1, O2 | 永続化、低メモリ時の進行、媒体世代、既定構成の性能を合意した用途で確認できる。 | [ws016](https://github.com/awemorris/zedBSD/issues/17), [ws024](https://github.com/awemorris/zedBSD/issues/25), [ws025](https://github.com/awemorris/zedBSD/issues/26) |
| **MG005：シンプルで一貫したネットワーク/サービス管理を利用できる** | O1, O2, O3 | networkd/netconf/serviceの責務・設定・操作が一貫し、永続化と失敗後の復旧を確認できる。 | [ws002](https://github.com/awemorris/zedBSD/issues/3), [ws005](https://github.com/awemorris/zedBSD/issues/6), [ws011](https://github.com/awemorris/zedBSD/issues/12), [ws012](https://github.com/awemorris/zedBSD/issues/13) |
| **MG006：グラフィカルな操作環境を利用できる** | O2 | 入力・描画・ウィンドウ・端末・GUIツールの一連の操作を合意した環境で確認できる。 | [ws006](https://github.com/awemorris/zedBSD/issues/7), [ws007](https://github.com/awemorris/zedBSD/issues/8), [ws008](https://github.com/awemorris/zedBSD/issues/9), [ws014](https://github.com/awemorris/zedBSD/issues/15), [ws017](https://github.com/awemorris/zedBSD/issues/18), [ws005](ws005/ws.md) |
| **MG007：用途別の独自ディストリビューションを構成・配布できる** | O1, O2 | 第三者が用途別に構成し独自ブランドでビルド・配布できる。Linux/Android代替の対象用途・機能/品質基準を具体化し実証する。 | [ws013](https://github.com/awemorris/zedBSD/issues/14), [ws015](https://github.com/awemorris/zedBSD/issues/16) |
| **MG008：最小HALの移植契約を公開し異なる機種で実証できる** | O4 | 32bit/64bitのHAL契約・移植手順と異種/レトロ機での実証を公開し、移植者が必要な実装を判断できる。 | [ws018](https://github.com/awemorris/zedBSD/issues/19) |
| **MG009：AI活用OSS開発の経験を検証可能な知見として公開できる** | O5 | 設計権限・レビュー・変更追跡・失敗からの回復について事例と根拠を公開し、知見と未解決の問いを整理する。 | 未割当：成果を担う作業の具体化が必要 |

### 未充足・保留の扱い

- MG007：既存WS013/WS015は一部の用途別機能だけを担う。リブランディング/配布とLinux/Android代替の実証全体は未充足。Future扱いは解除しない。
- MG008：WS018の旧完了は移植契約全体の完成を証明しない。現行HAL契約の公開・異種機での実証範囲を確認する必要がある。
- MG009：WS009/WS026は関連する基盤・資料を提供するが、事例研究と知見公開の担当作業は未定義。既存WSへ無断で追加しない。
- MG002の互換範囲、MG003の機種/媒体、MG004の用途別性能などの詳細基準は既存証拠から確定する。正式なUNIX認証やAndroidアプリ互換方式はこの階層だけでは決定しない。
- 全Milestoneは構造設定段階であり、今回completedとは判定しない。既存WSの完了・停止・取消し・保留を維持する。

## Current Focused Goals

| Goal | 当面の成果 | Milestone | 担当 |
| --- | --- | --- | --- |
| fg004 | PC98、Dell Latitude 5320、Let's Note SV7、Let's Note LX6でインストールが行える | MG003 | [ws003](ws003/ws.md) |
| fg005 | 有線LAN常駐管理、起動時の接続待機、DEへのネットワーク状態通知 | MG005 / MG006 | [ws005](ws005/ws.md) |
| fg006 | PC-9821V13での起動改善：LBA0実行後のビープ停止を解消し、通常起動を進める | MG003 / MG008 | [ws003-p022](ws003/phase022/phase.md) → [ws003-p023](ws003/phase023/phase.md) → [ws003-p024](ws003/phase024/phase.md) |
| fg007 | HAL契約の可読性改善：コンソールAPIの集約、アロケータの kernel_alloc/kernel_free 化、kernel_entry() 前関数の prekern 命名 | MG008 / MG001 | 未定（WS018は完了。再開か新WSかの判断が必要） |

2026-09-11ユーザー指定。4機種は順位ではない。旧fg001〜fg003は履歴のまま。旧Priorityリストを再作成せず、順序付けは未指定として保持する。

#### fg007：HALリファクタリングの確定事項

2026-09-11ユーザー指示。正本は `include/hal/hal.h` のXXX注釈。以下は本文で確定した決定であり、
実行許可ではない。Queue投入前に下の未決事項を確定する。

- コンソール出力を `hal_cons_write()` に集約し、row・column・attrib を引数へ追加する。
  `hal_cons_write_at()`、`hal_cons_write_at_attr()`、`hal_cons_write_n()`、`hal_cons_clear_row()`、
  `hal_cons_clear_to_eol()`、`hal_cons_clear_to_eol_at()`、`hal_cons_save_state()` を削除する。
  `hal_cons_write_n_at()`、`hal_cons_restore_terminal()`、`struct hal_cons_state` は削除済み。
- `hal_cons_get_size(unsigned *cols, unsigned *rows)` を追加し、`HAL_CONS_COLUMNS` と
  `HAL_CONS_ROWS` を廃止する。`HAL_CONS_NORMAL_ATTRIBUTE` を `HAL_CONS_ATTRIB_NORMAL` へ改名する。
- `hal_cons_set_event_mode(int enable)` を追加する。`enum hal_cons_mode` と `hal_cons_set_mode()`
  は削除済みで、イベントモードの選択はこの関数が担う。
- `struct hal_key_event` を廃止し、`hal_cons_read_event()` と `hal_cons_poll_event()` の引数を
  `keysym` と `flags` へ展開する。入力能力の申告は廃止し、スタブでも常時サポートとする。
- `hal_set_allocator()`、`hal_malloc()`、`hal_free()` を廃止し、HALがカーネルの `kernel_alloc()` と
  `kernel_free()` を直接呼ぶ。
- `kernel_entry()` より前に呼ばれる関数は `prekern_` を接頭辞とする。それ以外は `kernel_entry()`
  以降から呼ばれるものとして扱う。例：`bsp_boot_init` → `prekern_bsp_boot_init`、
  `pcat_cons_init` → `prekern_pcat_cons_init`。これによりアロケータ使用可否が名前で判別できる。

未決の判断：

- `amd64_cpu_init()` と `amd64_descriptor_init()` は `amd64_cmain()` と `amd64_ap_entry()` の
  両方から呼ばれる。AP側は `kernel_entry()` 以降のため `prekern_` を付けられない。
  分割・別名・規約の例外のいずれを取るか未定。
- `prekern_` を推移的に適用するか、cmain から直接呼ばれる非static関数に限るか未定。
- `hal_*` と `kernel_*` の公開APIは対象外とする想定。`hal_puts()` は前後どちらからも呼ばれる。
- 担当WS。WS018（MG008）は完了のため、再開するか新WSを立てるかはユーザー判断。

影響範囲の実測（plan/ と build/ の複製を除く）：コンソール定数198箇所、write/clear系75箇所、
`hal_key_event` 関連59箇所、アロケータ278箇所。`prekern_` 対象は5アーキの起動前経路で約50関数。
HAL内のアロケータ使用26箇所のうち起動経路は6箇所のみで、残る20箇所は実行時のため対象外。


### トレーサビリティの読み方

各WSのPrimaryは一つ。Relatedは横断的な貢献先であり親ではない。各Phaseの親WSは既存IDから追跡し、達成を裏付けるのはそのPhaseの現行結果・成果物である。取消しPhaseと置換済み実装は現在の達成証拠に数えない。Phaseの実行/結果は既存Queueと記録へ辿る。

<!-- traceability:end -->


Last updated: 2026-09-11
Status: active

このファイルは現在の優先順位・WS状態・未解決事項の索引です。
詳細設計と受け入れは各WS/Phase、実行履歴はQueueと結果資料に置きます。
新しい経緯を本文へ追記し続けず、状態と参照先を更新します。

## 現在地

2026-09-11のユーザー指示により現在のPriorityリストを削除。WS025のp029/p030/p032/p038をcleared、WS025をcompletedとし、既存completedのWS019/WS006/WS022/WS002とともに閉鎖する。未実施の検証をPASSへ変更せず、今回の計画上の受け入れとして記録する。q303はfinished/stoppedのまま。active QueueとPriorityリストはない。後続のユーザー指示でfg004（インストーラ）、fg005（ネットワーク）、fg006（V13起動改善）、fg007（HALリファクタリング）を追加した。

旧Priority順はWS025 → WS006 → WS022 → WS019 → WS002 → WS009。2026-09-11に削除済みであり、実行順・承認として再利用しない。WS009/DOC-54のWS014手動保留は維持する。

## Workstream registry

Future Listへ移したWS013・WS015は次節で管理する。完了WSの詳細証拠は各WSを参照。

| WS | Primary Milestone | 内容 | 状態 | 残作業・参照点 |
| --- | --- | --- | --- | --- |
| [WS001](ws001/ws.md) | MG002 | POSIX準拠 | 継続 | 準拠性台帳・コード規約の残件。 |
| [WS002](ws002/ws.md) | MG005 | システムサービス | completed | p021をユーザー判断でcleared。p023/p024完了、POSIXの引継ぎと既知バグの再発条件は保持。 2026-09-11ユーザー指示で閉鎖。 |
| [WS003](ws003/ws.md) | MG003 | x86・PC-98実機対応 | incomplete | fg004の4機種インストーラとfg006のV13起動改善。p022→p023→p024をユーザー指定の実行Phaseとする。その他の残件を保持。 |
| [WS004](ws004/ws.md) | MG003 | ハードウェア拡張 | 継続 | 主要USB/WLAN経路完了。NVMe実機・転送・ドライバ共通化等の後続項目を保持。 |
| [WS005](ws005/ws.md) | MG005 | ネットワーク・WLAN | incomplete | fg005: net lan、network-enable（どちらかIP・既定30秒・timeoutでも起動継続）、DE状態通知をp013〜p017で計画。既存p001〜p012の完了は維持。 |
| [WS006](ws006/ws.md) | MG006 | 入力・evdev | completed | q147。両USB構成の通常ビルドで実Xzed/PTYとUSB-root/HID受け入れ。 2026-09-11ユーザー指示で閉鎖。 |
| [WS007](ws007/ws.md) | MG006 | グラフィックス・デスクトップ | 一部未クリア | p004の正確なGUI再現条件、amd64残件、統合試験。 |
| [WS008](ws008/ws.md) | MG006 | Noct・BeUI | 完了 | q063。 |
| [WS009](ws009/ws.md) | MG001 | ドキュメント | 手動保留項目待ち | p001〜p008完了。残るDOC-54はWS014 GPUの保留解除後にPhase化。 |
| [WS010](ws010/ws.md) | MG001 | スクリプト・イメージツール | 完了 | q063。 |
| [WS011](ws011/ws.md) | MG005 | ネットワーク設定コンソール | 完了（ユーザー確認） | commit confirmed完了。VLANキャンセル、bridgeはF-001へ移管。 |
| [WS012](ws012/ws.md) | MG005 | サービス管理コンソール | 完了 | q018。 |
| [WS014](ws014/ws.md) | MG006 | ネイティブGPU | 明示保留 | GPU設計の保留解除時に再開。 |
| [WS016](ws016/ws.md) | MG004 | 実行時swap制御 | 完了 | q021。 |
| [WS017](ws017/ws.md) | MG006 | LFB描画高速化 | 依存待ち | WS022後にmmap・Xzed高速描画・受け入れ。 |
| [WS018](ws018/ws.md) | MG008 | カーネル所有権・構成統一 | 完了 | p001〜p020。I/O後続はWS025。 |
| [WS019](ws019/ws.md) | MG003 | インストール・ディスク管理 | completed | 完了（q186）。amd64共存・専用・グラフィカル版とPC98 FATインストールを受け入れ済み。 2026-09-11ユーザー指示で閉鎖。 |
| [WS020](ws020/ws.md) | MG003 | Intel Mac UEFI・Variant | 完了 | 2026-09-05ユーザー実機確認。 |
| [WS021](ws021/ws.md) | MG001 | LLVM・sysroot | 完了 | q064。 |
| [WS022](ws022/ws.md) | MG002 | ELF TLS | completed | q128：p001〜p003、両x86 QEMU・dynamic回帰、PC98確認。 2026-09-11ユーザー指示で閉鎖。 |
| [WS023](ws023/ws.md) | MG001 | x86 HALコーディング規約 | 完了 | q067。 |
| [WS024](ws024/ws.md) | MG004 | 単一64-bit UFS | 完了 | q102。 |
| [WS025](ws025/ws.md) | MG004 | I/O・キャッシュ・物理メモリ | completed | 2026-09-11ユーザー判断でp029/p030/p032/p038をcleared、WSを閉鎖。p028 canceledを維持。 |
| [WS026](ws026/ws.md) | MG001 | テスト資産の整理 | 計画のみ | 不要・重複テストの整理と現行ソースへの追随。Phase未詳細化・Queue未投入。 |

## Future List(やりたいことリスト)

[Future Work](future-work.md)にF-001〜F-003の由来・再開点を保持。現在の実行対象ではない。

## Known bugs

正本は [既知バグ台帳](known-bugs.md)。再現済み・ユーザー報告・解決済みを区別して記録する。

- **BUG-010：実機USBブート後にhaltするとUSBエラーが出る**。ユーザー報告、未修正。
  [WS002-p023](ws002/phase023/results.md)でq135にQEMU再現・修正済み。実機とEHCI/UHCIの追加確認は残る。
- AX211のfirmware内部assertは残存する。無限診断・復帰不能の修正と実機復帰試験は
  [WS025-p035結果](ws025/phase035/results.md)を参照。

## 主要な依存関係

- WS025の追加残件は2026-09-11ユーザー判断でcleared。既存のI/O契約と旧証拠を保持し、未検証の実装・実機結果を他WSの前提充足に流用しない。
- WS022 → WS017。WS006のevdev移行はデスクトップ各consumerへ反映する。
- WS019のインストーラ受け入れ → WS003の実機NVMeインストール・起動。
  WS013の完了済みブート設定基盤を使い、将来のRuntime CPARを前提にしない。
- WS001の準拠性とWS009の文書は各実装WSから更新する。

## 方針・履歴への索引

- [設計方針・決定の参照資料](master-design-policy.md)：独立実装・ライセンス境界、モジュール設計、ツールチェーン、個別設計判断。
- [整理前master全文](old/master-history-2026-09-09.md)：旧優先順位、milestone、依存図、完了経緯、保留の履歴。現在の実行指示には使わない。
- [Awesome Plan設定](config.md)・[Guardrail](guardrail.md)・[コーディング規約](coding-style.md)。旧MWP-Q/governanceはold/の履歴。
- [q124結果](history/queue-q124.md)、[q123：p031未クリア](history/queue-q123.md)、[q122：WS025統合受け入れ](history/queue-q122.md)。
- [FS関連フォローアップ](old/fs-report-followups.md)。

長期目標は独立実装した共通OS基盤をデスクトップ・モバイル・組込み・サーバーへ提供すること。
設計詳細や過去の結果をこの索引へ重複掲載せず、所有する資料へリンクする。

### Priority execution clarification (2026-09-09)

- 実装可能な作業を先行し、WS006 EHCI/UHCIのQEMU解析は後段へ回すが、現在のゴール内で修正を目指す。
- WS025 p027–p030は実機不在を実装の停止条件にしない。QEMU機能検証と実機性能の未測定を分離する。
- ユーザーはUAS実機を所有していない。ローカルQEMUには`usb-uas`があるため検証経路を確認する。QEMU検証が不可能と確認された場合にUASをFuture Listへ移す。
- WS019 p008の旧形式証跡に代えてp014で現行UFS formatterを再検証・必要改修し、installerの前提とする。

<!-- awesome-plan-operations:start -->

## Awesome Plan運用

[Queue](https://github.com/awemorris/zedBSD/issues/362) · [Guardrail](https://github.com/awemorris/zedBSD/issues/363) · [Future Work](https://github.com/awemorris/zedBSD/issues/364) · [Bug Board](https://github.com/awemorris/zedBSD/issues/365) · [Past Log](https://github.com/awemorris/zedBSD/issues/366)

GitHub modeで運用。q303はfinished/stopped、active Queueはありません。仕様固定版は314a669f57265da3084ffac810871b0e660c9526。次回はリポジトリAGENTS.mdとplan/config.mdから開始します。

<!-- awesome-plan-operations:end -->

GitHub Milestones: [MG001](https://github.com/awemorris/zedBSD/milestone/1) · [MG002](https://github.com/awemorris/zedBSD/milestone/2) · [MG003](https://github.com/awemorris/zedBSD/milestone/3) · [MG004](https://github.com/awemorris/zedBSD/milestone/4) · [MG005](https://github.com/awemorris/zedBSD/milestone/5) · [MG006](https://github.com/awemorris/zedBSD/milestone/6) · [MG007](https://github.com/awemorris/zedBSD/milestone/7) · [MG008](https://github.com/awemorris/zedBSD/milestone/8) · [MG009](https://github.com/awemorris/zedBSD/milestone/9)

## 2026-09-11 実行保留

ユーザーがp022のキュー実行を保留。fg004/fg005/fg006とp022→p023→p024の計画は保持し、active Queueは作成せず、build・実機操作・実装は開始していない。MasterのGitHub同期は今回明示された範囲で行う。
