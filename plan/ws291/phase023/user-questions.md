<!-- awesome-plan project=zedbsd record=ws291-p023-questions -->

# ws291-p023 ユーザーへの問い（共有の image の画素の並び）

- 2026-10-31 P3（q1781-i01）。設計の本文は [design.md](design.md)、レビューは [design-review.md](design-review.md)。
- 第 1 版の問い（どこまで進めるか・protocol の版 4・他の GPU）は、その後のユーザーの決定（compositor が決める、GPU の一番速い並びを driver ごとの一覧で、RTX の block-linear は
  必須で実装する、game mode は WS309 p015）と Q1 の interface の決めで答えが出たので外した。残りは下の 2 つ。どちらも nvrtx の実装を止めない。

## 前置き

- **どの WS か**: [WS291](../ws.md)（GPU の driver の描画の速さ: desktop の 60 fps と重い 3D）の Phase [p023](phase.md)
  （共有の image の画素の並びを client と compositor で決める）。
- **どんな WS か**: AMD・NVIDIA・PowerVR などの GPU で、desktop の操作を 60 fps に、重い 3D の model を Linux 以上の fps にする。いま NVIDIA（RTX 2070）の
  「1 frame に 1000 回 submit する bench（subs-1000）」が 11 fps（Linux の NVK は 75 fps）で、その一因が「描く pass ごとの全画面の copy」。
- **今の状態**: 設計（第 2 版）が終わり、これから P3 が共有の部分（kernel の約束）と NVIDIA の driver の部分を実装する。分かった事:
  1. NVIDIA の driver の中で image を GPU の並び（block-linear）に置けば、pass ごとの copy は無くなり、1 frame に 1 回（画面へ渡す時）だけ残る。
     見込みは subs-1000 で 11 → 約 16 fps。NVK の 75 fps との残りは、GPU に仕事を 1 回渡すごとの固定の時間（約 56 µs）で、この設計の範囲の外。
  2. NVIDIA の display も block-linear を画面に出せる（hardware の文書にある）。ただし今の driver の作りで正しく読めるかは、実機で確かめるまで分からない。
- **決まっていない事**: 下の Q1・Q2。

### 用語

- **画素の並び**: image の画素を memory にどう並べるか。**pitch-linear** は行ごとに左から右へ並べる素直な並び。**block-linear** は NVIDIA の GPU が得意な、
  小さな block（64 byte × 8 行）ごとに並べる形。
- **scanout**: display の hardware が memory の image を読んで画面に出す事。
- **keiland-shot**: 試験で画面を PNG に撮る道具。compositor が描いた結果を読むので、display が実際に読んだ画素は見ない。
- **CRC**: display が実際に画面に出した画素から計算する検査の値。同じ画像なら同じ値になるので、画面を撮らずに「正しく出たか」を機械で確かめられる。
- **LW**: 作業量の単位（8 LW ≈ 実時間 2〜3 時間）。

## Q1. NVIDIA の display が block-linear を正しく画面に出したかを、どう確かめるか

- **(a) 推し**: display の CRC を読む仕組みを NVIDIA の driver に足す（約 3 LW 増）。同じ画像を pitch-linear と block-linear で出して CRC が一致する事を機械で確かめる。
  以後の回帰の試験にも使える。
- (b) CRC は作らず、画面を目で見る確かめを「ユーザーが拾える試験の一覧」に置く。確かめが済むまで block-linear の scanout は既定で切っておく（その間、game mode の NVIDIA は
  1 frame に 1 回の copy が残る形になる）。

## Q2. subs-1000 を NVK（75 fps）に近づける続きをするか

この設計で届く見込みは約 16 fps まで。残りは GPU に仕事を 1 回渡すごとの固定の時間（約 56 µs、Linux の NVK は約 5〜7 µs）で、その内訳（texture と shader の cache の
無効化、最初の draw の全 state、pass の始めと終わりの待ち、終わりの fence など）を 1 つずつ外して測る作業が要る（p014 の残りの候補）。

- **(a) 推し**: この設計の実装と実機の結果を見てから、続きを別の Phase として決める（今は決めない）。
- (b) この設計の実装の後に続けて、その内訳を測る Phase を足す。
- (c) subs-1000 は約 16 fps で止める。
