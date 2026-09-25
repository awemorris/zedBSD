<!-- awesome-plan project=zedbsd record=ws044p007 -->

# ws044-p007: 実機の表示を 1920x1080 に固定する

Phase ID: `ws044-p007`
Parent: [WS044](../ws.md)
Status: cleared（QEMU の確認まで。実機の確認はユーザー待ち、2026-09-24）
Queue: ユーザーの指示による割り込み（WS043 の完了の作業中）。

## 報告（2026-09-24 ユーザー、ws044-p006 の image で）

「実機ではまだLCD表示がおかしいです。1920x1080固定で出力できますか？…640x480くらいのエリアにゴミが見え、だんだん画面全体が消えていくので、LCDコントローラがおかしいです。scanoutの周波数かなあ。」

## 調べたこと

- kernel の直接対応は RAM 全体を覆い（`space.c`）、memory は FDT の memory node（firmware が GPU の領域を除く）から取る。SD の driver は PIO で DMA を使わない。
  kernel が VideoCore の memory を壊す経路は見つからなかった。
- HAL は firmware に 640x480 の framebuffer を要求し、firmware がそれを拡大して出していた。HDMI の mode は config.txt で固定しておらず、EDID 任せだった。

## 変更

- `platform/arm64/config.txt`（HAL ではない）: `hdmi_group=2`・`hdmi_mode=82`（1920x1080 60 Hz、DMT）、`disable_overscan=1`、`framebuffer_width=1920`・`framebuffer_height=1080`。
- HAL `src/hal/arm64/bsp-rpi4/framebuffer.c`（承認済み）: firmware が設定した大きさを `TAG_GET_PHYSICAL` で問い合わせてそのまま要求する（等倍で scanout。返らない QEMU は 640x480）。
  cache を書き出した後に `dsb sy`（GPU は CPU の共有領域の外にある）。差分 `plan/ws044/proposed/rpi4-framebuffer-size.diff`、承認 2026-09-24「承認、すぐ進める」。
- 1920x1080 では console の 80x25（640x400 画素）は画面の中央に小さく出る。

## 検証

| 検証 | 結果 |
| --- | --- |
| rpi4 の image の build | 成功（`build/rpi4-font/hdd-image.img`） |
| QEMU raspi4b の boot test | login prompt |
| 実機 | 未実施（ユーザーの確認待ち） |

まだ直らないときの次の手: UART（GPIO14/15、115200）の log があれば起動のどこで止まるかが分かる。無ければ、起動の段階ごとに画面の隅を色で塗る目印を入れる。
