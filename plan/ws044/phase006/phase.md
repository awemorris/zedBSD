<!-- awesome-plan project=zedbsd record=ws044p006 -->

# ws044-p006: 実機で framebuffer に何も出ない（data cache）

Phase ID: `ws044-p006`
Parent: [WS044](../ws.md)
Status: cleared（QEMU の確認まで。実機の確認はユーザー待ち、2026-09-24）
Queue: ユーザーの指示による割り込み（q389 の実行中）。

## 報告（2026-09-24 ユーザー、実機 RPi4）

「起動しませんでした。フレームバッファの特定の部分、1KBくらいかなあ、黒い中にわずかにゴミが表示されて、ディスプレイコントローラの制御もよくないのか、だんだん消えていきました。」

## 原因の見立て

HAL の framebuffer（`src/hal/arm64/bsp-rpi4/framebuffer.c`）は、firmware が割り当てた framebuffer を kernel の直接対応（cacheable な Normal memory）で書いている。
実機では GPU が CPU の cache を通らずに DRAM を読むので、描いた画素が data cache に残ったまま表示されない。cache から追い出された行だけが見え（小さなゴミ）、
黒へのクリアが少しずつ memory に届くにつれて消えていった、と考えられる。QEMU には cache が無いので再現しない。
画面に出ないだけで、起動そのものは進んでいた可能性がある（未確認）。

## 修正（HAL の差分、承認済み）

描いた後に data cache を memory へ書き出す: 文字の枠（16 行 × 8 画素）、cursor（2 行）、初回のクリア（全体）。`hal_dcache_clean_range`（`dc cvac`、PoC まで）を使う。hal.h は不変。
差分 `plan/ws044/proposed/rpi4-framebuffer-cache.diff`。承認: 2026-09-24「承認、すぐ進める」（[Guardrail](../../guardrail.md) の表）。

## 検証

| 検証 | 結果 |
| --- | --- |
| rpi4 の kernel と image の build | 成功（`build/rpi4-font/hdd-image.img`、Raspberry Pi 4 image check PASS） |
| QEMU raspi4b の boot test（`BOOT_MODE=raspi4b plan/tools/boot-test.sh`） | login prompt まで（`build/boot-test-rpi4/login.png`） |
| 実機 | 未実施（ユーザーの確認待ち） |

この image には、実行中の ws043-p012 の libc の変更（printf の浮動小数の正確な十進変換、stdout が端末でないとき完全 buffer）も入っている。amd64 guest と QEMU raspi4b では起動を確認済み。

実機でまだ出ないときの次の手: 起動の段階ごとに画面の一部を色で塗る目印、または UART（GPIO14/15）の log。
