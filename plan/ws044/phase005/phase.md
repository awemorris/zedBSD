<!-- awesome-plan project=zedbsd record=ws044p005 -->

# ws044-p005: rpi4 の実機起動の準備

Phase ID: `ws044-p005`
Parent: [WS044](../ws.md)
Status: cleared（q378-i01、2026-09-24）
Queue: q378（q378-i01）

## 目的

2026-09-24 ユーザー依頼「実機RPi4で起動する上で問題になりそうなことがあれば教えてください。なければイメージを作っていただいて、
私の方で実機で起動確認してみます。」QEMU（raspi4b は GPU firmware を動かさず、kernel を ELF で直接読む）と実機の違いを洗い、
直せるものを直して実機用の image を作る。

## 調べたことと対応

| 項目 | 実機での問題 | 対応 |
| --- | --- | --- |
| kernel の読み込み番地 | vmunix は物理 0x80000 で動くよう link されている（arm64 Image の header の text_offset も 0x80000）。現行の firmware（2026-08）は 64-bit の kernel を既定で 0x200000 に置く。そこで走ると、MMU を入れた後の高位の番地が 0x80000 の物理を指し、止まる | `platform/arm64/config.txt` に `kernel_address=0x80000` |
| SD の register の幅 | BCM2711 の EMMC2（iProc SDHCI）は 32-bit の access だけを受ける（Linux は 8/16-bit を 32-bit にまとめ、TRANSFER_MODE と COMMAND、BLOCK_SIZE と BLOCK_COUNT を一語で書く）。driver は 8/16-bit で書いていた（QEMU の SDHCI は幅を問わない） | `src/drivers/platform/rpi4/rpi4-sdhci.c` の `r8`・`r16`・`w8`・`w16` を 32-bit の access にした。対の前半を保持して後半と一語で書く |
| 例外 level | firmware は EL2 で渡す | 既に EL2→EL1 の切替えがある（問題なし） |
| 他の CPU | 実機は spin-table | boot CPU 以外は停めたまま（単一 CPU で動く） |
| mailbox の cache | 実機は cache が効く | clean・invalidate 済み（問題なし） |
| SD の DMA | — | PIO なので DMA の cache 問題は無い |
| UART | PL011 の clock 48 MHz、`dtoverlay=disable-bt` で GPIO14/15 | 既定どおり（問題なし） |
| firmware | 2026-08-10 の start4.elf | Pi 4B の全 revision に足りる |

直していない危険（実機の結果で判断する）:

- `SCTLR_EL1` を既知の値に書かず、読んだ値に bit を足している（`src/hal/arm64/locore.S`）。firmware の armstub は通常 reset 値のまま渡すので
  問題にならない見込み。firmware の後に何も出ずに止まる場合の候補。直すには HAL の差分の承認が要る。
- SD の base clock が capabilities で 0 のとき 100 MHz と仮定している。firmware の EMMC2 の clock と違えば初期化が timeout しうる（`sdhci: init error`）。
- 入力はシリアルだけ（Pi 4 の USB は PCIe の先の VL805 で driver が無い）。HDMI は出力だけ。
- DTB は Pi 4 Model B 用だけ（Pi 400・CM4 は別）。

## 検証

| 検証 | 結果 |
| --- | --- |
| rpi4 の kernel と disk image の build | 成功、warning 0 |
| `BOOT_MODE=raspi4b plan/tools/boot-test.sh` | PASS（画面の login prompt） |
| QEMU raspi4b でシリアルから login、`uname`・`id`・`df`・`dyntest`（`plan/ws036/tests/boot-rpi4.sh`） | PASS |
| SD への書き込みと読み戻し（1 MiB の file と小さな file、`sync` の後に読む） | PASS |
| 実機 | 未実施（ユーザーが確認する。image は `build/rpi4-font/hdd-image.img`） |

一度目のシリアルの試験は login の直後に shell の prompt を見られず失敗した（同じ image で再実行すると通った。harness の待ちの競合と見ている）。
