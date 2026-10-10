<!-- awesome-plan project=zedbsd record=ws203-p001 -->
# ws203-p001: GENET実装

Parent: [WS203](../ws.md)
Status: cleared / normal
Queue: rpi4-genet-20261011-i01
Approval: user Ethernet driver instruction (2026-10-11), exact text in Queue.

Design: FDT compatible/reg/SPI/phy-handle/phy-mode/MACを読む。SCBのidentity DMA window内のuncached bufferを既存drv_dmaで確保する。UMAC reset、Broadcom PHY reset/RGMII delays/autoneg、256 RX/TX descriptorのring16を初期化する。net_device en0を登録、通常MTUで送受信、IRQ+定期poll、リンク変化通知、同期close/reopenを実装する。失敗はgenet:ログを残しboot継続。kernel/platform、arm64 make、共通Ethernet menuのみを配線する。

Acceptance: sourceが既存API所有権/orderingに従い、timeoutが有限、DMAがuncachedでidentity窓内、CRC/alignment/producer-consumerを処理、menu ON/OFFで正しくbuildする。実機SSHはp003。
Rules: Guardrail/C全文/automation、HAL変更なし、独立Zlib、toolchain read-only、対象kernelだけbuild、QEMU/pushなし。
Verification: p002のfinal source full-standard review/build/host model。Reference facts: FreeBSD sys/arm64/broadcom/genet/if_genet{,reg.h}, sys/dev/mii/brgphy{.c,reg.h}; firmware bcm2711-rpi-4-b.dtb。一時参照は/tmpまたはtemp（未commit）。

## 2026-10-11 分割の指示

ユーザー「src/drivers/ethernet/bcm54213pe.c と、src/drivers/platform/rpi4/rpi4-ethernet.c に分けて実装するのがいいと思います。」を適用。MAC/PHY/DMA/net_deviceの制御はbcm54213pe.c、FDT解析・board handoffはrpi4-ethernet.cへ分割。hardware driverはFDTを読まず、platform configを受け取る。scope/受入/API責務は維持。p002は分割後ソースを検証する。共有Masterは編集しない。

### 分割の訂正（同日、ユーザー「rpi4-ethernet.c に GENETのMACがあるんじゃないかなあ？」）

上の分割説明は訂正。GENET MAC/DMA/ring/net_device/FDT/board起動をrpi4-ethernet.c、外付けBCM54213PE PHYだけをbcm54213pe.cへ置く。PHYはcaller-owned clause-22 MDIO callbackを使い、GENET MMIOに依存しない。旧説明と訂正の履歴を保持する。p002はこの最終構造で検証する。

## Clearance / 2026-10-11

Source criteria satisfied: final split follows the user's correction; GENET connects the existing DMA/IRQ/net_device APIs and arm64 build/common Ethernet option. p002 verified the final source. [Evidence](../tests/results.md), [hashes](../tests/source.sha256). The scoped i01 clears; p003 retains actual hardware DHCP/SSH acceptance. Main integration pending; no remote Issue closure/publication performed.
