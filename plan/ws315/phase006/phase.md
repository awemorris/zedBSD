<!-- awesome-plan project=zedbsd record=ws315-p006 -->
# ws315-p006: TCP の受けの buffer の自動調整（F-WS315-rcvbuf）

Status: in-progress（2026-10-30 09:16 P3 q1696-i01 ラップアップ（利用の上限、Q1 の指示）: 実装・host・build・QEMU は済み、5330 の同時の send の退行が未解決。下の「引き継ぎ」から再開。08:35 着手）
Parent: [WS315](../ws.md)
Queue: q1696 / q1696-i01（P3）
依存: p003（5330 の同時の送受信の測り）、WS221 p010・p012（10GbE の recv、SACK）

## 由来

- ユーザー（2026-10-30、原文）:「F-WS315-rcvbufを解決してほしいです。WS315のphaseにして、今取り組んでください。」
- F-WS315-rcvbuf: zedBSD の TCP の受けの buffer は `TCP_RECEIVE_BUFFER` 128 KB 固定。5330 の RTL8156B（vendor mode）で送受信を同時に流すと
  recv 1442.9 Mbit/s（Linux r8152 は 2340.9）。片方向は Linux と同等（p003 の k6）。差は受けの window と往復（128 KB / 1443 Mbit/s ≈ 0.73 ms）。

## 今の形（調べた事）

- 窓（`tcp_receive_window`）は、buffer の byte の残りと、queue が取れる packet の残り × `SOCKET_STREAM_PACKET_BYTES`（1024）の小さい方。
  64 bit の機械は `receive_packet_limit` 128・`receive_hiwat_bytes` 128 KB。SO_RCVBUF は byte の上限だけを変え packet の上限は変えないので、
  SO_RCVBUF を 1 MB にしても窓は 128 KB を超えない。
- packet は stack 全体の 1 つの pool（64 bit: 標準 2 KB × 1024 個 ≈ 2.3 MB、32 bit: 32 個）から取る。1460 byte の segment が 1 個ずつ占める。
  窓 W は最大 W / 1024 個を約束する。1 MB の窓は 1024 個（今の pool の全部）。WS311 p003 の tcp-model で「1 MB の窓で A の copy と B の queue が
  1024 個を使い切って接続が死んだ」、WS221 p010 で「768 KB の窓（pool 4096）で NIC の取りこぼしから崩れた（SACK が無かった）」。今は SACK がある（p012）。
- window scale: この端の shift は 5（`TCP_RECEIVE_WSCALE`、65535 << 5 ≈ 2 MB）。
- 時計: kernel の tick は 1 ms（amd64）〜 10 ms。LAN の往復（0.2〜1 ms）は tick では測れない。`kern_rtc_read_counter`（amd64 は TSC）は細かい。

## 設計

考え方の出典（code は写さない）: M. Fisk, W. Feng, "Dynamic Right-Sizing in TCP"（LACSI 2001。受け手が「1 つの窓の分を受けるのにかかった時間」を
往復の上限として測り、1 往復に受けた量の 2 倍を buffer にする）。Linux の tcp_rmem の autotuning と FreeBSD の receive buffer の auto-sizing は
同じ考え方（受けた・読んだ量が buffer に迫れば広げる、上限と system の memory の上限がある）。

1. **測り**（接続ごと、network worker の入力の順の data の後、socket の lock の下）:
   - 測りの始めに `mark = receive_next + 今の窓`、`base = receive_next`、`start = 今`（µs、`kern_rtc_read_counter`）。
   - receive_next が mark を越えたら: `elapsed = 今 − start`（1 つの窓を受けた時間 ≥ 往復）、`bytes = receive_next − base`。
     受けの往復の見積り `rtt`: 小さい標本はそのまま取り、大きい標本へは 1/8 ずつ寄る（窓で抑えられた時の elapsed が往復に近い）。
   - 1 往復に受けた量 `per_rtt = bytes × rtt / elapsed`。読み手が追いついている（queue の byte が buffer の半分未満）時、`2 × per_rtt` が今の buffer を超えたら広げる。
   - 時計が無い・elapsed が 0 の時は測りを延ばす（mark だけ進め、start・base は保つ）。
2. **広げ方**: 新しい buffer = min(上限, 2 × per_rtt) を 1024 byte の packet の数に切り上げ、packet の上限と byte の上限を一緒に（packet × 1024）。
   縮めない（約束した窓を引っ込めない、RFC 9293 3.8.6.2.2）。
