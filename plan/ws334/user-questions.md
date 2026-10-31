<!-- awesome-plan project=zedbsd record=ws334-user-questions -->

# WS334 ユーザーへの問い（ws334-p001、2026-11-01 P7）

## 前置き（AGENTS.md §3）

- **どの WS か**: [WS334](ws.md)「libbrowser の Web ブラウザ OmniGrammaton を Windows に移植する」。
- **どんな WS か**: zedBSD の Web ブラウザの engine（libbrowser）を使い、Windows で動く .exe（名前は OmniGrammaton）を作る。移植を通して、libbrowser が
  OS に依らない事を確かめる。libbrowser を 3 つの engine（描画・JavaScript・保存）に分け、保存の部分（cookie・cache・localStorage）を差し替えられる形にする。
  Windows 版は tree の一番上の `apps/browser-win32/` に置き、`make browser-win64`（arm64 は `make browser-winarm64`）で作る（決定 12・13）。
- **今の状態**: planning。p001（設計）を P7 が書いた（[design.md](design.md)）。実装の担当の割当はまだ（ユーザーの決定で、今は計画だけ）。
  ブラウザの窓の部分は決定 15 で決まった: 今の zedBSD のブラウザの UI のまま、その下の libkeiland に Windows の backend を作って動かす（足りなければ後で Win32 を直に使う版）。
- **決まっていない事**: 下の U1〜U15（統括の「Q1」と紛れないよう U の番号にした）。どれも推しの案を書いた。推しのままでよければ「推しで」で足りる。

## 用語

| 用語 | 意味 |
| --- | --- |
| backend | libkeiland の中で OS・toolkit に合わせて差し替える部分（今は Wayland、作るのは Win32、将来 Qt6・GTK4）。ブラウザの UI は 1 つで、backend が OS の差を吸収する |
| Win32 | Windows の OS が直に出す窓・入力の API。追加の部品の library が要らない |
| Qt6・GTK4 | 窓と部品の toolkit（他社の library）。Windows・Linux・macOS で同じ code が動くが、DLL を同梱する |
| libkeiland | zedBSD の desktop（Keiland）の部品の library。今の zedBSD のブラウザはこれを使う |
| DWM | Windows の窓の枠・影・半透明（Mica）を描く仕組み |
| IME・IMM32・TSF | 日本語の入力の仕組み。IMM32 は古く簡単な口、TSF は新しく全部の機能（再変換など）が使える口 |
| toolchain・llvm-mingw・mingw-w64 | Linux の上で Windows 用の .exe を作る compiler の一式。llvm-mingw は clang 製で arm64 の Windows にも出せる |
| DPI・高 DPI | 画面の細かさ。150% の拡大の画面では、engine が拡大に対応していないと文字が小さいかぼやける |
| User-Agent・navigator.platform | ブラウザが web site に名乗る文字列 |
| OpenSSL・SChannel | https の暗号の部品。OpenSSL は zedBSD と同じ物（DLL を同梱）、SChannel は Windows に内蔵の物 |
| 署名 | .exe に付ける発行者の電子署名。無いと Windows が「発行元不明」と警告する |
| LW | 作業の量の単位（8 LW ≈ 2〜3 時間） |

## 問い

### U1 libkeiland の Windows の backend の作り方と、全体の移植の置き場（決定 3・8・14・15）

- (a) 作り方: Win32 を直に使う（推し）か、Qt6 を libkeiland の下に入れるか。推しの理由: libkeiland は部品を自分で描くので Qt の部品を生かせず、Windows では DLL と
  license の約束だけが増える。Qt6・GTK4 の backend は、Wayland の無い所（macOS など）に出す時の物（将来の `apps/browser-qt`・`apps/browser-gtk4`）（design.md §4.3）。
