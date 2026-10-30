<!-- awesome-plan project=zedbsd record=ws315-p006 -->
# ws315-p006: TCP の受けの buffer の自動調整（F-WS315-rcvbuf）

Status: in-progress（2026-10-30 08:35 P3 q1696-i01 着手）
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

（未）
