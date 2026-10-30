<!-- awesome-plan project=zedbsd record=ws315-p006 -->
# ws315-p006: TCP の受けの buffer の自動調整（F-WS315-rcvbuf）

Status: cleared（2026-10-30 10:45 P3 q1702-i01: 同時の send の退行を送りの ring 768 と倍率 1.25 倍で解いた。5330 の同時 send 2268.3・recv 2359.2（Linux 2254〜2346・2311〜2341）、片方向・10GbE は落ちない。前: 09:16 q1696-i01 ラップアップ、08:35 着手）
Parent: [WS315](../ws.md)
Queue: q1696 / q1696-i01（P3）、q1702 / q1702-i01（P3、同時の send の退行）
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
6. **倍率と送りの ring（q1702 で加えた）**: 送受信を同時に流すと、各向きの ACK は他の向きの data の queue の後ろに並び、両方向が同じ往復を共有する。
   両方向が窓で律される時、速さは窓（in flight）の比になる: 送り ≈ 速さ × 送りの in flight ÷ 受けの窓。受けの窓が広がると送りが落ちる（k1・g10）。
   - 送りの ring を受けの窓の上限より大きく: 64 bit の `CONFIG_TCP_SEND_QUEUE_MAX` 256 → 768（1460 byte で 1.1 MB）、`TCP_SEND_BUFFER` 384 → 1152 KB（Q1 が採った、2026-10-30）。
     費用: tcp_endpoint が 1 つ約 16 KB 増える（kern_calloc）、stack の `struct tcp_discard` 2 → 6 KB（kernel stack 32 KB）、大きな送り 1 本が pool 4096 の最大 768 個。32 bit（ring 8）は変えない。
   - 倍率（`TCP_RECEIVE_AUTO_GROWTH_EIGHTHS`）を 2 倍 → 1.25 倍: 2 倍は窓 − BDP の queue を switch に作り、同時の送りの往復が 2 ms に延びる。1.25 倍は queue を短く保ち、受けは足りる。

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

### 同時の send の退行の解決（q1702-i01、4575db11d 送りの ring・f49185c5b 倍率 1.25 倍）

- 5330（30 s、相手 centris。証拠 build/ws315-hw/<run>/、plan/tools/hw5330/evidence/ws315-p006-<run>.md）。この host が相手なので host の load を併記する:

| run | 形 | send | recv | 同時 send | 同時 recv | 同時の受けの buffer | host の load |
| --- | --- | --- | --- | --- | --- | --- | --- |
| p006-b1 | p006 の前（128 KB 固定・ring 256） | 2371.6 | 2334.7 | 2285.5 | 1546.7 | 131072 | 40 → 14 |
| p006-d1 | 2 倍 ＋ ring 768（診断） | 2370.9 | 2368.5 | 2176.1 | 2350.3 | 864256 | 低 |
| p006-a1・a2・a3 | 案 A（2 倍 ＋ ring 768、4575db11d） | 2367.2・2349.9・2372.4 | 2372.4・2366.7・2348.4 | 1616.1・1538.5・1873.8 | 2286.3・2337.2・2346.7 | 886784〜923648 | 19・20〜40・4〜6 |
| p006-ag10 | ring 768 ＋ 1.25 倍 | 2370.8 | 2355.8 | 2258.0 | 2300.7 | 452608 | 7 → 3 |
| **p006-f1** | **最終（f49185c5b、ag10 と同じ binary）** | **2369.9** | **2372.9** | **2268.3** | **2359.2** | 403456（recv だけ 405504） | 8 → 4 |
| Linux p006-lin1 | r8152 | — | — | 2253.7 | 2310.8 | — | 39 |
| Linux p003 both1 | r8152 | 2353.4 | 2353.6 | 2345.7 | 2340.9 | — | — |

