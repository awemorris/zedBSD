<!-- awesome-plan project=zedbsd record=ws334-user-questions -->

# WS334 ユーザーへの問い（ws334-p001、2026-11-01 P7）

## 前置き（AGENTS.md §3）

- **どの WS か**: [WS334](ws.md)「libbrowser の Web ブラウザ OmniGrammaton を Windows に移植する」。
- **どんな WS か**: zedBSD の Web ブラウザの engine（libbrowser）を使い、Windows で動く .exe（名前は OmniGrammaton）を作る。移植を通して、libbrowser が
  OS に依らない事を確かめる。libbrowser を 3 つの engine（描画・JavaScript・保存）に分け、保存の部分（cookie・cache・localStorage）を差し替えられる形にする。
  Windows 版は tree の一番上の `apps/browser-win32/` に置き、`make browser-win64` で作る（決定 12・13）。
- **今の状態**: planning。p001（設計）を P7 が書いた（[design.md](design.md)）。実装の担当の割当はまだ（ユーザーの決定で、今は計画だけ）。
- **決まっていない事**: 下の Q1〜Q15。どれも推しの案を書いた。推しのままでよければ「推しで」で足りる。

## 用語

| 用語 | 意味 |
| --- | --- |
| frontend | ブラウザの「窓・ボタン・入力」の部分。engine（libbrowser）は共通で、OS や部品の toolkit ごとに frontend を作る（Win32 用・Qt 用・GTK4 用・zedBSD の Keiland 用） |
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

### Q1 Windows に最初に出す frontend（決定 8・13）

- 問い: Windows 版の最初の frontend を、Win32 を直に使う `apps/browser-win32` にしてよいか。
- 案:
  - A（推し）: `apps/browser-win32`（Win32 直）を先に。Qt の `apps/browser-qt` は Linux・macOS にも出したくなった時に。
  - B: `apps/browser-qt`（Qt6）を先に作り、Windows もそれで出す。
- 推しの理由: 追加の DLL（約 20 MB）と Qt の license（LGPL）の約束が無く zip 1 つで配れる。toolbar に Windows の普通の部品を使えば日本語の入力・読み上げ・
  clipboard が最初から働く。arm64 の Windows も同じ C の build で出せる。Qt は「Windows でも動くが、主に他の OS のため」と位置付ける（design.md §4.2）。

### Q2 libkeiland の Windows の実装（決定 3）をどこでやるか

- 背景: 決定 13 で、Windows のブラウザは libkeiland を使わない形になった。決定 3（libkeiland の Win32 の実装を作る）はブラウザの道から外れる。
  WS327 で thread と network の部分の Win32 版は既に書いてある。残りは窓・入力・titlebar の部分（約 28 LW）。
- 案:
  - A（推し）: 窓の部分は保留中の [WS234](../ws234/ws.md)（libkeiland の DLL 版、Windows 用を含む）へ移し、WS334 では作らない。WS334 は OmniGrammaton の .exe に集中する。
  - B: WS334 に残し、OmniGrammaton の .exe の後に p016・p017 として作る。
- どちらでも、libkeiland を Windows に移す時は Qt6 でなく Win32 直を推す（design.md §4.6）。

### Q3 共通の shell の置き場と、zedBSD の今のブラウザの場所

- 背景: frontend ごとに薄い層を作り、共通の部分（引数、headless の試験の mode、戻る・進む、題、鍵の名前、Vulkan の描画の手順）は 1 か所に置く。
  zedBSD の今のブラウザ（`userland/desktop/browser/`、`/bin/browser`）も同じ共通の部分を使い、2 つが別れないようにする。
- 案:
  - A（推し）: 共通の部分は `apps/browser-shell/`。zedBSD のブラウザは今の場所（`userland/desktop/browser/`）のまま共通の部分を使う（zedBSD の image の package の仕組みに乗っているため）。
  - B: zedBSD のブラウザも `apps/browser-keiland/` に移す（apps/ に frontend が全部並ぶ。zedBSD の package の規則の直しが要る）。
  - C: 共通の部分を `userland/desktop/browser/` の下に置き、apps/ から参照する。

### Q4 cross compile の toolchain（決定 11、Q1 の許可も要る）

- 問い: llvm-mingw（版を固定した release を取得・照合）を host と Windows の host に入れてよいか。
- 推し: 入れる。arm64 の Windows（`make browser-winarm64`）に要る。入るまでは、今の host にある Debian の mingw-w64 gcc で x86_64 だけを先に作る。
  共有の LLVM（toolchain/）は変えない（design.md §6.1）。

### Q5 高 DPI（150% などの拡大の画面）の page の中身

