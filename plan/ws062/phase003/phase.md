<!-- awesome-plan project=zedbsd record=ws062p003 -->

# ws062-p003: amd64 の既定と試験の道具を native にする

Phase ID: `ws062-p003`
Parent: [WS062](../ws.md)
Status: cleared
Queue: q456-i01

## 目的

`config/ci/config-amd64.mk` と guest の config の `ZEDBSD_VARIANT` を native に、`plan/tools/guest/`・`boot-test.sh`・`plan/tools/sh/guest-batches.sh` を native の image で動かす。README と design policy 2.3 を更新する。`hybrid` 等は残す。

## 受け入れ

既定の `make disk-image` が native。道具が全て動く。文書。

## 実装（2026-09-26）

- 既定: `Makefile` の amd64 の variant の順を `native hybrid uefi bios` に（先頭が既定）。`tools/menuconfig.py` に `native`（「UEFI, UFS root partition (for PC/AT)」）を先頭に足した。`config/ci/config-amd64.mk`・`plan/ws035/tests/config-amd64-userland.mk`（guest の config が include する）・手元の `config.mk`（git の外）を native に。GPU の WS の試験の config（WS014・WS029・WS031、ws035-p048）は hybrid のまま（各 WS の Phase で決める）。
- 大きさ（2026-09-26 ユーザー「root 1GB, swap 1GBにして、きちんと2GBのディスクイメージにしましょう。そのかわりgzip」）: `AMD64_NATIVE_ROOT_MIB`・`AMD64_NATIVE_SWAP_MIB` を 1024 に。image は 2,216,689,664 byte（ESP 64 MiB + 1 GiB + 1 GiB + GPT）。root と swap の image の名前に大きさを入れ、大きさを変えると作り直すようにした（前は swap が作り直されなかった）。
- CI: `.github/workflows/ci.yml` は image を `gzip -9` した `zedbsd-amd64.img.gz` を artifact と release に（2 GiB の release asset の上限の内、約 84 MB）。
- `make run`（`platform/amd64/run.mk`）: native は OVMF（variables の複写を build の下に）と NVMe で起動。他の variant は今まで通り SeaBIOS と IDE。
- 道具: `guest.py start` の既定の disk を NVMe に。`boot-test.sh` の既定の `BOOT_MODE` を `uefi-nvme` に、image を `IMAGE=` でも受け取るように（引数だけだったため、ws065-p002〜ws067-p002 の boot test が別の image を起動していた。各記録に訂正を追記）。`plan/tools/sh/guest-batches.sh` は native の guest image（`IMAGE=` で変更可）を NVMe で起動、runtime の directory も `GUEST_RUNTIME=` で変更可。`menuconfig-target-host-test.py` の期待を native に。
- 文書: `docs/howto/build-from-source.md`（profile の表、`make run`）、`README.md`、design policy 2.3（build した amd64 の image の既定は native、installer v1 は payload FAT のまま）。

## 検証（2026-09-26）

| 確認 | 結果 |
| --- | --- |
| 既定の `make disk-image`（手元の `config.mk`） | native（GPT: ESP、`zedBSD-root` 1 GiB、`zedBSD-swap` 1 GiB）。gzip -9 で 84 MB、42 秒 |
| boot test（引数なし = 既定の image・`uefi-nvme`、と `uefi-usb`） | 両方 PASS（`build/boot-test-amd64-ws062p003/login.png`。root は `PARTLABEL=zedBSD-root`、swap は partition） |
| guest image（`config-amd64-guest.mk`、`ZEDBSD_VARIANT` の指定なし） | native の 2 GiB。`guest.py start`（既定 NVMe）、root 1 GiB に expat の configure・`make -j4`・runtests 4932/4932（使用 28% → 30%） |
| `plan/tools/sh/guest-batches.sh`（repo の版、NVMe） | 1412/1458、失敗の一覧は ws067-p001 と同じ |
| CI の再現（`ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ci-check`、`make toolchain` を含む） | 最初は 2 点で失敗: `make toolchain` が WS010 の完了で消した `plan/ws010/tests/toolchain-smoke.noct` を要る（GitHub の CI の失敗の原因）、新しい build の directory で clang の package が `$(abspath $(BUILD))/dynamic/libc.so` を要り規則が無い（ws061-p009 の退行。既にある file では見えなかった）。smoke を `tools/build/noct-toolchain-smoke.noct` に移し、clang の依存を `$(BUILD)/dynamic/libc.so` に直して通った。image は boot test PASS（`build/boot-test-amd64-ci-check/login.png`）、gzip 84 MB |
| `menuconfig-target-host-test.py` | variant の検査は通る。その後の package の検査（「expat is filed under packages/libs, which the menu omits」）で止まる。HEAD（変更前）でも同じで、この Phase の外 |

実機は未実施。GitHub の CI は push の後に確かめる（ユーザーの 2026-09-26 の指示「ghコマンドを使えるようにしたので、CIを直してくれる？」）。

