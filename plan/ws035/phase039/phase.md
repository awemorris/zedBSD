<!-- awesome-plan project=zedbsd record=ws035p039 -->

# ws035-p039: USB CDC-ECM の実機確認

Phase ID: `ws035-p039`
Parent: [WS035](../ws.md)
Status: **cleared**（q343-i01、2026-09-24）
Phase disposition: normal
Queue: q343（q343-i01）
実行: メインセッション

## なぜ要るか

`src/drivers/usb/usb-cdc-ecm.c` は**実績が無いまま入っている**（2026-09-23 ユーザー指摘）。
ゲストのネットワーク（`ue0`）が上がるかどうかを確かめていないので、
それに依存する SSH ハーネス（p038）は成立しない。

## 速度と controller の食い違い（2026-09-23 ユーザー指摘）

**ECM は USB 2.0 の device である。それを xHCI で試した記録が無い。**

- QEMU の `usb-net`（`hw/usb/dev-network.c`）は USB 2.0 の device で、full/high speed である。
- q323 でハーネスに使ったのは `qemu-xhci`。**xHCI は USB 3.0 の controller** で、
  2.0 の device は root hub の 2.0 ポート側に付く。
  経路（transfer ring、endpoint context、TT の扱い）が EHCI とは別物である。
- ws004-p019（`Status: complete (q049)`、「ECM を control path とする」）の本文は
  「xHCI and USB binding からの共通経路を証明する」と書いており、
  `plan/ws004/tests/qemu-usb-cdc-ecm.noct` も `qemu-xhci` に `usb-storage`（port 1）と
  `usb-net`（port 2）を並べている。**q323 で collapse したのと同じ並びである。**
- したがって「xHCI で試していない」と「xHCI で試したことになっている」が食い違う。
  **この Phase の最初の仕事は、どちらが本当かを確かめること**である。
  q049 の実行結果（`plan/ws004/` の記録）を読み、実際に流れたのか、
  topology の `device_add` だけで packet は流れていないのかを見る。

## 範囲

シリアルコンソール（p037、cleared）で観察しながら確かめる。**キー入力は使わない。**

1. **記録の照合**: ws004-p019 / q049 が ECM について実際に何を通したのか。
   xHCI で packet が流れた証拠があるのか、topology の確認だけなのか。
2. **EHCI で確かめる**: `-device usb-ehci` に `usb-net` を付け、
   device が attach され `ue0` が現れ、DHCP で address を得るか（`net show`）。
   ECM が USB 2.0 である以上、ここが素直な組合せである。
3. **xHCI で確かめる**: 同じことを `qemu-xhci` で行い、EHCI との差を見る。
   差があるなら、それは xHCI 経路の未対応であって ECM driver の問題とは限らない。
4. **USB storage との同居**: q323 で、boot disk と同じ xHCI controller に置いた直後に
   `usb-storage: BOT CBW error=13` が続き root が応答しなくなる様子を観測した。
   **原因は切り分けていない**（emulator か、指定か、driver か、上の速度の件か）。
   **実機では未確認。** controller・port・速度の組合せを変えて切り分ける。

   **訂正（2026-09-23、WS040）**: この観測の原因は USB storage でも usb-net との同居でもなかった。
   `KERN_CLOCK_HZ` が 1000 になったのに USB の timeout が 1 tick = 10 ms で換算されていて、
   **5 s のつもりの timeout が約 500 ms** になっていた。TCG の遅いゲストで負荷がかかると要求が
   500 ms を超え、`ETIMEDOUT` → overlay の read-only 隔離に至った。
   KVM で disk を 1 I/O/秒に絞る実験で再現を確かめた。詳細は [WS040](../../ws040/ws.md)。

## 受け入れ

- 1の照合結果を記録する（記録と実態が食い違っていたなら、ws004-p019 の記述を直す）。
- 2と3で、どの組合せで `ue0` が上がるか、上がらないならどの段で止まるかを証拠付きで記録する。
- 4の再現条件と切り分け結果を記録する。原因が driver なら直す。
- 直した場合、`plan/tools/guest/guest.py` の controller の並びをその結果に合わせる
  （今は「動いた並び」を採っているだけで、根拠は無いと注記してある）。

## 結果（q343-i01、2026-09-24）

### 1. 記録の照合

ws004-p019 / q049 は、xHCI（boot disk と同じ controller を含む）で **static・DHCP の address、ping、detach・reconnect が通った**と
記録している（topology の確認だけではない）。ただし実行時の記録（pcap 等）は repository に残っていない
（履歴は "Refactor start" から始まる）。今の tree で試し直した結果が下表で、**boot disk と同じ xHCI では
今は起動しない**（下の 4）。記録を否定する材料ではなく、その後に入った退行か、q049 のときは競合に当たらなかったかは
区別できない。ws004-p019 に注記を足した。

### 前提の訂正: `usb-net` は full speed

**QEMU の `usb-net` は full speed（USB 1.1）の device である**（high speed ではない）。EHCI（high speed だけ）に直接は付かず、
QEMU は `speed mismatch` で起動しない。EHCI で試すには UHCI の companion が要り、その場合 device は companion の UHCI に付く。
Phase の本文の「USB 2.0 の device」は full speed の意味で正しいが、「EHCI が素直な組合せ」は誤りだった。

### 2・3. どの組合せで `ue0` が上がるか（QEMU 10.0.11、KVM、`-netdev user`、amd64 UEFI USB 起動）