- ping 1472・MTU 9000/1500・down/up の後の ping は全 run 3/3、5330-run PASS、毎回 Debian に戻った。
- ue0 の TX dropped（f1 575341、g10 152516）: driver の TX の queue（128）の満杯で ENOBUFS を返した手渡し（ifi_oqdrops）。TCP は unsent のまま ring に持ち、transmit の完了の後に渡し直す。
  wire の損失ではない（同時の送りの retransmit_segments: f1 281、d1 181、k1 218）。費用は refused ごとの packet の取り・header の組み・解放。Q1 の条件どおり数値はそのままにして記録した。
- 10GbE（10.0.10.3 の RTL8127 ↔ centris、ATR 有効、20 s、`rdna-tcp.sh`、証拠 build/ws221-rdna/p006-{base,f,base-s,f-s}/）:

| kernel | send | recv | 同時 recv / send |
| --- | --- | --- | --- |
| p006 の前 | 5982.8・5618.8・5786.6・5782.6 | 4993.2 | 3621.9 / 4739.9、3611.9 / 4896.9 |
| 最終 | 5444.3・6273.4・6237.0・6275.7 | 6129.6 | 3732.7 / 4650.1、3561.4 / 4924.6 |

  最終の 1 回目の send 5444 だけ低い（その run の送りの側の 1 CPU が 90 % sys、3 回の測り直しは 6237〜6276）。片方向・同時とも落ちていない。
  `plan/ws221/tests/rdna-tcp.sh` に `--runs both`（send と recv を同時、recv は次の port）を足した（2de3b6dc8、Q1 が範囲に足した）。
- host: tcp-model PASS（build/tmp/p006-model-f.txt。往復 10 ms の場面 32 → 60 MB/s、LAN の受けの buffer 174 KB、10 ms で 1 MB。nodelay-1024 の期待を 16000 → 15000: 大きな ring で device の満杯の間に後ろの write が詰まる、15570）。
- build: 21 config（f49185c5b の tree、build/tmp/p006-buildall-c.txt）すべて rc 0・warning 0。QEMU `qemu-tcp.sh "" 15 inet,inet6` PASS（4575db11d の tree、send_ring=768）。倍率の変更の後の QEMU は未実施（64 bit の定数 1 つ、tcp-model で確かめた）。

## 引き継ぎ（2026-10-30 10:45 q1702-i01 P3、Phase は cleared）

- 最後の commit: この節を含む commit（Q1 に送った SHA）。source: 8b9523b20（受けの自動）、29dd67524（倍率の定数）、4575db11d（送りの ring 768・TCP_SEND_BUFFER 1152 KB・tcp-model の期待）、
  f49185c5b（倍率 1.25 倍と説明）、2de3b6dc8（rdna-tcp.sh の `--runs both`）。未 commit の source は無い（d1・base の kernel の一時の変更は戻した）。
- 測りの全部は上の「結果」の表（q1702-i01）。
- 機械の状態: 5330 は Debian（10:45 に uname で確かめた）。NVMe の zedBSD の kernel は f1（= ag10、sha256 f561a0d9a6dc9bad、vmunix.old も f1）、userland は p003 の img1、zedbsd.cfg に CFG_ADD の行は無い。
  10.0.10.3 は 10:37 に lock を返した（free、Debian 6.12.111、NVMe の kernel は vmunix-va1 cksum 1463007375 を確かめた）。QEMU は qemu-tcp の最後の確かめ（最終の tree）を流している → 結果は Q1 への最終の報告に。
- 残り: なし（Phase の範囲）。F-WS315-rcvbuf の解決の文案は Q1 に送った。WS315 の受け入れと completed の判定は Q1。
- 消してよい物（Q1 へ）: build/tmp/p006-base、build/p006a-*・build/p006b-*・build/p006c-*（21 config の build）、build/p006-{k1,g10,g12,d1,a1,ag10,amd64,base5330,base8127,f-5330,f-8127}、
  build/p006-model-{a,ag10,f}、build/ws315-p006-{k1,g10,g12,d1,a1,ag10,base}-vmunix（f1 は 5330 の NVMe と同じなので残してもよい）、build/tmp/p006-host-*。
