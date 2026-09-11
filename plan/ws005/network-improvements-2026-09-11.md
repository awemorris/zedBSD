# ネットワーク改善1〜3 — 2026-09-11

担当: [ws005](ws.md)。Primary Milestone: MG005。Related: MG006（DE状態反映）。Focused Goal: fg005。
今回のユーザー指示は計画。実装・build・ネットワーク設定変更・サービス起動は未実施。旧Priorityリストは削除したまま、順序付け・実行Queueは未指定。

## 確定した要件

### 改善1: net lan と有線LANの常駐管理

- `net lan enable` は有線LAN管理を有効化、`net lan disable` は無効化。disableの説明はユーザーが「無効にする」と訂正確認。
- `networkd` がバックグラウンドでインタフェースup、IP設定、`dhcpc`等を管理する。
- enable時にケーブル未接続でも管理状態を保持し、後でケーブルが挿されたらIP設定またはDHCPを行う。
- 設計案: CLIは管理意図の受理を返し、IP取得完了まで常にブロックしない。対象は物理有線LAN、loopbackとWLANを区別する。全有線かnet.conf選択対象か、複数NIC、手動設定との関係はp013で固定する。
- disable後の遅延イベントで再有効化しない。稼働中DHCPを重複起動せず、切断・再接続・デバイス再列挙を管理状態に反映する。停止時に撤去するIP/route/DNSは管理所有範囲を明確にする。

### 改善2: 起動時のoneshot network-enable

- `net lan enable` と `net wifi enable` の両方を実行する。
- **有線またはWi-Fiのどちらか**が有効になりIP取得できた時点で待機を終了する（ユーザー回答）。両方のIP取得は要求しない。
- 最大待機時間は設定可能、既定 **30秒**。設計案:サービスの有効化要求と状態待機で一つのmonotonic deadlineを共有し、各コマンドの待機を直列加算して30秒を超えない。
- **タイムアウトしても通常起動を続行し、networkdの接続処理は継続する**（ユーザー回答）。待機終了をdisableやdaemon停止へ変換しない。timeoutとIP取得成功は記録上区別する。
- 既に使えるIPがある場合、片方の機器がない場合、ケーブル未接続、Wi-Fiプロファイルなしでも両方式の要求を適切に試み、取得済みの他方を妨げない。
- 設計案のready判定: loopbackを除く対象のadmin-up・有効な接続状態・使用可能なIP。現在のIPv4基盤を基準にし、インターネット疎通やDNS応答を追加の必須条件にしない。static IPも対象に含める。

### net wifi enable の現行仕様確認

現行 `userland/base/net/main.c` のhelpは「enable policy; association runs in background」と記載。`networkd/main.c` の `wifi_request_enable` はpeerのeffective UIDに対応するプロファイルを読み、管理ポリシーを有効化して `schedule_automatic_work(0U)` を呼ぶ。RF associationをenable要求の中で完了させない。
既存仕様ではautoプロファイルから自動選択し、WLANのL2認証後にDHCPを行う。enableの成功とIP取得成功は同義ではない。起動サービスがrootで呼ぶ場合はrootのsystem store（`/etc/wifi.conf`）が対象となり、ログインユーザーの個人プロファイルを自動的に使うとはしない。
これらは現行source/既存文書の確認であり、新しい実行試験はしていない。再enable、プロファイルなし、無線機器なし、disableとの競合、接続済み、UID切替えと自動再試行をp013で状態表にする。

### 改善3: networkdの状態通知とDE受信

- networkdがネットワーク状態変化を外部から受け取れるunixソケットまたはファイルを提供する。
- インタフェース接続・切断等を通知し、DEが受信してネットワーク状態の表示を変える。
- **設計案**: read-only用途のAF_UNIXストリーム通知を第一候補とする。接続時の全体snapshotと、その後の変更通知を同じ状態モデルから配信する。パス・形式・権限はp013/p016で確定し、既存の制御用 `/run/networkd.sock` と操作権限を混同しない。
- 候補レコード: version、daemon世代、sequence、インタフェース名/indexとdevice世代、媒体種別、管理有効/無効、carrier/L2、IP準備状態、変更理由。秘密鍵・passphrase・credentialは含めない。
- DEの途中起動、再接続、networkd再起動、受信欠落をsnapshotで復元できる設計にする。遅い/停止した受信側でnetworkdやDHCPを止めず、無制限の履歴を保持しない。
- DE側は初期状態、接続、切断、IP取得/喪失、再接続の表示反映を確認する。実際のDE受信箇所・表示箇所は実装前に特定する。UIの具体的な意匠は未指定。

## 現行実装との接続点

`networkd` は既存AF_ROUTE/RTM_IFINFOを読み、managed-WLANの状態機械に渡している。有線の継続管理とDE向け購読APIは、この既存経路・net.conf設定・DHCP子プロセス管理と整合させる。既存のnet commit/confirmed rollback、設定所有権を壊さない。
既存サービスは `networkd`（daemon、fd3 readiness）→`net`（oneshot、`net boot`）→`ntpdate`。`notify-timeout` はnon-oneshotのfd3待機であり、新サービスの30秒待機の代用ではない。
設計案:小さなサービス用補助処理に `--timeout=秒` を持たせ、サービス定義のargumentsで設定する。最終CLI/設定名はp013で確定。`networkd` のreadyと既存 `net boot` による設定読込みの後に有効化し、DHCPを二重管理しない。ntpdate等の依存は実際の起動契約を確認して必要な範囲だけ更新する。

## Phaseと依存

| Phase | 担当 | 状態 | 前提 |
| --- | --- | --- | --- |
| [ws005-p013](phase013/phase.md) | enable/disable、Wi-Fi現行仕様、ready・通知・設定の共通契約 | planning | 現行sourceと今回のユーザー回答 |
| [ws005-p014](phase014/phase.md) | net lanとnetworkdの有線管理、共通状態snapshot/query | planning | p013 |
| [ws005-p015](phase015/phase.md) | network-enable起動サービスと全体30秒deadline | planning | p013/p014の要求・ready状態契約 |
| [ws005-p016](phase016/phase.md) | 状態通知とDE受信・表示反映 | planning | p013/p014の共通状態モデル |
| [ws005-p017](phase017/phase.md) | 統合受け入れ・最終変更の全文規約確認 | planning | p014/p015/p016 |

p015とp016は共通状態を共有するが、起動待機をDEや未起動の購読者へ依存させない。p013の契約確定後、独立した実装範囲を有限Queueに選ぶ。Wi-Fi enableを仕様確認なしに同期IP待ちへ変更しない。

## 残る設計判断

有線NICの対象選択とnet.confの優先関係、有線/Wi-Fi同時接続時のroute/DNS所有権、通知のsocket/file選択・レコード・閲覧権限、DE受信箇所、timeoutの設定名/有効範囲・観測方法を具体化する。確定済みの「どちらかでIP」「既定30秒」「timeoutでも起動継続・daemon継続」は再質問しない。

## 検証と運用境界

[guardrail](../guardrail.md)、`plan/coding-style.md`全文と`plan/standards/automation.md`に従う。変更範囲のfocused check、対応設定の `make -j16`、必要なQEMU通常系を選ぶ。aggregate `make check`、commit、push、未承認HAL変更は行わない。既存の完了Phaseの受け入れを維持し、規約確認は今回の変更に対して終盤に行う。
今回の3件は機能追加計画であり、未実装を新しい故障Ticketとして登録しない。前のインストーラ計画とともにGitHub公開待ちを保持し、今回の計画継続を公開承認と読み替えない。
