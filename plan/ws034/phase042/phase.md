<!-- awesome-plan project=zedbsd record=ws034p042 -->

# ws034-p042: TCP で大きな write が届かない

Phase ID: `ws034-p042`
Parent: [WS034](../ws.md)
Status: **cleared**（q335-i01、2026-09-23）
Phase disposition: normal
Queue: q335（q335-i01）
実行: メインセッション

## きっかけ

ws034-p017（curl）の試験で、loopback の TCP で大きな write が届かなかった。ws034-p005 から切り出した。

## 再現（`plan/ws034/tests/tcp-bulk.c`）

loopback で1回の write に N byte を送り、受け側が数と中身を確かめる（20 秒で打ち切り）。
**修正前: 4096 byte までは届き、それを超えると受け側は 4096 byte の後に EOF を受け取る**（`evidence/before.txt`）。

## 原因と修正（`src/kern/net/tcp.c`、`src/kern/net/socket.c`）

調べると、互いに重なった原因が6つあった。

| # | 原因 | 修正 |
| --- | --- | --- |
| 1 | 受け側は、受信 queue に入らなかった segment でも `receive_next` を進めて ACK していた（`socket_enqueue_packet()` の結果を見ていなかった）。queue は socket ごとに最大8個の packet なので、9個目からの data は**届いたことにされて捨てられた** | queue に入れてから進める。入らなければ ACK を進めず、送り側が再送する |
| 2 | window は常に固定の 4096 を広告していた | queue の空き（byte と、packet の残り数 × MSS の小さい方）を広告する。packet は stack 全体で32個の共有 pool から来るので、packet 数の上限は残した |
| 3 | window だけを開く ACK（読み手が空きを作ったとき）で、送り手が起こされなかった | window が開いた ACK でも送り手を起こす。window が1 segment より小さくなっていたら、読んだ側が window update の ACK を送る |
| 4 | 失われた segment の回復は、timeout ごとに一番古い1つを再送するだけだった。受け側は順序どおりの data しか取らないので、後続の分だけ timeout が重なる | 再送した segment が ACK されたら、次の segment をその場で再送する（ACK が実際に segment を片づけたときだけ。重複 ACK では回さない） |
| 5 | `close()` で endpoint がすぐに解放され、**まだ ACK されていない data と FIN の再送の控えが消えた** | `endpoint_close`（file が参照を持っている間に呼ばれる）で FIN を送り、上限 10 秒待つ。先に閉じる側は FIN まで、後に閉じる側（相手は閉じ終えて消えているかもしれない）は data まで待つ。kernel thread（network worker）は待たない |
| 6 | 送り手の再試行が約1秒（99回）で打ち切られ、blocking の write が EAGAIN で失敗した | blocking では、signal・error・接続の終わりまで待ち続ける |

あわせて、**小さい segment を queue の最後の packet に詰める**（`socket_enqueue_stream()`）。512 byte の segment が packet を1つずつ取ると、8個で queue が埋まるため。

途中で自分の修正の誤りを1つ直した（ACK の後で送り手を起こす判定に、片づけた segment を解放した後の数を使っていた）。

## 検証

| 検証 | 結果 |
| --- | --- |
| `tcp-bulk`（ゲスト、QEMU KVM） | **100 byte〜256 KiB の全 size が届き、中身が一致**（`evidence/after.txt`）。64 KiB は 1.0 秒 |
| curl（ws034-p017）の HTTPS | 既定の 1.5 KiB の ClientHello のまま、`openssl s_server` から 5,360 byte の頁を取得できた |
| amd64 `disk-image` | PASS、新しい warning なし |

## 残したこと（別の Phase に）

- **throughput が低い。** 1 MiB は 20 秒で終わらない（約 300 KB で 15 KB/s 前後）。転送が進むほど遅くなる。
  正しさではなく性能の問題として ws034-p044 に分けた。
- **fork した子の blocking write が、親の fault を止める**（ws034-p043）。`sys_write` は user buffer を `uaccess_pin`
  したまま socket の送信で眠る。その page を fork で共有している親が同じ page に書くと（copy-on-write の fault）、
  子の write が終わるまで待たされる。試験では、書き手が fork の後に自分の buffer を作ることで避けた。
- `FD_SETSIZE` が 32（`include/uapi/select.h`）。
- ゲストで `ps` に fork した子が `kernel` と出る（fork が command 名を写さない）。

## 気づいたこと（試験の進め方）

`ZEDBSD_EXTRA_FILES` で入れる file の中身を変えても、一覧が同じなら rootfs は作り直されない。
その file を `ZEDBSD_EXTRA_INPUTS` にも書く（Makefile の注記どおり）。途中の数回、古い試験 binary で試していた。
