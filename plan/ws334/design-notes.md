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

## 今の libbrowser の cookie・cache・保存（2026-11-01 Q1 が source を読んだ）

- cookie（`net/cookie.c`、`net/net.h` の net_cookie_*）: RFC 6265 の最初の段。process の memory の jar（最大 512 個、古い物から捨てる）だけで、file に保存しない。
  Expires・Max-Age は過去の時の削除にだけ効き、残る cookie は全部 session の間だけ。HttpOnly は script から見えない。jar は file の static の変数で、
  page の thread と loader の network の thread が lock で共有する（libbrowser の instance ごとではなく process に 1 つ）。
- HTTP の cache（`net/loader.c` の loader_cache_*）: memory だけ。200 の response で max-age か ETag を持ち no-store でない物を、URL を key に、
  body の合計 64 MiB まで LRU で持つ。plain な GET だけ。ETag で再検証。file には保存しない。cache は loader（instance の側）が持つ。
- Web Storage（`page/storage.c`・`bind/storage.c`）: sessionStorage は memory、localStorage は origin ごとに
  `$XDG_DATA_HOME/keiland/browser/local-storage/ORIGIN.ls`（無ければ ~/.local/share）へ変更のたびに書く。上限 5 MiB。POSIX の file の口
  （unistd・sys/stat）を直に使う → Windows では platform の層を通す必要がある。
- IndexedDB など他の保存は見当たらない。
- ストレージバックエンドに分ける時の要点: cookie の jar が process の global なので instance ごとにする、cookie と HTTP の cache の保存（disk）は今は無い、
  localStorage の file の場所と口が POSIX と XDG に直に依る。
