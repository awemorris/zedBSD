<!-- awesome-plan project=zedbsd record=ws291-p024 -->

# ws291-p024: NVIDIA: subs-1000 の GPU の 1 回の実行ごとの固定の費用（約 56 µs）を内訳ごとに測って縮める

Status: in-progress（2026-10-31 21:04 P3 q1785 で着手）
Parent: [WS291](../ws.md)
依存: [p023](../phase023/phase.md)（block-linear の置き方で pass ごとの copy が消えた後）

## 目的

2026-10-31 ユーザーから、p023 の実装の後に続けて subs-1000 の固定の費用の内訳を測る、という趣旨の答えを受けた（p023 の Q2 の (b)）。
p014 の nc の診断で、copy を除いた GPU の 1 run は約 60 µs（描画は約 4 µs）、NVK は 1 submit 約 5〜7 µs。

## 範囲（案、Queue を立てる時に確定）

1. run ごとの固定の段（texture pool と shader cache の invalidate、最初の draw の全 state、pass の始めと終わりの barrier の WFI、run の終わりの WFI の fence、
   channel の切り替えの有無）を 1 つずつ外した変種で測る（p014 の rtx-runcost.sh）。
2. 外せる物・軽くできる物を直し、subs-100・subs-1000・big-1000・scene・desktop の後退と画素の比べ。HAL は変えない。

## ユーザーの指示（2026-10-31）

- 2026-10-31 ユーザーから、P3 は RTX での描画の最適化を目標の達成まで進める、という趣旨の指示を受けた。目標は p013・p014 の NVK 並み（big-1000 136.2 fps、subs-1000 75.4 fps）。
  この Queue では p024（subs の 1 run の固定の費用）に加え、big-1000 の残りの差と、zero copy の game mode での big-100 の約 20% の遅れも扱う。