- (b) 置き場: browser に要る最小の部分（窓・入力・日本語入力の composition・DPI・clipboard・暗い mode）は WS334 の前半、browser の質に効く物（TSF・drag と drop・touchpad の
  慣性・accent の色・Mica）は WS334 の後半、browser の使わない libkeiland の API の全部（file の dialog・menu・設定など）と他の Keiland の app を Windows で動かす事は
  保留中の [WS234](../ws234/ws.md)（libkeiland の DLL 版）を推す（design.md §4.9）。WS234 を再開する時期はユーザーが決める。

### U2 ブラウザの UI の source の置き場

- 推し: 今の場所（`userland/desktop/browser/`）のまま。zedBSD の image の package の仕組みに乗っているため。`apps/browser-win32/`（将来 `apps/browser-qt`・`apps/browser-gtk4`）は
  入口・resource・build だけを持ち、同じ UI の source を compile する。
- 代案: UI の source を `apps/browser/` などへ移し、zedBSD の package がそこを compile する（apps/ に全部が並ぶ。zedBSD の package の規則の直しが要る）。

### U3 cross compile の toolchain（決定 11。統括の Q1 の許可も要る）

- 問い: llvm-mingw（版を固定した release を取得・照合）を host と Windows の host に入れてよいか。
- 推し: 入れる。arm64 の Windows（`make browser-winarm64`）に要る。入るまでは、Debian の UCRT 版の mingw-w64 の gcc（package `gcc-mingw-w64-ucrt64`、apt で入る。
  今入っている版は古い C の runtime（msvcrt）で作られていて新しい runtime（UCRT）と混ぜられない）で x86_64 だけを先に作る。この package の導入も許可の対象。
  共有の LLVM（toolchain/）は変えない（design.md §6.1）。
- 対象の Windows: Windows 10 1903 以降（x64）と Windows 11（arm64）。日本語の名前の folder の file を開くための設定（UTF-8 の code page）が 1903 から働くため。

### U4 高 DPI（150% などの拡大の画面）の page の中身

- 背景: engine に「拡大率」が無く、拡大の画面で page の文字が小さく描かれる。engine の対応は描画の全体に及び、WS074（ブラウザの engine の WS）の作業と重なる。
- 案:
  - A（推し）: 第 1 段は拡大しない（文字は小さいがくっきり）。engine の拡大率は WS074 と調整した別の作業に。
  - B: 第 1 段は GPU で拡大して大きさを合わせる（ぼやける）。
  - C: WS334 の中で engine に拡大率を入れる（大きく、WS074 と競合しやすい）。

### U5 HTTP の cache を disk に残すか

- 推し: 保存の部品（ストレージバックエンド）の口は cache も扱う形で作り、disk に残すのは cookie と localStorage を先に、cache は第 2 段（上限 256 MB）。
  cache を残すと再起動の後の表示が速いが、壊れた時の扱いと容量の管理が増える。

### U6 Windows の font を使うか

- 推し: 第 1 段は zedBSD と同じ同梱の font（Mahora・JetBrains Mono・Droid Sans Fallback）。見た目が zedBSD と同じで試験を共有できる。Windows の font（Yu Gothic など）を
  選べる設定は後で。

### U7 libtruetype と libgif-compat の小さな直し（決定 6 の解釈）

- 背景: 決定 6 は「そのまま使う」。Windows では libtruetype の 1 関数（font の file を開く所）が compile できず、binary の file を text として読む恐れがある。
- 推し: 決定 6 を「zedBSD の library を Windows でも使う（他社の FreeType などに替えない）」と読み、この 1 関数と libgif-compat の 1 行だけを直す。
- 補い: これらの library は Windows の compiler で warning が出るかもしれない（Linux の試験の build でも warning を誤りにしていない）。推し: warning を消すための直しはせず
  数を記録するだけにし、「warning 0」の約束は WS334 が書く・変える code に当てる。

### U8 web site に名乗る名前（User-Agent・navigator.platform）

