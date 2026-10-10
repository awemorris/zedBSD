<!-- awesome-plan project=zedbsd record=ws203 -->
# WS203: Raspberry Pi 4 内蔵 Ethernet (BCM2711 GENET v5)

Status: incomplete
Primary Milestone: MG005
Related Milestones: MG003, MG008
Parent: [Master](../master.md)
Owner: Codex / codex/rpi4-genet / `.claude/worktrees/rpi4-genet`
Queue: [scoped Queue](codex-queue.md)

## 目的・受入

ユーザー 2026-10-11「とりあえず、RPi4のEthernetドライバを実装できますか？デバッグが楽になると思います。」を実行する。USBとは独立したGENETとBCM54213PE PHYを既存network stackに登録し、既存networkd/DHCP/SSHから利用できるようにする。実機受入は有線LANのリンク、DHCP、SSH接続。ソース受入はFDT、DMA、PHY、送受信、close/reopen、build optionの実装とwarning0のarm64 kernel buildおよび限定ホスト検証。

## 範囲・制約

初版はRPi4のGENET v5、通常MTU1500、標準ring16、IPv4/IPv6の既存stack。offload、jumbo、WOL、suspend、複数queueは含まない。ハードウェア非搭載・disabled node・初期化失敗でbootを止めない。GENETは既存FDT/MMIO/DMA/IRQ/net_device APIを利用しHAL APIを変更しない。コントローラIRQに加え短い周期のpollでPHYリンクとIRQ取り逃しを回復する。

[Guardrail](../guardrail.md)、[C全文](../coding-style.md)、[automation](../standards/automation.md)、[scoped ownership](../agents/protocol.md)を適用。FreeBSD GENET/brgphyからハードウェアのoffset/bit/動作だけを確認し、ソース・構造・commentを写さずZlibの独自実装。GPL例外の範囲拡張は行わない。共有toolchainはread-only。QEMU/image build/pushは行わない。main統合は具体的成果の承認後。

## Phase一覧

| Phase | 目的・goal | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws203-p001](phase001/phase.md) | FDT/PHY/DMA/ring/net_device driverとbuild/menu配線 | cleared | main a094b953c の既存API |
| [ws203-p002](phase002/phase.md) | 最終ソースのC全文適合、warning0 build、送受信/異常系の限定host model | cleared | p001のソース成果 |
| [ws203-p003](phase003/phase.md) | 実機有線LAN/DHCP/SSH受入 | uncleared | p001,p002とユーザー実機 |
| [ws203-p004](phase004/phase.md) | 実機初期化失敗のGIC/MMIO補完と最終規約/build | cleared | 現mainと実機写真 |

Resume: p003実機初期化失敗、p004の限定source/build修正はcleared、main7b16e364cへ統合済み。新SDイメージからp003 DHCP/SSH再確認へ。F-029のpromotion、Master登録/focus/priority、共有Queue/history/cache/GitHub投影はQ1保留（共有記録を編集しない）。USBのws048-p009実機失敗調査は別件として残る。

## 2026-10-11 分割の指示

ユーザー「src/drivers/ethernet/bcm54213pe.c と、src/drivers/platform/rpi4/rpi4-ethernet.c に分けて実装するのがいいと思います。」を適用。MAC/PHY/DMA/net_deviceの制御はbcm54213pe.c、FDT解析・board handoffはrpi4-ethernet.cへ分割。hardware driverはFDTを読まず、platform configを受け取る。scope/受入/API責務は維持。p002は分割後ソースを検証する。共有Masterは編集しない。

### 分割の訂正（同日、ユーザー「rpi4-ethernet.c に GENETのMACがあるんじゃないかなあ？」）

上の分割説明は訂正。GENET MAC/DMA/ring/net_device/FDT/board起動をrpi4-ethernet.c、外付けBCM54213PE PHYだけをbcm54213pe.cへ置く。PHYはcaller-owned clause-22 MDIO callbackを使い、GENET MMIOに依存しない。旧説明と訂正の履歴を保持する。p002はこの最終構造で検証する。

