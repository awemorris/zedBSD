<!-- awesome-plan project=zedbsd record=ws062p003 -->

# ws062-p003: amd64 の既定と試験の道具を native にする

Phase ID: `ws062-p003`
Parent: [WS062](../ws.md)
Status: in-progress
Queue: q456-i01

## 目的

`config/ci/config-amd64.mk` と guest の config の `ZEDBSD_VARIANT` を native に、`plan/tools/guest/`・`boot-test.sh`・`plan/tools/sh/guest-batches.sh` を native の image で動かす。README と design policy 2.3 を更新する。`hybrid` 等は残す。

## 受け入れ

既定の `make disk-image` が native。道具が全て動く。文書。
