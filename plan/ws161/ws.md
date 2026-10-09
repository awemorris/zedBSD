<!-- awesome-plan project=zedbsd record=ws161 -->
# WS161: YubiKey のサポート

Status: incomplete（2026-10-08 q902 P1 の照合: p002〜p005 cleared。残りは p006 の実機の UAT と p001 の判定。旧: planning（2026-10-05 追加、**ベータ2**（2026-10-05 ユーザー）、見積もり 4 LW → 第 2 版で 6〜7 LW（U5）。p001 の設計の第 2 版（§9）あり、q770））
Master: [master](../master.md)
Primary Milestone: MG006

## 由来

ユーザー（2026-10-05）「YubiKeyサポート（USBのFIDO2が最初。NFCのCTAP2 が目標）」

## 範囲（案、p001 の設計で確定する）

1. USB の HID の FIDO2（CTAPHID）で YubiKey と話す（`usb-hid` の hidraw `/dev/input/hidrawN`、userland の libpasskey）。
2. NFC の CTAP2: ACR1252U（USB の CCID）の共通の `usb-ccid`（`/dev/smartcardN`、APDU の交換）と libpasskey の NFC の transport。
3. 鍵の情報・PIN・登録・認証の道具 `passkey` と試験。詳細は p001 §9（第 2 版）。

## 第 2 版への変更（2026-10-05 夕のユーザーの決定）

`usb-hid` の hidraw（`/dev/input/hidrawX`、`/dev/fidoN` は無し）、YubiKey の CCID と ACR1252U の NFC の reader は共通の `usb-ccid`、独自の
libpasskey（CTAPHID・NFC の APDU・CTAP2・CBOR・PIN/UV、暗号は OpenSSL）、OTP は非対応。段を p002〜p007 に組み直した（実行前、p001 §9.6）。
判断 U1〜U5（p001 §9.9）は 2026-10-05 ユーザーが承認（CCID の node の名前は `/dev/smartcardN`）。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws161-p001](phase001/phase.md) | 要件と設計 | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）） | | — |
| [ws161-p002](phase002/phase.md) | kernel: `usb-hid` の hidraw、`include/uapi/hidraw.h`、seat の一覧、試験の loopback。T1 | cleared（2026-10-05 Q1、T1-197・T1-199。実機の xHCI の interrupt OUT は p006） | p001、U1・U3・U4 |
| [ws161-p003](phase003/phase.md) | kernel: `usb-ccid`、`include/uapi/ccid.h`、seat の一覧。T1 | cleared（2026-10-05 Q1、T1-199。本物の USB CCID は p006） | p001、U2・U3 |
| [ws161-p004](phase004/phase.md) | libpasskey: cbor・transport-hid・ctap2・pin・verify・os 層、道具 `fidoctl`、host 試験 | cleared（2026-10-07、T1-274） | p002（os 層だけ） |
| [ws161-p005](phase005/phase.md) | libpasskey: transport-nfc と `/dev/smartcard*`、host 試験 | cleared（2026-10-08 T1-374 QEMU PASS、実機は p006） | p003・p004 |
| ws161-p006 | 実機の UAT（YubiKey 5 の USB、ACR1252U と YubiKey 5 NFC）、Linux・FreeBSD の build | planned（5330 とユーザーの鍵・reader。Linux・FreeBSD の build は 10/13 以降、2026-10-08 ユーザー） | p005 |
| ws161-p007 | 全文規約の見直し | planned（ベータ3、2026-10-08 ユーザー） | p006 |

## 2026-10-10 UAT（Q1）

「FIDO2のキー登録とログインができました。」（USB の YubiKey）。p006 の USB の分は確認済み。NFC（ACR1252U と YubiKey 5 NFC）と Linux・FreeBSD の build は未確認のまま（10/13 以降）。