- 背景: engine に「拡大率」が無く、拡大の画面で page の文字が小さく描かれる。engine の対応は描画の全体に及び、WS074（ブラウザの engine の WS）の作業と重なる。
- 案:
  - A（推し）: 第 1 段は拡大しない（文字は小さいがくっきり）。engine の拡大率は WS074 と調整した別の作業に。
  - B: 第 1 段は GPU で拡大して大きさを合わせる（ぼやける）。
  - C: WS334 の中で engine に拡大率を入れる（大きく、WS074 と競合しやすい）。

### Q6 HTTP の cache を disk に残すか

- 推し: 保存の部品（ストレージバックエンド）の口は cache も扱う形で作り、disk に残すのは cookie と localStorage を先に、cache は第 2 段（上限 256 MB）。
  cache を残すと再起動の後の表示が速いが、壊れた時の扱いと容量の管理が増える。

### Q7 Windows の font を使うか

- 推し: 第 1 段は zedBSD と同じ同梱の font（Mahora・JetBrains Mono・Droid Sans Fallback）。見た目が zedBSD と同じで試験を共有できる。Windows の font（Yu Gothic など）を
  選べる設定は後で。

### Q8 libtruetype と libgif-compat の小さな直し（決定 6 の解釈）

- 背景: 決定 6 は「そのまま使う」。Windows では libtruetype の 1 関数（font の file を開く所）が compile できず、binary の file を text として読む恐れがある。
- 推し: 決定 6 を「zedBSD の library を Windows でも使う（他社の FreeType などに替えない）」と読み、この 1 関数と libgif-compat の 1 行だけを直す。

### Q9 web site に名乗る名前（User-Agent・navigator.platform）

- 推し: Windows 版は Windows の Chrome と同じ形（`Windows NT 10.0; Win64; x64`、platform は `Win32`）。site が Windows 用の page を出す。OmniGrammaton の名前は入れない
  （入れると site が知らないブラウザとして扱い、表示が崩れる事がある）。zedBSD 版の名乗りは今のまま（Linux の Chrome の形）。

### Q10 名前・icon・発行者

- (a) zedBSD の上のブラウザも OmniGrammaton と呼ぶか。推し: 製品の名前は両方 OmniGrammaton（窓の題・About）、zedBSD の command の名前 `/bin/browser` と内部の名前 "browser" は変えない。
- (b) icon。推し: 今の zedBSD のブラウザの icon の絵を各大きさに描いて使う。新しい図案にするならユーザーが決める。
- (c) .exe の版の情報の発行者（CompanyName）。推し: 「Awe Morris」（著作権の表示と同じ）。

### Q11 https の暗号の部品

- 案:
  - A（推し）: zedBSD と同じ OpenSSL の DLL を同梱し、信頼する証明書は Windows に登録された物を読む（会社の証明書も効く）。
  - B: Windows 内蔵の SChannel を使う（DLL 不要。code が大きく、zedBSD と挙動が分かれる）。将来の案。

### Q12 配り方と署名

- 推し: 第 1 段は zip（展開して .exe を開く）。installer と署名は後。署名が無い間は Windows が「発行元不明」と警告する。署名には有償の証明書が要る。

### Q13 Windows の host での担当の働き方（決定 11）

- 背景: shell がある程度できたら Windows の host で開発・試験する。担当は SSH で入って build と試験を流すが、SSH の session からは窓が画面に出ない。
- 推し: Windows の host に OpenSSH server・llvm-mingw・Git for Windows（sh）・make・python 3・GPU の driver（Vulkan）・（任意で Vulkan SDK）を入れる。
  窓の試験は「利用者が login した画面で走る予定の task」として起動し、画面の PNG と結果を file で回収する 1 本の script にする。
  ユーザーに決めてほしい事: どの Windows の機械か（x86_64・arm64）、担当が SSH で入ってよいか、login したままにできるか、電源の入れ直しの方法。

### Q14 JavaScript の engine の core の公開の header

- 背景: 決定 9 で JavaScript の core（vm・js）を単独の library として build できるようにする。今の engine の中の header のままでも build はできる。
- 推し: 今は公開の header を作らず、「core だけで library になる」事を試験で保証する。外の program が使いたくなった時に公開の口を決める。

### Q15 Windows の窓の題の帯

- 案:
  - A（推し）: 第 1 段は Windows の普通の枠（暗い mode・Mica）と、その下の toolbar（戻る・進む・再読み込み・URL の欄）。第 2 段で Chrome・Edge のように題の帯に
    部品を入れる形にするかを、第 1 段を見てから決める。
  - B: 最初から Chrome・Edge の形（題の帯に部品、自前で描く）。

## make の target の名前（問いにしない、案の記録）

- `make browser-win64`（x86_64）・`make browser-winarm64`（arm64）。出力は `build/browser-win64/`・`build/browser-winarm64/`（design.md §6.9）。
