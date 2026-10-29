<!-- awesome-plan project=zedbsd record=ws074-p262 -->

# ws074-p262: 縦書きの writing-mode を logical の座標で layout する

Status: in-progress
Disposition: normal
Parent: [WS074](../ws.md)
Primary Milestone: MG006
Queue: なし（p260 の一部）
Origin: p260 の gap の測定（2026-10-27）。Chromium が通り browser が落とす grid・flexbox・writing-modes の約 960 件のうち、約 390 件が
vertical-lr/rl を使う。今の layout は横書きだけを前提にしていて、縦書きは grid と inline の一部の個別の対応しかない。

## 設計

- writing-mode が親（の layout の座標系）と違う box を「logical の root」とし、その subtree を logical の座標で layout する。
  - subtree の style を logical に写す: width↔height、min/max、margin・border・padding・inset の四辺（vertical-lr: 物理の left→block-start（top）、
    right→block-end（bottom）、top→inline-start（left）、bottom→inline-end（right）。vertical-rl は block-start が物理の right）、overflow-x↔y、
    aspect-ratio の逆数。writing-mode は horizontal-tb にする（grid.c・inline.c の縦書きの個別の経路は通らない）。flex・grid・gap・align・justify は
    もともと logical なので写さない。
  - 横書きの今の block・inline・flex・grid の algorithm をそのまま走らせる。
  - 結果を物理に戻す: 大きさは入れ替え、四辺は戻し、位置は vertical-lr なら転置、vertical-rl なら親の物理の幅から鏡映。inline の行は logical のまま
    `vertical_inline` で描く（今の描画の縦書きの経路）。vertical-rl は行を右から並べる。
  - subtree の中でさらに writing-mode が変わる box は、その box を別の root として同じことをする（外側の変換は、その box の位置と大きさだけを動かす）。
- 直交の root の大きさ: logical の inline の大きさは、物理の height が決まっていればそれ、なければ親の決まった高さ、なければ viewport の高さ。
  logical の block の大きさ（物理の幅）は中身から決まる（物理の width が指定されていればそれ）。
- tree に今の logical の座標系（writing-mode）を持ち、box の物理の writing-mode がそれと違う時に root にする（position.c の absolute もこれで通る）。

## 受け入れ

- Chromium が通る css-writing-modes・css-flexbox・css-grid の試験のうち、縦書きを使う物が増える。後退 0（全件の測定）、host build の warning 0、Acid 不変。

## 状態

- 2026-10-27: 設計。実装を始める。
