<!-- awesome-plan project=zedbsd record=ws034p052 -->

# ws034-p052: root の UFS image を BUILD ごとに置く

Phase ID: `ws034-p052`
Parent: [WS034](../ws.md)
Status: **cleared**（q360-i01、2026-09-24）
Phase disposition: normal
Queue: q360（q360-i01）
実行: メインセッション

## 不具合（ws041-p002 で発見）

root の UFS image（p036 以後は `$(BUILD)/rootfs` の tree そのもの）が、全 BUILD 共有の `build/arch-images/` に置かれていた:
`amd64.ufs`、pcat と pc98 が同じ名前で共有する `i386.ufs`、`aarch64.ufs`。作り直すかどうかは自分の BUILD の tree の stamp との時刻比較だけで決まるので、
**別の config の BUILD が後から書いた image を、tree の変わっていない BUILD がそのまま自分の disk image に入れていた**。

実際に起きたこと: SSH ハーネスの BUILD（openssh あり）の後に測定用の BUILD（openssh なし）を作り、ハーネスの kernel だけを作り直すと、
ハーネスの image の root が測定用のものになり、`/etc/service.d/sshd` が無く sshd が起動しなかった（kernel の不具合と見誤りかけた）。
以前からあった `ZEDBSD_TEST_IMAGE_TAG`（E-127、試験の root に別名の image を持たせる）は、この共有を試験用だけ避ける回避策だった。

## 変更

- `Makefile`: `ZEDBSD_ROOTFS_IMAGE_DIR := $(BUILD)/arch-images`。
- `platform/amd64/vmunix.mk`・`pcat`・`pc98`・`arm64`: root の UFS image（`amd64.ufs`、`i386.ufs`、`aarch64.ufs`、`ZEDBSD_TEST_IMAGE_TAG` の別名も）を
  そこへ置く。決まった file の一覧から作る試験の image（`amd64-deferred-test.ufs` 等）は共有のまま（BUILD の tree から作るものではない）。
- 共有の場所に残った古い `amd64.ufs`・`i386.ufs` は、共有の build 成果物を消さない規則に従い残した（もう誰も読まない）。

## 検証

| 検証 | 結果 |
| --- | --- |
| 測定用の BUILD を作った後にハーネスの BUILD を作り直す（不具合の手順） | 各 BUILD に自分の `arch-images/amd64.ufs`（538 MB と 71 MB）。ハーネスは SSH で答え、`/etc/service.d` に sshd がある |
| pcat・pc98 の CI の disk image | それぞれの BUILD に `i386.ufs` |
| pcat の BIOS・IDE 起動 | login prompt（PASS） |
