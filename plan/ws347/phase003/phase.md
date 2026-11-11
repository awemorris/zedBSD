<!-- awesome-plan project=zedbsd record=ws347-p003 -->

# ws347-p003 driver の追従の実装と host 試験

- 親: [WS347](../ws.md)。状態: planned。依存: p002。

## 範囲

- p002 の設計に従い `src/drivers/gpu/ghci/` を直す。Blitter が無い（CAP_BLT=0）時の転送・clear・blit は shader の道
  （WS290 p010 の vk-meta.c の compute・描画の代替）に回す。
- HAL は変えない。

## 受け入れ

- WS290 の host 試験と新しい CAPS の場面（bit ごとに 0・1）が PASS。riscv64（VF2、CONFIG_DRIVER_PCI_GHCI=y）と amd64 の kernel が warning 0。
