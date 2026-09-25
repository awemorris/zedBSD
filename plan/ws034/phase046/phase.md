<!-- awesome-plan project=zedbsd record=ws034p046 -->

# ws034-p046: TCP の MSS option と window update

Phase ID: `ws034-p046`
Parent: [WS034](../ws.md)
Status: **cleared**（q344-i01、2026-09-24）
Phase disposition: normal
Queue: q344（q344-i01）
実行: メインセッション

## なぜ

ws035-p039 で見つけた（USB CDC-ECM の上の TCP。ECM の問題ではない）。QEMU の `-netdev user` から 1 MiB を取ると、
246 秒で 600 KB しか進まない。pcap では:

1. ゲストの SYN に **MSS option が無い**。相手（slirp）は 1440 byte の segment を送る。
2. ゲストは受信 window を「受信 queue の空き枠 × 1024 byte」で広告する（ws034-p042 の count-aware window）。
   1440 byte の segment 7 つで window が 1024 に落ちる。相手は 1440 > 1024 なので送らず（SWS 回避）、
   **5.12 秒後の zero-window probe まで止まる**。
3. アプリが読んで枠が空いても、ゲストは **window update を送らない**（window が 0 から開いたときだけ送る）。

## 範囲

- SYN と SYN-ACK に MSS option を付ける（送出 interface の MTU から。ゲストが受けられる大きさ）。
- 受信 window が「1 MSS 以上、または受信 buffer の半分以上」広がったら window update を送る（RFC 1122 4.2.3.3 の SWS 回避の受信側）。
- window の広告の単位（枠 × 1024）が相手の MSS より小さくならないよう、枠あたりの byte 数を相手の MSS に合わせる
  か、枠の数と byte の両方で window を決める。

## 受け入れ

- pcap で SYN に MSS option がある。
- QEMU の `-netdev user` から 1 MiB の fetch が数秒で終わり、5 秒の停止が無い。中身が一致する。
- ws034-p042・p044 の TCP 試験（`tcp-bulk`）が退行しない。

## 結果（q344-i01、2026-09-24）

`src/kern/net/tcp.c` を直した。試験は QEMU（KVM）で、USB CDC-ECM（boot disk と同じ xHCI、`-netdev user`）の上の
`fetch`（host の HTTP から 1 MiB）と、ゲスト内 loopback の `tcp-bulk`（ws034-p042/p044 の試験）。pcap は QEMU の `filter-dump`。

### 直したもの

1. **SYN・SYN-ACK に MSS option（1024）**。受信の枠 1 つ分。pcap で `02040400` を確かめた。
   ただし QEMU の user network（slirp）はこれを無視して 1440 byte で送ってくる（下の 2 が効く）。
2. **window update**: 前に広告した window が「1 segment 未満」だったか、「2 segment 未満で、今は 2 segment 以上広い」なら、
   読んだ後に ACK を送る。1440 byte で送る相手が 1024 の window に送らず persist timer（5.12 秒）を待つのを避ける。
   広告が十分大きいときは送らない: 何も確認しない window update を、この stack の送信側は「最古の segment が拒否された」と
   読んで即座に再送するので、update を増やすと loopback で余分な再送が増え、20 秒で終わらない回が出た（8 回中 2 回）。
3. **受け取れない segment への ACK（RFC 793）**: すでに受け取った範囲だけの segment（相手が ACK を見なかった再送）と、
   順番でない FIN（相手が FIN の ACK を見なかった再送が多い）に ACK を返す。先の segment には返さない
   （loopback で両端が互いに再送し合う）。
4. **接続の無い segment に RST（RFC 793）**。以前は黙って捨てていたので、解放済みの接続へ相手が FIN を 1 分近く再送し続けた。
   RST には RST を返さない。
5. **SYN・FIN の送信が device の混雑（`ENOBUFS`・`EAGAIN`・`EBUSY`）で失敗しても、再送の ring に残す**。以前は取り消して
   error を返していた。data は書き手がすぐ再試行するのでよいが、close の FIN は誰も再試行せず、**FIN が一度も出ない**接続があった
   （ECM は送信の URB が 1 本で、使用中の間の送信を `ENOBUFS` で捨てる。1 MiB の取得で数百回）。data の segment は今までどおり取り消す。
6. **close は FIN の確認まで待つ**（上限は従来の 10 秒）。以前は後から閉じる側（LAST_ACK）は data だけを待ち、FIN の再送の前に
   endpoint を解放していた。

### 検証

| 検証 | 修正前 | 修正後 |
| --- | --- | --- |
| ECM で 1 MiB の `fetch` | 246 秒で 600 KB（5.12 秒ごとの停止） | 0.13〜2.1 秒、cksum 一致（10 回中 9 回。1 回は 69 秒、下） |
| 接続の終わり（pcap） | ゲストの FIN が出ない、相手の FIN の再送に無応答（47 秒以上） | 10 回中 10 回、ゲストの FIN と相手の FIN が交わされ RST 無し |
| loopback `tcp-bulk` 1 MiB | 8 回中 8 回 OK、0.7〜10.7 秒 | 8 回中 8 回 OK が 2 巡、1.6〜2.7 秒 |
| amd64・pcat・pc98 の CI kernel | — | warning 0 |

loopback の 8 MiB は修正前も修正後も `tcp-bulk` の 20 秒の alarm に間に合わない（約 30 KB/s）。ws034-p044 の既知の遅さで、
この Phase では扱っていない。

### 残り

- **ECM の受信の取りこぼし**: 10 回中 1 回、1 MiB に 69 秒かかった。pcap では、ゲストが window 2048 を広告した直後に
  相手が送った 1440 byte の segment がゲストに受け取られず、相手の再送（約 1.5 秒）を 45 回待った。TCP の判断ではなく、
  受信の frame が ECM／network の経路で落ちている（ECM の送信も URB 1 本で、使用中は捨てる）。**ws035-p045** へ。
- slirp が MSS option を無視する件は QEMU の側で、扱わない。
