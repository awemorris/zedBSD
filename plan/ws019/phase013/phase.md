# WS019-p013: installer prerequisite contracts

Date: 2026-09-09
Status: completed (q129 design)
Parent: [WS019](../ws.md)

## Objective

p004を現在のkernel/loader/Noct/FATへ接続する前提を設計し、独立して実装・検証できるPhaseへ分割する。p004/p005の既存媒体を破壊しない契約を維持し、欠けたprimitiveをcheck-then-actや成功扱いで迂回しない。

## Procedure

1. boot handoff、kernel retained origin、configured boot0、rootfs backing、公開mount/geometry APIを照合する。firmware実行元とkernel/config元の意味を分離し、証明できないidentityを推測しない。
2. VFS/FAT rename serializationとdirectory durabilityを確認し、atomic no-replace publication APIを定義する。
3. 利用可能なNoctから既存コマンドを呼ぶ構成とし、各操作を実在するコマンドへ対応付ける。不足は既存コマンド拡張または未実装のPOSIX/UNIX標準的コマンドで補う。installer専用helperの新設はしない。必須機能がなお不足する場合、具体的に記録して該当Phaseをunclearedとする。
4. FAT stagingのbounded allocationとformatter reservationを現WS025/WS024実装へ合わせ、必要な実測を選ぶ。
5. 各prerequisiteを独立Phaseにし、先に実行できる項目をQueue化。p004/p005の着手条件を更新する。

## Acceptance

現ソースの根拠、採用するAPI・所有権・失敗時処理、Phase dependencies、ホスト/QEMU受け入れの具体像を文書化する。成果物は設計とfixture planであり、installer実装・実機受け入れ済みとは扱わない。

Timebox: 60 active minutes。残る人間判断があれば当該作業だけunclearedにし、独立項目を続ける。

Results: [contract](contract.md) records actual source findings and the user-selected command boundary. p014 current formatter is completed; p015 atomic publication, p016 provenance and p017 command staging are independently planned. Their detailed implementation contracts are finalized before their own finite queues.
