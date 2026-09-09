# Queue q129: WS019 installer prerequisite design

Date: 2026-09-09
Status: finished
Authorization: Priority全件のPhase設計・Queue化・自走をユーザー明示承認済み。
Timebox: p013 60 active minutes、p014 120 active minutes。
Previous: [q128](queue-q128.md) finished / WS022 completed。

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws019-p014](ws019-installation/phase014-current-ufs-formatter/phase.md) | completed | ユーザー追加指示：現行UFS formatter整合、旧fixture修正、target受け入れ。 |
| 2 | [ws019-p013](ws019-installation/phase013-installer-prerequisite-contracts/phase.md) | completed | 起動元provenance、atomic publication、Noct native primitive、stagingの不足を現ソースで再評価し、前提Phaseを設計。 |

p004/p005は前提不足を解消してから実行Queueへ入れる。既存ESP/FAT/GPT/NVRAMを変更しない契約と、対象を明示して確認するinstaller UIを保持。今回の調査では実媒体へ書き込まない。

追加指示によりp013の前提設計へ[p014 current UFS formatter](ws019-installation/phase014-current-ufs-formatter/phase.md)を含める。現行producer比較の読取主体の診断を先行し、p014を本Queueへ追加し、p013設計の続きに先立って実行する。包括的なQueue作成・自走承認と今回の追加指示に基づく。

Results: p014 current formatter host + target + reboot PASS; p013 prerequisite contracts completed. Combined runtime found existing ATA flush BUG001; isolated format/overlay passed. Next queue selects atomic publication before source identity and Noct staging.
