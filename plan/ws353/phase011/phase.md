<!-- awesome-plan project=zedbsd record=ws353-p011 -->

# ws353-p011: render は合成の buffer を数で回す（飛んでいる間は最古の fence、1 ms の覗きを消す）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.6・§5.6、対応表の 1-05・1-06・1-13。

## 目的

render の待ちを一度に一つにし、timeout 付きの覗きを無くす。合成の完了を知らせる専用の thread は置かない（ユーザーの決定 4、Q1 の指示）。slot が飛んでいる間は render が最古の fence を無期限に待ち、終わったら REAPED を出す（main は REAPED で `in_flight`・`bake`・`copies`・scene・shot を進めるので、render が完了を見ないと main と render が互いを待って止まる。設計 §5.6、レビュー S1）。

## 変える file

- `render-thread.c`・`render-thread.h`: `KWL_RENDER_POLL_NS`・`KWL_RENDER_ACQUIRE_NS`・`RENDER_WAIT_MS` を消す。step の機械（設計 §5.6 の 1.〜6.）:
  1. 依頼（stop・park・answer・task・want）を先に見て、出来る事をする。
  2. frame の want で slot に空きがあれば output の image を timeout 0 で一度だけ取る。取れたら head（timeout 0）と READY。
  3. 取れず、slot か task が飛んでいる: 最古（submit の順）の fence を `vkWaitForFences(UINT64_MAX)`、REAPED、1. へ。
  4. 取れず、何も飛んでいない: `vkAcquireNextImageKHR(UINT64_MAX)`。
  5. 依頼が無く、飛んでいる物がある: 最古の fence を無期限、REAPED、1. へ。
  6. 依頼が無く、何も飛んでいない: `changed` の cond を無期限。先回りの acquire（`render_ahead`）はここで timeout 0 の一回だけ（block しない。レビュー S4）。
  park・drain・stop は飛んでいる fence を順に無期限で待ってから答える。fence の reset は render だけ。
- `compose.c`: COMPLETED を REAPED の意味に（今の扱い compose.c:4770〜4840 は同じ）。render の op（`compose_op_wait` の slot の fence compose.c:5137、task の fence 5194、output の acquire 4941）の timeout は render が渡すので、render の側で `UINT64_MAX` と 0 だけを渡す（対応表 1-13、レビュー L7）。head の image は timeout 0 のまま（missed は p010 の SHOWN で。それまでは今の扱い）。

## 先に確かめる事

- 表示が止まった時（display の電源・park、出力の移動）に、render の 4. の acquire が返る事。i915 は flip の latch を 100 ms 待って EIO（xd-picture.c:35・1078〜1115）、worker が `chain->error` を立て（wsi-swapchain.c:4325〜4338）、acquire は error で返る（4592〜4603）と読んだ。amdrdna・nvrtx・videocore・ghci・venus の present が表示の止まりで返るかを読む。返らない driver があれば止まって Q1 に報告（display の thread の park の上限 10 s に掛かる。レビュー S4）。

## 残す振る舞い

- READY・answer・cancel の手順、task の順（task は frame の前に submit）、park と drain の意味、切り替えの打ち切り（T3 の 100 ms ずつ、最大 10 s）、image が空いている時の前の frame の GPU と次の frame の walk の重なり（2. の timeout 0）。

## 確かめ

- build warning 0。host: render の step の試験（WS309 の `render-task` の host 試験を広げる。偽の Vulkan で）: slot の満杯、acquire の block、park の最中の依頼、**layer の焼きの後に依頼が無い時に REAPED が来て main が次を頼める、2 frame の後に依頼が無い時に `in_flight` が 0 に戻る、present の失敗（OUT_OF_DATE）の frame の後に REAPED が来る、`kwl_compose_quiesce` と `kwl_compose_complete` が返る**（レビュー S1・L11）。
- QEMU: 描画の回帰（WS099 の C1・C2、`KWL PERF pass`）、出力の移動・回転・game mode の出入り（p012 の resize・scanout・rotate の guest 試験）、layer cache の焼き（静けさの後）の後に画面の変化が描かれる。WS352 の poll の診断で render の thread の timeout の起床が 0（描画中も）。

## 受け入れ

- 上の判定が PASS。render に timeout 付きの待ちが打ち切り（切り替えの間）の他に無い。結果・SHA・未実施を書く。

## 依存

- p004（順の都合だけ。main loop の Phase と p010 に依らない。REAPED が GPU の完了の直後に来るので、callback・animation・client の buffer の返しは今の意味のまま動く）。
