<!-- awesome-plan project=zedbsd record=ws353-p011 -->

# ws353-p011: render は合成の buffer を数で回す（1 ms の覗きを消す）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.6・§5.6、対応表の 1-05・1-06。

## 目的

render の待ちを一度に一つにする: 依頼が無ければ cond を無期限、slot が足りない時だけ最古の slot の fence を無期限、output の image は空くまで block。合成の完了を知らせる専用の thread は置かない（ユーザーの決定 4、Q1 の指示）。

## 変える file

- `render-thread.c`・`render-thread.h`: `KWL_RENDER_POLL_NS`・`KWL_RENDER_ACQUIRE_NS`・`RENDER_WAIT_MS` を消す。step の機械を「依頼の cond → slot が要れば最古の fence → acquire（UINT64_MAX）→ READY」に。起きた時に飛んでいる slot を `vkGetFenceStatus` で見て REAPED。park・drain・stop は fence と task の fence を順に無期限で待ってから答える。fence の reset は render だけ。
- `compose.c`: REAPED の扱い（層の絵の完了、scene の有効、`done_serial`）、先回りの acquire の時期（前の present の後）。head の image は timeout 0 のまま（missed は p010 の SHOWN で）。

## 先に確かめる事

- sleep・display の電源を切る手順で、render が acquire に block している時の park が最長で一つの refresh で答えられる事（電源を切る前に park する順）。崩れるなら止まって Q1 に報告。

## 残す振る舞い

- READY・answer・cancel の手順、task の順（task は frame の前に submit）、park と drain の意味、切り替えの打ち切り（T3 の 100 ms ずつ）。

## 確かめ

- build warning 0。host: render の step の試験（WS309 の `render-task` の host 試験を広げる。偽の Vulkan で slot の満杯・acquire の block・park の最中の依頼）。
- QEMU: 描画の回帰（WS099 の C1・C2、`KWL PERF pass`）、出力の移動・回転・game mode の出入り（p012 の resize・scanout・rotate の guest 試験）。WS352 の poll の診断で render の thread の timeout の起床が 0（描画中も）。

## 受け入れ

- 上の判定が PASS。render に timeout 付きの待ちが打ち切り（切り替えの間）の他に無い。結果・SHA・未実施を書く。

## 依存

- p010（callback と release を表示の完了に移した後。逆だと idle の FIFO の client が止まる）。
