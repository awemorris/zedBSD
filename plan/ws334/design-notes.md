<!-- awesome-plan project=zedbsd record=ws334-design-notes -->

# WS334 設計の前の覚え書き（Q1）

## libkeiland の Windows の backend: Win32 直か Qt6 か（2026-11-01 ユーザーの問いへの Q1 の答え）

- 他のブラウザ（Q1 の知識、出典の取得はしていない）:
  - Chrome は Windows でも UI の部品（tab・toolbar・menu）を自前の toolkit（Views）で自分で描き、OS の標準の部品は使わない。OS には window・
    DWM の枠・IME（TSF）・accessibility（UI Automation）・file の dialog・font の描き方などを頼る。見た目は「Windows の部品そのもの」ではなく、
    Windows の流儀に寄せた自前の描画。
  - Ladybird は UI を Qt6 で作っている（Linux と Windows、macOS は AppKit の native の UI もある）。Ladybird には自前の UI の toolkit が無いので Qt を使う。
- zedBSD の事情: Keiland の app は libkeiland の UI の toolkit で部品を自分で描く。Windows の backend が受け持つのは window・入力・描画の面・IME・
  clipboard・file の dialog・font・DPI。この形は Chrome と同じ（自前で描き、OS の口に直に乗る）。
- Q1 の推し: Win32 直。理由: (1) 部品を自分で描く以上、Qt の部品の native らしさは使わず、Qt は window と入力の中継だけになる。(2) native の L&F を
  左右する window の枠（DWM・Mica）・IME（TSF）・file の dialog・accessibility は Win32 の方が直に、細かく使える。(3) Qt6 の DLL（数十 MB）と
  license（LGPL の動的 link の約束）を持ち込まない。Qt6 は Linux など他の OS への移植を同じ code で得たい時に意味がある。
- 未確かめ: Chrome・Ladybird の今の実装の細部（版で変わる）。設計の Phase で出典を取る。
