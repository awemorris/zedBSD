<!-- awesome-plan project=zedbsd record=ws034p044 -->

# ws034-p044: TCP の throughput

Phase ID: `ws034-p044`
Parent: [WS034](../ws.md)
Status: **cleared**（q338-i01、2026-09-24。改善を記録。残る非効率は下記）
Phase disposition: normal
Queue: q338（q338-i01）
実行: メインセッション

## きっかけ

ws034-p042 で正しさを直した後、loopback で 64 KiB は 1.0 秒だが、1 MiB は 20 秒で終わらない（約 300 KB、
転送が進むほど遅くなる）。

## 範囲

1. どこで待っているかを測る（network worker の起床、再送の timer、window、packet pool の枯渇、512 byte の segment）。
2. 1 segment を MSS（1024）まで使う、packet pool の大きさ、window update と遅延 ACK、worker の起こし方を見直す。
3. 1 MiB・16 MiB の転送時間を修正の前後で記録する。

## 測ったこと

p042 の後、1 MiB が 20 秒で終わらない原因を追った（debug の klog を一時的に入れて、終わったら外した）。

- 読み手の queue（8 packet）が満ちると、送り手の segment が拒まれる。拒まれた segment は、読み手が空けて
  window update の ACK を送っても、**再送の timer（1 秒）まで送り直されなかった**。これが遅さの主因だった。
- close の drain は数 ms で終わっていた（遅さの原因ではない）。packet pool の枯渇も起きていなかった。

## 入れたもの（`src/kern/net/tcp.c`）

**window が開いた ACK で、何も片づけていないのに segment が残っているなら、一番古いものをすぐ送り直す**
（それは失われたのではなく、相手に場所が無くて拒まれたものなので）。

## 結果（`evidence/final.txt`、QEMU KVM、loopback）

| 大きさ | p042 の後 | この Phase の後 |
| --- | --- | --- |
| 256 KiB | 11 秒前後（close の drain を含む） | 0.1 秒 |
| 1 MiB | 20 秒で終わらない | **0.7〜7.6 秒（5回とも完了）** |
| 4 MiB | 20 秒で終わらない（約 2.5 MB で止まる） | **23 秒で完了** |

中身の照合、`TCPBULK_SHARED`（p043）、`cow-stress` も PASS。

## 試して戻したもの

p042 の「回復中は ACK のたびに次を送り直す」は、まだ飛んでいる segment まで送り直しており、4 MiB で
約 9,000 回（ほぼ全 segment）の再送になっていた。NewReno 風に「回復の開始時点までを送り直したら回復を終える」
形に直したところ、**かえって 1 MiB が 20 秒で終わらなくなった**。window の縁で拒まれる segment が多く、
重複の再送がそれを埋めていたためである。元の形に戻した。

## 残る非効率（今後の課題、未計画）

- 重複の再送が多い。本来の直し方は、受け側が順序の外れた segment を捨てずに持つ（out-of-order queue）ことと、
  送り手が window 0 のときに segment を1つ押し込まない（zero window probe を別にする）こと。
- `sys_write` は socket へ 512 byte ずつ渡すので、segment が MSS（1024）の半分になる。
- 1 MiB の所要時間のばらつきが大きい（0.7〜7.6 秒）。