- 推し: Windows 版は Windows の Chrome と同じ形（`Windows NT 10.0; Win64; x64`、platform は `Win32`）。site が Windows 用の page を出す。OmniGrammaton の名前は入れない
  （入れると site が知らないブラウザとして扱い、表示が崩れる事がある）。zedBSD 版の名乗りは今のまま（Linux の Chrome の形）。

### U9 名前・icon・発行者

- (a) zedBSD の上のブラウザも OmniGrammaton と呼ぶか。推し: 製品の名前は両方 OmniGrammaton（窓の題・About）、zedBSD の command の名前 `/bin/browser` と内部の名前 "browser" は変えない。
- (b) icon。推し: 今の zedBSD のブラウザの icon の絵を各大きさに描いて使う。新しい図案にするならユーザーが決める。
- (c) .exe の版の情報の発行者（CompanyName）。推し: 「Awe Morris」（著作権の表示と同じ）。

### U10 https の暗号の部品

- 案:
  - A（推し）: zedBSD と同じ OpenSSL の DLL を同梱し、信頼する証明書は Windows に登録された物を読む（会社の証明書も効く）。
  - B: Windows 内蔵の SChannel を使う（DLL 不要。code が大きく、zedBSD と挙動が分かれる）。将来の案。

### U11 配り方と署名

- 推し: 第 1 段は zip（展開して .exe を開く）。installer と署名は後。署名が無い間は Windows が「発行元不明」と警告する。署名には有償の証明書が要る。

### U12 Windows の host での担当の働き方（決定 11）

- 背景: shell がある程度できたら Windows の host で開発・試験する。担当は SSH で入って build と試験を流すが、SSH の session からは窓が画面に出ない。
- 推し: Windows の host に OpenSSH server・llvm-mingw・Git for Windows（sh）・make・python 3・GPU の driver（Vulkan）・（任意で Vulkan SDK）を入れる。
  窓の試験は「利用者が login した画面で走る予定の task」として起動し、画面の PNG と結果を file で回収する 1 本の script にする。
  ユーザーに決めてほしい事: どの Windows の機械か（x86_64・arm64）、担当が SSH で入ってよいか、login したままにできるか、電源の入れ直しの方法。

### U13 JavaScript の engine の core の公開の header

- 背景: 決定 9 で JavaScript の core（vm・js）を単独の library として build できるようにする。今の engine の中の header のままでも build はできる。
- 推し: 今は公開の header を作らず、「core だけで library になる」事を試験で保証する。外の program が使いたくなった時に公開の口を決める。

### U14 cookie を暗号にして保存するか

- 背景: 再起動の後も cookie（login の状態など）を残すため、disk に保存する。今の設計では平文（今の zedBSD の localStorage も平文）。Windows の Chrome は利用者の
  login に結び付いた暗号（DPAPI）で cookie を保存する。平文だと、同じ PC の他の program が cookie を読める。
- 推し: 第 1 段は平文で保存し、Windows の暗号（DPAPI）は WS334 の後半（p016）で足す。zedBSD・Linux での暗号は後で別に決める。

### U15 決定 9 の読み（どの engine を差し替えられるようにするか）

- 背景: 決定 9 は「描画・JavaScript・保存の 3 つの engine がそれぞれ interface を持ち、その実装を libbrowser の instance に登録する」。決定 10 は「描画と JavaScript は今の
  source で分かれているので、その分離を保てばよい」。
- 推し: instance に**登録して差し替えられる**のは保存（ストレージバックエンド）だけにし、描画と JavaScript は今の内部の interface で libbrowser の中で結ぶ（差し替えの口は作らない）。
  JavaScript の core が単独の library になる事は試験で保証する（U13）。
- 代案: 描画と JavaScript も instance に登録する公開の口を作る（公開の API が大きくなり、WS074 の作業と重なる）。

## make の target の名前（問いにしない、案の記録）

- `make browser-win64`（x86_64）・`make browser-winarm64`（arm64）。出力は `build/browser-win64/`・`build/browser-winarm64/`（design.md §6.9）。