3. **上限**:
   - 接続ごと: `SOCKET_BUFFER_MAX`（1 MB）。window scale 5 のままで足りる（2 MB まで表せる）ので shift は変えない。
   - system 全体: 既定の 128 個を超えて自動で広げた packet の合計は pool の実際の数の 1/4 まで（全体の予算、接続を閉じると返す）。予算が尽きたら広げない。
   - pool: 64 bit の既定を 1024 → 4096 個（約 9.3 MB、boot の時に 1 度。memory が足りなければ今の形で半分ずつに減り、予算も実際の数の 1/4 になる）。
     自動の予算 1024 個 ＋ 送りの copy（接続ごとに ring 256）＋ device の受けの分を、1 MB の窓の接続が 1 本あっても pool に残す。
   - 小さな板（32 bit、pool 32 個）: 自動にしない（今の 8〜44 個の queue のまま）。条件は `PACKET_BUF_POOL_COUNT >= 1024`。
4. **SO_RCVBUF**: 明示した socket は自動にしない（その後も）。値が 128 KB を超えれば packet の上限もその値まで予算から広げる（今は窓が 128 KB を超えなかった）。
   listener の SO_RCVBUF は accept した接続に引き継ぐ（BSD・Linux と同じ）。
5. **見える所**: net.tcpstats の `receive_limit_bytes`（今の receive_hiwat_bytes）が広がりを示す。UAPI は変えない。

## 確かめ

- host: tcp-model の全 scenario PASS。場面を足す: 往復の長い（10 ms）経路で B の buffer が自動で広がり速さが 128 KB の窓の上限を超える事、
  SO_RCVBUF を明示した B は広がらない事。shim に `kern_rtc_read_counter`（model の時計）。
- build: 全 platform の vmunix の warning 0。
- QEMU: `qemu-tcp.sh "" 15 inet,inet6`。
- 5330: `5330-rtl.sh`（send・recv・同時）と Linux の基準（`5330-linux-both.sh`）。
- 10GbE（10.0.10.3 の RTL8127 ↔ centris、ATR 有効）の recv の非退行と改善。
- 受け入れ: 5330 の同時の recv が 2000 Mbit/s 以上（Linux 2341 に近づく）、片方向・10GbE が落ちない。

## 結果

### host・build・QEMU（8b9523b20、広げる倍率 2 倍）

- tcp-model（`plan/ws220/tests/tcp-model/run.sh`）PASS。場面を 4 つ足した: receive-auto-10ms（12.13 → 32.13 MB/s、B の buffer 1 MB）、receive-auto-lan（285 KB）、
  receive-fixed-10ms（SO_RCVBUF 128 KB は広がらない、12.13）、receive-buffer-1m（SO_RCVBUF 1 MB が 1 MB の窓に、32.13）。前の tree と比べて変わったのは
  往復 10 ms の場面（rtt-10ms・nagle-1024・nodelay-1024・rtt-estimate-10ms・syn-retried）の速さだけ（上がった）。shim に `kern_rtc_read_counter`、model に `--expect-receive-limit` と「receive:」の行。
- vmunix の build（`build/tmp/p006-buildall.sh a`、21 config）: amd64・pcat・pc98・intelmac・rpi4・rpi5・zybo・visionfive2・riscv64-virt・odroid-c5・armv7-virt・
  riscv32-virt・ppc32-mac99・ppc64-pseries・m68k-virt・mips32-malta・mips64-malta・sun4u・x68k・amd64-rtl8127・5330-rtl すべて rc 0・warning 0。
- QEMU `plan/ws221/tests/qemu-tcp.sh "" 15 inet,inet6` PASS（guest send/recv inet 42.7/40.9、inet6 46.2/41.1 Mbit/s、net.tcpstats の packets_total=4096）。

### 5330（RTL8156B vendor mode、相手 centris、30 s、`MODE=--nvme-kernel 5330-rtl.sh`）

| run | 倍率 | send | recv | 同時 send | 同時 recv | 受けの buffer（同時の時） |
| --- | --- | --- | --- | --- | --- | --- |
| p003 k6（前、128 KB 固定） | — | 2372.0 | 2338.3 | 2346.5 | 1442.9 | 128 KB |
| **p006-k1**（8b9523b20） | 2 倍 | 2364.3 | 2372.2 | **1030.3** | **2334.3** | 973824（recv だけの時 674816） |
| **p006-g10**（未 commit の試し、`TCP_RECEIVE_AUTO_GROWTH_EIGHTHS 10`） | 1.25 倍 | 2367.7 | 2372.3 | **1644.7** | **2356.2** | 373760（recv だけの時 297984） |
| Linux r8152（p003 both1） | — | 2353.4 | 2353.6 | 2345.7 | 2340.9 | — |

