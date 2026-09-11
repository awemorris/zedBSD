# インストーラ実機bring-up計画 — 2026-09-11

担当: [ws003](../ws003/ws.md) / MG003 / fg004。今回の依頼は計画。実装・build・実機操作は未実施。

<!-- installer-bringup-current:start -->

## Current focus — 2026-09-11 installer bring-up

Status: incomplete。親: [master](../master.md)。Primary Milestone: MG003。Focused Goal: fg004。
現在のPriorityリストは削除済みのまま。今回の機種一覧は順位ではない。Queue: none。今回のユーザー指示は計画更新であり、開発・実機操作は開始しない。

### ゴールと担当

| 機種 | 到達点 | 担当Phase |
| --- | --- | --- |
| PC-9821V13 / 64MB / CF-IDE | 実機でインストールが行える | [ws003-p024](../ws003/phase024/phase.md)、[ws003-p026](../ws003/phase026/phase.md)、必要な[ws003-p027](../ws003/phase027/phase.md) → [ws003-p029](../ws003/phase029/phase.md) |
| Dell Latitude 5320 | 実機でインストールが行える | [ws003-p018](../ws003/phase018/phase.md)（既存FAT経路）または[ws003-p019](../ws003/phase019/phase.md)（native経路）。実行する方式を具体化して選択 |
| Let's Note SV7 | 実機でインストールが行える | [ws003-p030](../ws003/phase030/phase.md) |
| Let's Note LX6 | 実機でインストールが行える | [ws003-p028](../ws003/phase028/phase.md) → [ws003-p031](../ws003/phase031/phase.md) |

共通の受け入れ案は通常インストールの完了と、インストール先からloader→kernel→root→init/login・基本操作を確認すること。text/graphic、FAT/native、具体的な書込み先は機種ごとの実行計画で確定する。両モード全組合せや新しい反復実機試験を自動的な必須条件にしない。既存の具体的な媒体・データ保持の制約は維持する。
この4機種の成果はfg004を担う。WS003の既存の別ゴール・未完了事項を今回の範囲に無断で加えたり、4機種だけでWS全体がcompletedになったと判断したりしない。

### 今回の観測と課題

- [BUG-013](../bugs/BUG-013.md): PC98のLBA0がロード・実行されるがビープ停止。以前の到達推定を今回のユーザー観測で更新。PC98構成は従来と同じことをユーザーが確認。
- [BUG-023](../bugs/BUG-023.md): `/sbin` が空なのはQEMUで起動したPC98環境。実機IPL停止とは別経路。
- [BUG-024](../bugs/BUG-024.md): PC98でPCI/USBをmenuconfigから選べない。platform除外とnormalize経路を静的確認。実動作対応は別途確認。
- [BUG-025](../bugs/BUG-025.md): LX6 USB起動でbootパーティションを判別できずinitを起動できない。正確なログ・起動モードは未取得。

閉鎖済みWS019/WS025およびWS025-p032の受け入れは維持。インストーラ実装は[ws019](../ws019/ws.md)を利用し、PC98ブート問題の現担当をWS003-p024/BUG-013へ統一する。BUG-017とLX6問題の同一原因は未証明。

### 依存と実行準備

p024 → p029、p026 → p029、p028 → p031。p027 → p029はPCI/USBを実際に使う場合の成果依存であり、IDE単独の経路を不要に止めない。SV7およびLatitudeの準備はPC98/LX6修復から独立。
Latitude p018は既存の読取り・loader・installer成果を現行artifactで確認する。p019はnative方式を選んだ場合だけ具体化する。閉鎖されたWS019への古い「実装待ち」は現在の前提にしない。
[ws003-p032](../ws003/phase032/phase.md)は今回変更した最終sourceに対する規約確認を担う。変更が出揃う終盤に機種別結果と合わせる。前提未充足や実機入力待ちは対応Phaseで明示し、独立作業を待たせない。
新しい実行Queueは未選択。調査順の候補はQEMU `/sbin`・menuconfigの独立切り分け、PC98 LBA0後の境界確定、LX6のboot媒体解決の切り分け。これは承認済み実行順ではない。

<!-- installer-bringup-current:end -->

## 登録・公開状態

ローカル計画は作成済み。新規7 Phaseと4 Bug TicketのGitHub公開は自動レビューで拒否され、明示承認待ち。既存8 Issueの更新・関連コメント・親子/Project反映も送信待ちとしてjournalに保存する。現行Priorityリストは削除したまま。

公開先: https://github.com/awemorris/zedBSD （既存の公開リポジトリ）、Project: https://github.com/users/awemorris/projects/2 。新規作成はws003-p026〜p032とBUG-013/023/024/025。BUG-013は既存台帳項目のTicket化であり別バグではない。

### 確認済みのユーザー回答

- /sbinが空: QEMUで起動したPC98環境。
- PC98実機: PC-9821V13 / 64MB / CF-IDEで従来と同じ。

### 次の計画入力

Latitude/SV7/LX6の実際の起動モード、各機種のインストール先と保持データ、使用するインストーラのモード、LX6の実際の停止ログは未確定。これは新しい未承認機能の追加ではなく、機種別の実行範囲を固定するための入力。

### BUG-013の旧索引（履歴）

| `BUG-013` | Current PC98 physical boot | Open; user report 2026-09-09; WS025-p032 reopened | Current binary still does not work on physical hardware. Earlier q124 QEMU acceptance is retained but does not establish physical success. Exact machine, image identity and last boot milestone remain to be correlated. | Retain current image/config hashes, check maintained QEMU regression, identify the physical stopping boundary, fix the demonstrated difference and confirm the same ordinary binary on hardware (p032 R10). Independent phases may continue while physical observations are pending. |

## 後続のCurrent Focus更新

fg006としてPC-9821V13の起動改善を独立指定。p022→p023→p024が契約確認・停止位置特定・修正/実機確認を分担する。fg004とインストール受け入れは保持する。[現行実行Phase](../ws003/v13-boot-focus.md)。
