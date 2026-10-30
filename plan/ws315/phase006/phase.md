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

## 引き継ぎ（2026-10-30 09:16、次のセッションが自動で再開する点）

- 最後の commit: この節を含む commit（Q1 に送る SHA）。その前の実装 8b9523b20。tcp.c は倍率を `TCP_RECEIVE_AUTO_GROWTH_EIGHTHS`（既定 16 = 2 倍、k1 と同じ動き）にまとめた形で commit 済み。
  未 commit の source は無い。g10・g12 の kernel は倍率の行を一時に 10・12 に書き換えて build し、16 に戻した（`build/p006-g10`・`build/p006-g12`）。
- 機械の状態: 5330 は Debian で起動している（09:16 に uname で確かめた）。NVMe の zedBSD の kernel は g10（vmunix.old は k1）、userland は p003 の img1 のまま、zedbsd.cfg に CFG_ADD の行は無い。
  10.0.10.3（RDNA 機）は使っていない（lock は取っていない、kernel は触っていない）。QEMU は止まっている。
- 次の 1 手: (1) g12（1.5 倍、`build/ws315-p006-g12-vmunix` は build 済み・未試験）を 5330 で測る: `ssh solaris10-man uname -a` で Debian を確かめ、
  `MODE=--nvme-kernel sh plan/ws315/tests/5330-rtl.sh build/ws315-p006-g12-vmunix p006-g12 30`。(2) 同時の send を戻す手: 倍率をさらに下げる（1.0〜1.125）か、
  受けの往復の最小からの延び（elapsed / rtt）が一定を超えたら広げない、のどちらかで同時の send・recv の和が最大になる点を選ぶ。送りの ring（TCP_SEND_QUEUE_MAX 256）を広げる手は
  送りの側で、この Phase の範囲の外（Q1 に諮る）。(3) 決めた倍率で tcp-model（場面の期待値を見直す）・21 config の build・QEMU・5330 を流し直し、Linux の基準（`5330-linux-both.sh`）も 1 回流す。
  (4) 10GbE: `flock -n /tmp/zedbsd-rdna.lock` を取り Q1 に知らせ、`plan/ws221/tests/rdna-tcp.sh --kernel build/<rtl8127 の build>/vmunix --bin build/p006-rdna-bin/bin --runs send,recv,recv NAME`
  （bin は build 済み: tcp-perf・sysctl）。終わりに Debian・NVMe の kernel を vmunix-va1（cksum 1463007375）に戻す。(5) VF2 など小さな板の memory: 64 bit の pool は 4096（約 9.3 MB）、
  32 bit は自動なし（build で確かめ済み）。(6) F-WS315-rcvbuf を「解決（WS315 p006）」にする文案を Q1 に送る。
- 消してよい物（Q1 へ）: build/tmp/p006-base（基準の tcp-model 用の tree の写し）、build/p006a-*（21 config の build）、build/p006-k1・p006-g10・p006-g12・p006-amd64。