- 同時の recv は Linux の 99.7 %（k1）・100.7 %（g10）に達した（受け入れの目安 2000 以上を満たす）。片方向の send・recv は落ちていない。ping・MTU・down/up の後の ping は両 run とも 3/3、5330-run PASS、Debian に戻った。
- **未解決: 同時の send が落ちた**（2346.5 → k1 1030.3・g10 1644.7）。見立て: 受けの窓が広がると centris の data が switch の 5330 向きの queue（2.5G の入りは data でほぼ満杯）に溜まり、
  その後ろで 5330 の送りへの ACK が遅れる（送りの接続の srtt k1 1987 µs・g10 1133 µs）。5330 の送りは ring 256 segment（約 374 KB）が in flight の上限なので、往復が延びると送りが律される
  （374 KB / 2 ms ≈ 1.5 Gbit/s）。Linux は送りの buffer が数 MB なので延びを吸収する。倍率を下げると queue（窓 − BDP）が減り send が戻る方向（1.25 倍で 1645）。
- 証拠: build/ws315-hw/{p006-k1,p006-g10}/summary.txt・extra.txt、kernel は build/ws315-p006-{k1,g10,g12}-vmunix。

## 引き継ぎ（2026-10-30 q1702-i01 P3、次のセッションが自動で再開する点）

- 最後の commit: この節を含む commit（Q1 に送った SHA）。受けの自動は 8b9523b20（倍率 2 倍のまま）、**案 A（Q1 が採った）は 4575db11d**: 64 bit の CONFIG_TCP_SEND_QUEUE_MAX 256→768（include/kern/net/tcp-socket.h）、
  TCP_SEND_BUFFER 384→1152 KB（tcp.c）、packet-buf.h の説明、tcp-model の nodelay-1024 の期待 16000→15000（大きな ring で device の満杯の間に後ろの write が詰まる、15570）と説明。32 bit（ring 8）は変えない。未 commit の source は無い。
- Q1 の条件（q1702）: (1) ue0 の TX dropped の増えの層を確かめて記録（送りの段の queue の溢れなら抑える。理由が分かれば数値はそのままで可）、(2) 案 A で tcp-model・21 config・QEMU・5330・10GbE（片方向と同時が落ちない）、(3) 32 bit は変えない。
- TX dropped の層（確かめた）: 数えているのは driver の TX の queue（RTL_TX_QUEUE_MAX 128）の満杯で ENOBUFS を返した手渡し（net_device_transmit の ifi_oqdrops、usb-rtl815x.c の rtl の transmit）。TCP はその segment を ring に unsent のまま持ち、transmit の完了の後に渡し直す（wire の損失・再送ではない）。
  同時の送りの接続の net.tcpstats: d1 send_refused 113962・retransmit_segments 181（k1 7550・218、g10 12796・144）。再送は増えていない。増えたのは in flight が大きく、ACK の到着で出す塊が queue 128 を越える回数。費用は refused ごとの packet の取り・header の組み・解放だけ。数値はそのままにする（Q1 の条件の「理由が分かれば可」）。
- 見立て（q1702）: 同時の時、両方向は同じ往復を共有する（各向きの ACK は他の向きの data の queue の後ろ）。受けの窓 W が送りの in flight S（ring 256 × 1448 ≈ 370 KB、sndbuf 384 KB）より大きいと、
  送りは R × S / W に落ちる（k1 W 973 KB → 1030、g10 W 374 KB → 1645）。受けの側だけでは W を S のすぐ下に置くしかなく脆い。
- 測り（30 s、`MODE=--nvme-kernel sh plan/ws315/tests/5330-rtl.sh <kernel> <name> 30`、証拠 build/ws315-hw/<name>/summary.txt）:

| run | 形 | send | recv | 同時 send | 同時 recv | 同時の受けの buffer |
| --- | --- | --- | --- | --- | --- | --- |
| p003 k6 | 128 KB 固定 | 2372.0 | 2338.3 | 2346.5 | 1442.9 | 128 KB |
| p006-k1 | 2 倍 | 2364.3 | 2372.2 | 1030.3 | 2334.3 | 973824 |
| p006-g10 | 1.25 倍 | 2367.7 | 2372.3 | 1644.7 | 2356.2 | 373760 |
| p006-d1 | 診断: 2 倍 ＋ ring 768・sndbuf 1152 KB（一時の変更、commit しない） | 2370.9 | 2368.5 | **2176.1** | **2350.3** | 864256（recv だけ 686080） |
| p006-a1 | 案 A（4575db11d、kernel の sha256 は d1 と同じ d10d30aeb406c145: 説明だけの差） | 2367.2 | 2372.4 | 1616.1 | 2286.3 | 923648（recv だけ 653312）。この host（相手の perf-peer）が load 19 の間に測った |
| p006-a2 | 案 A（a1 と同じ kernel） | 2349.9 | 2366.7 | 1538.5 | 2337.2 | 886784。host の load 20〜40（他の担当の build） |
| Linux p006-lin1 | r8152（`5330-linux-both.sh`、同じ load） | — | — | 2253.7 | 2310.8 | — |
| p006-b1 | p006 の前（c90327de3 の 3 file、128 KB 固定・ring 256、build/ws315-p006-base-vmunix） | 2371.6 | 2334.7 | 2285.5 | 1546.7 | 131072。load 40 → 14 |
| p006-ag10 | 案 A ＋ 倍率 1.25 倍（tcp.c の 16U → 10U を一時に、build/ws315-p006-ag10-vmunix） | 測り中 | | | | |

