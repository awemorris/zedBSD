<!-- awesome-plan project=zedbsd record=ws161-p005 -->
# ws161-p005: libpasskey の NFC の transport と zedBSD の `/dev/smartcard*`

Status: cleared（2026-10-08 Q1、T1-374 QEMU PASS（ws.md の表）。実機は p006）（旧: test-wait（2026-10-08 P1: 実装と host 試験まで、T1 の QEMU（loopback の card）待ち。実機は p006））
Disposition: normal
Parent: [WS161](../ws.md)、設計 [phase001](../phase001/phase.md) §9.4・§9.5・M5・M9
Queue: P1 の列（Q1、ユーザーの優先順の 7 番、2026-10-08。Q1 の ACK「範囲 1〜4 で進めてよい」）

## 範囲

1. libpasskey の `transport-nfc.c`: SELECT（AID A0000006472F0001）、NFCCTAP_MSG（CLA 0x80・INS 0x10・P1 0x80）、短い APDU の reader での command chaining（CLA 0x90）、61xx の GET RESPONSE、9100 の keepalive の NFCCTAP_GETRESPONSE（INS 0x11）、時間の上限。
2. zedBSD の `/dev/smartcard*` の列挙と開閉（os 層）、fidoctl が NFC の鍵も選ぶ。Linux・FreeBSD は NFC 無し（設計どおり）。
3. host 試験（ソフトウェアの authenticator を APDU の card の後ろに）。kernel・UAPI は変えない。
4. QEMU（T1）は p003 の loopback の card で SELECT まで。実機は p006。

## 実装（2026-10-08、P1）

- `userland/base/libpasskey/nfc.h`・`transport-nfc.c`（新）: `pk_nfc_open`（SELECT、答えの "FIDO_2_0"・"U2F_V2" を `versions` に。CTAP2 かは GetInfo で決める＝M5）、`pk_nfc_transact`（1 つの期限で全体、拡張の APDU の reader で入る時は 1 つの拡張の APDU、それ以外は 255 byte ごとの chaining（途中は 9000 を確かめる、Le 無し）、答えは 9000 まで 61xx ごとに GET RESPONSE（Le は SW2）、9100 は keepalive の状態を伝えて NFCCTAP_GETRESPONSE、room を越える答えは EMSGSIZE、他の status は EIO）、`pk_nfc_transport`（`pk_transport` に、cancel は無し）。送った APDU と message は使った後に消す（PIN の暗号文を含みうる）。
- `os.h`・`os-zedbsd.c`: `pk_os_list_cards`（`/dev/smartcard*` で card のある slot、`CCID_GET_INFO`・`GET_STATUS`）、`pk_os_card_open`（`CCID_POWER_ON` で card の電源と slot の claim、`CCID_TRANSMIT` の io、`max_command`・拡張の APDU は `CCID_GET_INFO` から）、`pk_os_card_close`（`CCID_POWER_OFF` と close）。`os-linux.c` は card 無し（`cards 0`、open は ENOTSUP）。
- `fidoctl`: `list` に `card PATH VID:PID NAME` の行と `cards N`（`devices N` の行は不変）。`-d /dev/smartcardN` で NFC、`-d` 無しは USB の鍵が無ければ最初の card。開いた後の GetInfo と PIN の protocol の選びは HID と共通（`fidoctl_ready`）。Makefile に `transport-nfc.c`。passkey-fido2（login の鍵）は範囲の外で変えない。
- 試験: `plan/ws161/tests/libpasskey-ctap2-host-test.c` に NFC の card（SELECT、MSG の度に 9100（UP needed）、GETRESPONSE で答え、短い reader では 256 byte ごとに 61xx）を足し、HID と同じ流れ（GetInfo・setPIN・誤った PIN・token・MakeCredential・沈黙の問い・GetAssertion の検証・changePIN）を NFC の短い APDU（protocol 2、CTAP 2.1）と拡張の APDU（protocol 1、CTAP 2.0）で。600 byte の echo（vendor の command 0x40）で chaining 2 回と GET RESPONSE 2 回（短い）・0 回（拡張）、room を越える答えの EMSGSIZE。`fidoctl-host-test.sh` は `cards 0` を確かめる。QEMU の試験 `plan/ws161/tests/fidoctl-p005.sh`（新）。

## 確認

- host: `sh plan/ws161/tests/libpasskey-host-test.sh`（ASan・UBSan）: cbor・ctap2（HID 2 本＋NFC 2 本）・descriptor・verify PASS。`fidoctl-host-test.sh`（Linux の fidoctl）PASS。`plan/ws172/tests/fido2-host-test.sh`（os-linux.c を使う）PASS。
- build: `make -j16 build/amd64/bin/fidoctl build/amd64/bin/passkey-fido2` exit 0、我々の source の warning 0（OpenSSL の package の warning は除く）。style-check 0（変えた libpasskey・fidoctl の file）。
- 未実施: QEMU（T1）`fidoctl-p005.sh`（image は `config-amd64-fidoctl.mk`、loopback の card: list に card、`-d /dev/smartcard0 info` が SELECT を通って GetInfo で止まる、後で smartcard-probe が PASS）。実機（p006: ACR1252U と YubiKey 5 NFC）。
