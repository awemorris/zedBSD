<!-- awesome-plan-current:start -->

## 現在状態 — 2026-09-12

Active Queue: none
fg006: completed

2026-09-12、ユーザーがWS003 p022・p023・p024の完了を報告し、MarkdownとGitHubの更新を指示した。3 Phaseをcleared（disposition: normal）として受け入れ、fg006を完了とする。WS003はインストーラ等の残件があるためincompleteを維持する。新しいQueueは作成せず、保留中の実行は再開しない。

対象: [p022](https://github.com/awemorris/zedBSD/issues/88)、[p023](https://github.com/awemorris/zedBSD/issues/89)、[p024](https://github.com/awemorris/zedBSD/issues/90)。

根拠は今回のユーザー完了報告。既存Markdownの自動検証・旧試行の結果は履歴として保持する。今回エージェントがbuild・QEMU・実機検証を再実施したものではなく、新しいartifact hashやroot/init/login到達点は報告されていないため追加しない。旧試行のunclearedや未実施項目を過去に遡ってPASSへ変更しない。

<!-- awesome-plan-current:end -->

# Past Log

## 2026-09-11 fg006: PC-9821V13での起動改善

ユーザーがCurrent Focused Goalsへの追加と、WS003 p022/p023/p024の具体的実行Phase化を明示指示した。fg004（4機種インストーラ）・fg005（ネットワーク）を保持する。

| 順序 | 実行Phase | 成果 |
| --- | --- | --- |
| 1 | [ws003-p022](../ws003/phase022/phase.md) | 現行IPLのstack・BIOS read契約、artifact対応、診断の前提 |
| 2 | [ws003-p023](../ws003/phase023/phase.md) | LBA0実行後の実機停止境界と原因を絞る観測 |
| 3 | [ws003-p024](../ws003/phase024/phase.md) | 根拠に対応した修正と通常imageでのV13起動確認 |

依存: p022 → p023 → p024。p022/p023は旧履歴専用の扱いを解除し、現行の役割・受け入れを上記と各Phaseに更新する。過去の試行・証拠は保持する。3 Phaseはunclearedのまま次の試行を待ち、今回in-progressやclearedにしない。
この順序は選択した3 Phaseの依存順であり、削除済みの全WS Priorityリストを復活させない。今回の依頼は計画更新。新しいactive Queue・実行時間枠は未設定。

新規source変更・build・実機操作・GitHub公開は未実施。

## 2026-09-11 ネットワーク改善1〜3（計画のみ）

ユーザー指定をfg005 / [ws005](../ws005/ws.md) p013〜p017に整理。net lan disableは無効化。network-enableは有線かWi-FiのどちらかのIP取得で待機終了、既定30秒・設定可能、timeoutでも通常起動とdaemon接続処理を継続することをユーザーが確認。状態通知のsocket/fileは設計選択として保持。現行net wifi enableはバックグラウンド接続開始であることを静的確認した。

既存完了Phase、インストーラfg004、旧Priority削除を維持。新規実装・build・ネットワーク変更・公開なし。GitHub更新は未承認のまま保留。

## 2026-09-11 インストーラ実機bring-up計画（実行なし）

ユーザーがPC98 / Latitude 5320 / SV7 / LX6でのインストールを指定。[ws003](../ws003/ws.md)のfg004として、既存Phaseを再利用しp026〜p032を計画、BUG-013を既存IDで詳細化しBUG-023〜025を追加。PC98はV13/64MB/CF-IDE、/sbin空はQEMU上とユーザーが確認した。

WS019/WS025の閉鎖、旧Priority削除、q303停止を維持。新しいQueue・実装・build・実機操作なし。媒体/方式と実行範囲の具体化が次の段階。

## 2026-09-11 計画判断（新規Queueなし）

2026-09-11のユーザー指示により現在のPriorityリストを削除。WS025のp029/p030/p032/p038をcleared、WS025をcompletedとし、既存completedのWS019/WS006/WS022/WS002とともに閉鎖する。未実施の検証をPASSへ変更せず、今回の計画上の受け入れとして記録する。q303はfinished/stoppedのまま。active Queueと新しいPriority/Focusはない。

決定者: current user。指示原文:

> 計画を更新します。現在のPriorityリストを削除します。WS025の残件はclearedにして、WS025を閉じます。WS019も閉じます。WS006, WS022, WS002を閉じます。

WS025の確認未実施・実機未確認事項は各Phaseの履歴に保持。既存のBug台帳、WS009/WS014保留、Milestoneの判定は変更しない。

## 最新Queueの履歴

最新: [q303](queue-q303.md) — finished / ws025-p038 uncleared。ユーザー停止。
3ビルドは当時PASS、amd64実行試験は中断、PC/AT未実行。現行修正後の合格ではない。
次のQueueは未承認。

[全Queue索引](queues.md)。旧completedは当時の表記を保持し、現在のclearedへ履歴を改書しない。

## 2026-09-12 fg009: PPC Open Firmware / APM+FAT

PowerBook G4 A1010 / 867MHzを移植先とし、まずQEMU mac99上で、Open Firmware → APM+FATの独自ローダ → zedboot.cfg → 同じFATのvmunix → PPCカーネル初期化を成立させる。後続でamd64上のUSB OHCI、PPCユーザーABI、USB root、rootfs.img/data.imgのループバック利用へ進む。今回は計画のみ。

最初の到達点はp033→p034→p035。rootfs.img/data.imgは後続p038。設定名は今回指定のzedboot.cfg（現行UEFIはzedbsd.cfg）、kernel=vmunix。独自ローダはXCOFFを第一候補とし、OFによるELF直接ロードに依存しない。

- [ws003-p033](https://github.com/awemorris/zedBSD/issues/367): OF起動契約・APM/FAT imageとXCOFFローダ入口 (planned)
- [ws003-p034](https://github.com/awemorris/zedBSD/issues/368): zedboot.cfg・FAT読み取り・PPC ELF handoff (planned)
- [ws003-p035](https://github.com/awemorris/zedBSD/issues/369): PPC HAL・mac99基板対応とカーネル初期起動 (planned)
- [ws003-p036](https://github.com/awemorris/zedBSD/issues/370): amd64でUSB OHCI・USBストレージを検証 (planning)
- [ws003-p037](https://github.com/awemorris/zedBSD/issues/371): PPCユーザーABI・libcとinit到達 (planning)
- [ws003-p038](https://github.com/awemorris/zedBSD/issues/372): PPC USB boot・rootfs.img/data.img統合 (planning)
- [ws003-p039](https://github.com/awemorris/zedBSD/issues/373): PPC/OHCI変更の最終規約・統合確認 (planning)

全体の共通契約は各Phase本文に記載。実行Queueは作成せず、既存の実行保留、fg006完了、他WSの判断を保持する。

## 2026-09-12 WS003終了・WS027新設

ユーザーがPPCを新規移植として独立WSへ移すよう指示し、WS003を閉じて再利用しないことを指定した。その他の未完了は「未完了のまま保留事項へ移し、WS003内のPhaseは終了する」と明示。目標達成や試験PASSを追加する判断ではない。

PPC移植は[ws027](https://github.com/awemorris/zedBSD/issues/374)のp001-p007（旧WS003 p033-p039）へ移管。初期到達点はp003まで。その他の未完了は[Future Work F-004](https://github.com/awemorris/zedBSD/issues/364)へ保留移管。fg009はWS027、fg004は保留。WS003は終了・再利用禁止。実行Queueは作らない。

## 2026-09-12 インストーラ実機動作を独立WS化

ユーザー指示により[WS028](https://github.com/awemorris/zedBSD/issues/382)を新設。単一目標はPC98 V13、Latitude 5320、SV7、LX6でインストーラを実行し、インストール先から起動・loginできること。fg004をこのWSで再選択する。PPC移植は[WS027](https://github.com/awemorris/zedBSD/issues/374)に分離済み。WS003は閉鎖済み・再利用禁止。

NVMeが実機で動作していないというユーザー報告と、menuconfigにNVMe項目がないだけかもしれないという仮説をWS028へ記録。現行pci.driversには項目があり、amd64/i386の組込み・登録経路もある。実機使用config/imageとの一致と失敗境界は未確認。

Future Work F-004のうち旧WS003 p018/p019とp026-p032はWS028への引継ぎ対象とする。その他は保留のまま。元Phaseは閉じたまま保持し、新しい実行Phase・Queueはまだ作らない。

## 2026-09-12 GPU計画更新

ユーザー指示により、[WS014](https://github.com/awemorris/zedBSD/issues/15)の初期bring-up対象をi915からQEMU virtio-gpuへ変更する。未完了・未着手の既存目標の具体化であり、終了WSの再利用ではない。[p001](https://github.com/awemorris/zedBSD/issues/213)の設計検討の手動保留を解除しplanningとする。実装Queueは作成・再開しない。WS009など他WSの保留は自動解除しない。

Vulkanのディスプレイ拡張をOSの公式なユーザー向け表示APIとする案を検討する。正式採用やABI凍結は未決定。Linux DRM互換を必須としない従来方針は維持するが、メモリ管理、同期、画面出力、所有権・権限を担うOS/ドライバ機構は必要。Vulkan APIをそのままカーネルABIへコピーしない。

## 2026-09-12 Vulkan API関数別の責務表

ユーザー依頼により、Vulkan 1.0〜1.4の全コア234関数と選択した表示関連拡張41関数、計275関数について、libvulkan.so側（loader/ICD/WSIを含むユーザー空間実装）とzedBSD GPUドライバ側の責務を一関数一行の表にした。

[WS014 p001の関数別責務表](https://github.com/awemorris/zedBSD/issues/213#vulkan-api-responsibility-table)に全文を掲載する。固定したKhronosレジストリとの集合照合で欠落・重複・空欄なし。libvulkan.so単体の構成とloader/ICD分離の違い、Venus転送、記録/submit/表示の違い、対象外拡張、対応宣言ではないことを明記した。

設計資料でありAPI/ABI採用確定やPhaseクリアランスではない。WS014/p001はplanning、Queueは未開始。Markdownはローカル作業ツリーにも保存し、git commit/pushはしていない。

## 2026-09-12 責務分類をU/Kに統一

ユーザー指示により、[Vulkan API責務表](https://github.com/awemorris/zedBSD/issues/213#vulkan-api-responsibility-table)の275関数を、U（ユーザー空間実装）とK（GPUドライバ）の二つの責務欄だけで整理した。旧Q/C/R分類と境界列を削除。キャッシュ・記録・転送は責務欄の説明として保持する。ドライバへの照会はK、結果の整形等はUであり、キャッシュ可能性を別分類にしない。関数集合とplanning状態、Queue未開始は維持。