- 同時の送りの接続（net.tcpstats）: d1 retransmit 181・fast 14、a1 917・53、a2 483・57（in flight 約 650 KB、cwnd 約 820 KB、srtt 約 2.1 ms）。案 A では ring でなく損失（CUBIC の cwnd）が律している。損失の場所は未確認（host の ixgbe の ATR の入れ替え、host の load の影響の見込み）。
  見立て（b1 の後）: 案 A でも受けの窓が 900 KB まで広がると、switch の 5330 向きの queue（窓 − BDP）で送りの往復が 2 ms に延び、損失の後の CUBIC の戻りが遅い。同時の受けは窓 200〜374 KB で足りる（g10 の 2356）。
  そこで「案 A ＋ 1.25 倍」（ag10）を測る。**未 commit の一時の変更: src/kern/net/tcp.c の TCP_RECEIVE_AUTO_GROWTH_EIGHTHS 16U → 10U**（再開したら `git diff` で確かめ、決めるまで commit しない）。
  host の load を揃えて比べるため、p006 の前の kernel（c90327de3 の 3 file を一時に置いて build、戻した。build/ws315-p006-base-vmunix）を同じ時間帯に測る。

- d1 の他: ping・MTU・down/up 3/3、5330-run PASS、Debian に戻った（uname で確かめた）。pool の使用の最大 1074/4096、alloc の失敗 0。ue0 の TX dropped 550183（g10 は 152516、送りが driver の TX の queue を溢れさせた分、SACK で回復）。証拠 build/ws315-hw/p006-d1/、plan/tools/hw5330/evidence/ws315-p006-d1.md。
  **見立ては確かめられた**: 送りの in flight を受けの窓の上限より大きくすると同時の send が戻る（1030 → 2176、Linux 2346）。
- kernel: build/ws315-p006-{k1,g10,g12,d1}-vmunix（d1 は include/kern/net/tcp-socket.h の 256→768 と tcp.c の TCP_SEND_BUFFER 384→1152 KB を一時に書いて build、戻した）。
- 機械の状態: 5330 は d1 の測りに入る前に Debian を確かめた。5330 は Debian、NVMe の zedBSD の kernel は a1（= d1 と同じ binary）。
  10.0.10.3 は使っていない（lock は取っていない、kernel は触っていない）。QEMU は止まっている。
- 確かめの進み: tcp-model PASS（4575db11d の tree、build/tmp/p006-model-a2.txt。往復 10 ms の場面が 32 → 67 MB/s）。21 config の vmunix の build（`build/tmp/p006-buildall.sh b`、build/tmp/p006-buildall-b.txt、build/p006b-*）すべて rc 0・warning 0。
  QEMU `qemu-tcp.sh "" 15 inet,inet6` PASS（build/tmp/p006-qemu-tcp-a.txt、send_ring=768、guest send/recv inet 58.2/34.6、inet6 61.1/24.4 Mbit/s: 速さは emulator の物）。5330 の kernel は build/ws315-p006-a1-vmunix（build/p006-a1、warning 0）。
  rdna-tcp.sh に `--runs both` を足した（2de3b6dc8、Q1 が範囲に足した）。
- 次の 1 手: host が静かな時に 5330 の a1 を測り直す（name p006-a2）、Linux の基準（`5330-linux-both.sh`）、10GbE（基準 = c90327de3 の tcp.c・tcp-socket.h・packet-buf.h を一時に置いた RTL8127 の kernel と、案 A の build/p006b-amd64-rtl8127/vmunix を `--runs send,recv,both,both`）。
  その後: tcp-model・21 config の build（`build/tmp/p006-buildall.sh <tag>`）・QEMU（`plan/ws221/tests/qemu-tcp.sh "" 15 inet,inet6`）・5330・Linux の基準（`5330-linux-both.sh`）・
  10GbE（`flock -n /tmp/zedbsd-rdna.lock`、`plan/ws221/tests/rdna-tcp.sh --kernel build/<rtl8127>/vmunix --bin build/p006-rdna-bin/bin --runs send,recv,recv NAME`、
  終わりに vmunix-va1 cksum 1463007375 に戻し lock を free に）、F-WS315-rcvbuf の解決の文案を Q1 へ。
- 消してよい物（Q1 へ）: build/tmp/p006-base、build/p006a-*、build/p006-k1・p006-g10・p006-g12・p006-d1・p006-amd64。
