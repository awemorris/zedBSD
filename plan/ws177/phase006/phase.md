<!-- awesome-plan project=zedbsd record=ws177-p006 -->

# ws177-p006: fidoctl の PIN の入力（案 I の 45 行）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-10 Q1 判定: ユーザーの 5330 の UAT「WS177 はOK」、host・build は前の記録のとおり）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q884 の 3（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 45（WS161 ws161-p004）、[案](../phasing-20261008.md) の I（実機の要らない 1 行）

## 設計と変更（2026-10-08 P1）

- `userland/base/fidoctl/main.c` の `fidoctl_read_pin(prompt, pin, size)`: 標準入力が端末なら prompt（`PIN: `・`Current PIN: `・`New PIN: `）を標準 error に出し、`tcgetattr`・`tcsetattr` で ECHO を切って 1 行を読み、元に戻して改行を出す。端末でなければ従来どおり 1 行を読む（script・試験）。
- CTAP2 の規則を鍵に聞く前に確かめる（`fidoctl_pin_valid`）: 63 byte 以下、4 文字（code point）以上。破れば標準 error に規則を言って EINVAL。buffer（72 byte）を越える行は残りを読み捨てて EINVAL（次の行を PIN と取り違えない）。拒んだ PIN は buffer ごと消す（`pk_crypto_wipe`）。
- 試験: `plan/ws177/tests/host-fidoctl-pin.{c,sh}`（main.c を main の名を変えて取り込み、pipe の行を読ませる）。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-fidoctl-pin.sh` → PASS（ASan/UBSan）: 4 文字を取る、3 文字を拒む、3 byte の 4 文字を取る、63 byte を取る、64 byte を拒む、buffer を越える行を拒み次の行を丸ごと読む、行が無いのを拒む。
- `OUT=build/p1-i sh plan/ws161/tests/fidoctl-host-test.sh` → PASS（回帰）。
- build: `make BUILD=build/p1-d ZEDBSD_CONFIG=plan/ws161/tests/config-amd64-fidoctl.mk build/p1-d/bin/fidoctl`（fidoctl は warning 0。log の warning は package の OpenSSL の source の物）。style-check 0。

## 未実施

- 端末で打った PIN が出ないこと（実機か QEMU の serial の console、UAT）。

## Event

2026-10-08 / q884-i03（P1）: 実装と host・build。