## 2026-10-11 scoped Queue終了

p001/p002はclear。最終実装はGENET MAC/DMA/ring/net_device/FDTをrpi4-ethernet.c、外付けPHYをbcm54213pe.cに所有させる。両者をMDIO callbackで接続。全CPU共通Ethernet menuでGENETをON/OFF可能、RPi4で既定ON。USB/PCIeの動作に依存せずen0を登録する。既存networkdのup/DHCP経路はsourceで確認。

[結果・手順・規約適合・制約](tests/results.md)。WSの実機受入は未達なのでincompleteを維持。p003はユーザー実機でリンク/DHCP/SSHを確認するhandoff、今Queueはfinished。main merge ACK、共有Master/focus/priority/Queue/history/F-029投影とGitHub publicationはQ1保留。

Submission: `e9ddb4540` (`WIP`), merge request `ws203-genet-20261011`. Integration SHA/ACK pending.

## 2026-10-11 main統合ACK

ユーザー「mainに統合をお願いします。」に従い、実装 `e9ddb4540` と記録 `d1def8aef` をmainへfast-forward統合。source hashes一致、mainの共通Ethernet menu・現在のRPi4 configでGENET=y・arm64への2 driver source登録をread-back確認。main統合の前提は満たした。p003は新イメージと実機DHCP/SSH結果待ち。WSはincompleteのまま。pushなし。共有Master/Queue/history/F-029/GitHubは別途Q1が投影する。

## 2026-10-11 network startup UAT investigation

User reports sshd process startup and apparent link, with DHCP address missing. p003 retains actual lease/SSH/traffic criteria; [source/image inspection](tests/network-startup-20261011.md) confirms configs match amd64 and automatic LAN management exists. Await actual service/network observations; no source changes or acceptance promotion. Shared projection/GitHub pending Q1.

## 2026-10-11 実機初期化失敗の修正開始

[ws203-p004](phase004/phase.md)を追加、GIC trigger/MMIO releaseを修正する。先行のsource/build clearanceと未達の実機受入を区別し保持。[限定Queue](../ws203/codex-repair-20261011.md)。既存HAL APIの補完のみ、共有投影はQ1 pending。

### 修正source/build結果

[ws203-p004](phase004/phase.md)の限定source/build criteria cleared。GIC trigger/MMIO共有alias、非coherent DMAのpage backingを修正し、現在config warning0 kernel buildと限定host checks PASS。[証拠](../ws203/tests/initialization-repair-20261011.md)。実機DHCP/SSH・USB入力は未確認なのでWSはincomplete、次は新imageでuser受入。main統合と共有投影はpending。

## Main integration / 2026-10-11

Current user「main仁藤剛してください。」を直前の7b16e364c統合承認への回答（mainに統合してください）として受領。clean main6b722f47eから修正7b16e364c53761f96a982797f88793f7f520dbb6へfast-forward統合、競合なし。先行read-only調査記録08e919a08も含む。main上で全8 source/test SHA256一致、source diffなし、現在config.mkが検証済みworktreeとbyte-identicalであることをread-back確認。前turnのwarning0 kernel buildと限定host checksが統合sourceに適用されるため追加のbuild/試験は行わない。sourceのmain統合は完了、先行の承認待ち表記は当時の履歴。新imageでの実機DHCP/SSH・USB入力受入は未達のまま、WS203/WS048はincomplete。pushなし。共有Master/Queue/history/FutureWork/GitHubはQ1投影保留。

## SSH physical evidence / 2026-10-11

User reports Ethernet working and supplies 10.0.30.2. Read-only SSH succeeds (g402598d arm64); en0 UP/RUNNING, IPv4 assigned, 1000Mbps full-duplex, RX/TX counters both advance, errors/dropped zero. GENET ready/RX-TX enabled/link-up confirms p004 startup repair on hardware. DHCP lease origin and cable unplug/replug were not separately verified; p003 stays uncleared for remaining full physical criteria, WS incomplete. No remote mutation/restart. Shared projections/GitHub Q1 pending.