| controller | `ue0` | DHCP | ping 10.0.2.2 | 止まる段 |
| --- | --- | --- | --- | --- |
| UHCI（`ich9-usb-uhci1`） | 出る | 通る | **0/3** | 受信: **64 byte の倍数の frame を取りこぼす**（下） |
| EHCI＋UHCI companion | 出る（companion の UHCI） | 通る | **0/3** | 同上 |
| xHCI（別の controller） | 出る | 通る | 3/3 | TCP は遅い（下の TCP） |
| xHCI（boot disk と同じ controller） | 修正前は**起動しない**、修正後は出る | 通る | 5/5、40/40（storage 同時） | —（下の 4 で直した） |

**UHCI の不具合（未修正、ws035-p044 へ）**: pcap（QEMU `filter-dump`）では、slirp は DHCP・ARP・ICMP のすべてに答えている。
ゲストは ARP を 21 回送り続け、RX の計数は 6 → 8 しか増えない。ARP の応答は 64 byte で、full speed の最大 packet（64）の
倍数なので、QEMU は長さ 0 の packet で転送を終える。UHCI の driver は SPD（Short Packet Detect）を立てていないので、
短い packet（長さ 0 を含む）の後も同じ URB の残りの TD へ次の frame を受け、**frame の境目が失われる**
（`uhci_request_actual` は長さ 0 の TD を読み飛ばして次の TD を足す）。DHCP の応答（590 byte）は 64 の倍数でないので通る。

**TCP（未修正、ws034-p046 へ）**: xHCI で 1 MiB の fetch が 246 秒で 600 KB しか進まない。pcap では、ゲストの SYN に
MSS option が無く（slirp は 1440 byte の segment を送る）、ゲストは受信 window を「空き枠 × 1024 byte」で広告するので、
7 segment で window が 1024 に落ちる。1440 > 1024 なので slirp は送らず、5.12 秒後の zero-window probe まで止まる。
ゲストはアプリが読んで枠が空いても window update を送らない（window が 0 でなかったため）。ECM の問題ではない。

### 4. boot disk と同じ xHCI（原因を特定して直した）

**症状**: `usb-net`（または `usb-kbd`）を boot disk と同じ xHCI に付けると、約 3 回に 1 回、device の configure の直後から
`usb-storage: BOT CBW error=42` が続き、root が応答しなくなる。high speed の `usb-storage` をもう 1 台付けても起きない
（full speed の device は configure の command が多く、競合の窓に当たりやすい）。WS040 の timeout 換算の修正後も KVM で再現した。

**切り分け**: QEMU の trace（`usb_xhci_*`）では storage の転送は全部 success で event も積まれていた。ゲストの側では
unmatched event も送信の拒否も無く、**event の割り込みが来なくなっていた**。trace の IMAN への書き込みを追うと、
ある時点から command の開始時に読んだ IMAN の IE が 0 で、以後 IE が戻らない。

**原因**: `xhci_irq()` は IP を消すのに `IMAN = IMAN | IP` と**読んで書き戻していた**。`command_ex()` は command の間
IE を 0 にして event ring を自分で poll し、終わりに IE を戻す。割り込み handler が別の CPU で「command 中（IE=0）に読み、
command の復帰の後に書く」と、**IE=0 を書き戻してしまい**、以後その interrupter は割り込みを出さない。
event ring は次の command が poll するまで処理されず、storage の要求は timeout する。
加えて、`command_ex()` の IE の切替えが IP（write-1-to-clear）に 1 を書いており、command 中に来た割り込みを捨てていた
（EHB が立ったままになると、以後の event でも割り込みが来ない）。

**修正**（`src/drivers/pci/pci-xhci.c`）: driver が望む IE を `interrupter_enabled`（初期化で 1、quiesce で 0）と
`command_polling`（command の poll 中）で持ち、IMAN への書き込みはすべて `event_lock` の下で
`xhci_interrupter_write_locked()` がその 2 つから作る（IMAN を読み戻さない）。IP を消すのは割り込み handler と
quiesce だけで、command の前後では IP に触らない（IE を戻すと保留の割り込みが出る）。

**検証**: 修正前は同じ構成で 3 回中 1 回、6 回中 2 回が起動しなかった。修正後は `usb-net` 10 回・`usb-kbd` 4 回の計 14 回が
すべて起動し、`BOT CBW error` は 0。共有の controller で DHCP、ping 5/5、storage を 433 file 読みながらの ping 40/40。
amd64・pcat の CI kernel は warning 0。

`plan/tools/guest/guest.py` の device の並びを、network と keyboard が boot disk と同じ controller を共有する形に戻し、
「理由の分からない並び」という注記を原因の説明に置き換えた。

### 回帰試験について

`plan/ws004/tests/` の xHCI・zero-packet・ECM の host fixture（`run-xhci-concurrent-urbs-test.sh` 等）は、**変更前の source でも
同じように失敗する**（`pc98-auto.c` の section が見つからない、`kern_malloc` が未定義）。refactor 以降に古くなったもので、
今回は直していない（VM の host 試験と同じ扱い。夜間の報告に記録）。

### 副次の観察

- ゲストの `dd if=/dev/zero of=/tmp/z bs=65536 count=64` が 32768 byte しか書かなかった（`dd` か tmpfs。調べていない）。
- ゲストの `grep` は `-i` を、`head` は `-N` 形式を受けない。
