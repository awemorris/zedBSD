<!-- awesome-plan project=zedbsd record=ws334-p031 -->

# ws334-p031 統合: 配る形（zip・license・icon・版の情報）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §6.2・§6.4・§6.7・§6.8、決定 21（U9: 名前 OmniGrammaton・icon は今の物・発行者 Awe Morris、U11: zip）。
- 状態: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-02 q1871-i01、P5）。前: planned（2026-11-01 p003 で作成）。
- **2026-11-01 の変更（DLL を作らず 1 つの .exe、libkeiland の Windows は pthread 無し。ws.md のユーザーの決定、design §6.2・§6.6、build の形は p036）**: zip に DLL は入らない（`OmniGrammaton.exe`・`cert.pem`・`share/`、design §6.2）。依存の確かめは .exe の import が Windows の system の DLL・UCRT の `api-ms-win-crt-*`・遅延の `vulkan-1.dll` だけ（同梱の DLL の欄は無い）。OpenSSL と winpthreads の許諾の文は .exe に静的に入る分として同梱する。
- 依存: p019（ico）・p029・p030。
- 担当の想定: Opus（phase-runner）を 1 回（約 5 LW）。使う module の約束は `apps/browser-win32/tools/ico.h`。

## 中身

1. **icon**: `apps/browser-win32/tools/make-ico.c`（host の道具、host の cc で build）: 今の browser の icon の絵（compositor の `userland/desktop/wayland/icons.c` の browser の icon の描き方）を 16・20・24・32・40・48・64・256 px に描き、256 は `userland/desktop/picture/png-write.c` で PNG、他は DIB で `ico_write` に渡し `$(BROWSER_WIN32_OBJ)/OmniGrammaton.ico`。compositor の描き方の code が compositor の内部に依る時は、icon の描画の部分だけを道具の中に書き直す（zedBSD の code なので規則を読んでよい。WS・Phase の番号を comment に書かない）。`.rc` に `IDI_APPLICATION` の代わりの icon（id 1）を足す。
2. **版の情報**: `.rc` の `VERSIONINFO` の版を、`apps/browser-win32/version.mk` の `OMNIGRAMMATON_VERSION`（p028 で置いた仮の 0.1.0）から。
3. **license の同梱**（design §6.8）: `share\licenses\` に zedBSD（Zlib、tree の `LICENSE`）、font 3 つ（`userland/desktop/fonts/` の license の file）、libbrowser の表（`userland/base/licenses/browser/WHATWG-entities`・`Unicode-License-V3`。CLDR の時刻帯の表の分もこの Unicode License V3）、OpenSSL（Apache-2.0、p030 の tarball の `LICENSE.txt`）、mingw-w64 の runtime（llvm-mingw の release の中の `COPYING` の類を探し、winpthreads と CRT の start-up の許諾の文。無ければ止めて Q1 に）、Mozilla の根（packages の ca-certificates の `MPL-2.0.txt`）、hid-input.c 由来の鍵の値は事実の値なので license の file を要しない（記録だけ）。`share\licenses\README.txt` に一覧。
4. **証明書**: packages の ca-certificates の束を `share\certs\ca-certificates.pem` に（p030 が読む path）。
5. **zip**: `browser-win64-zip`・`browser-winarm64-zip`（一番上の Makefile と `apps/browser-win32/Makefile`）: `OmniGrammaton/` の directory を `OmniGrammaton-<版>-win64.zip`（`-winarm64`）に。zip は host の `zip` か python の `zipfile`（`python3 -I`）で、file の時刻を固定して同じ入力から同じ bytes（再現できる）。
6. **依存の確かめ**: `cross-build.sh` に、zip の中身が design §6.2 の一覧と同じ、各 .exe・.dll の `objdump -p` の DLL の名前が許す表（Windows の system の DLL の名前の一覧 + 同梱の DLL）の中、を足す。

## 試験（受け入れ）

- `cross-build.sh` PASS（zip の中身と依存）。`run-unit.sh ico`・`all` PASS。make-ico が作った .ico を ImageMagick の `identify` が 8 枚と読む（独立の読み手の確かめ）。
- Wine＋xvfb-run: zip を展開した folder から `OmniGrammaton.exe` が起動し、Wine の explorer の一覧（`winefile` は無くてよい: `wine cmd /c dir` と `.exe` の icon の resource を `wrestool` か `x86_64-w64-mingw32-objdump` で確かめる）に icon がある。PNG を `build/review/ws334-p031/` に。
- `git diff --check`。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具。compositor（読むだけ）。HAL・toolchain・共有の build/。rm をしない。push しない。
- commit、SHA と zip の path を Q1 に、結果の節と ws.md の行。

## 結果（2026-11-02 00:18、P5、q1871-i01、基点 ed8a3e689）

commit（`git commit -m WIP -- <path>`）:

| SHA | 中身 |
| --- | --- |
| 7b6761792 | `apps/browser-win32/`: `tools/make-ico.c`（host の道具）・`tools/make-zip.py`、`resources/OmniGrammaton.rc`（icon id 1）・`resources/licenses-README.txt`、Makefile（icon・根の束・license・zip の規則）。一番上の `Makefile` に `browser-win64-zip`・`browser-winarm64-zip`。`libkeiland/win32/window.c` の窓の class に program の icon（resource id 1、`LoadIconW`） |
| 41d937378 | `plan/ws334/tests/`: `cross-build.sh`（項 7: zip）・`allowed-imports.txt`（`LoadIconW`）・新しい `zip-contents.txt`・`zip-wine.sh`。`plan/ws334/design.md` §6.2 と §6.10 の 3 の `cert.pem` を `share/certs/ca-certificates.pem` に（p030 の Q1 の判断） |

zip の中身（`OmniGrammaton-0.1.0-win64.zip`、23 file、時刻は全部 1980-01-01 00:00、`OmniGrammaton/` の下）: `OmniGrammaton.exe`、`share/browser/{about,start,text}.html`、`share/certs/ca-certificates.pem`、`share/fonts/keiland{,-bold,-mono,-fallback,-fallback-mono}.ttf`、`share/licenses/` の `README.txt`・`zedBSD-LICENSE.txt`・`Mahora-LICENSE.txt`・`JetBrainsMono-OFL.txt`・`DroidSansFallback-LICENSE.txt`・`Apache-2.0.txt`・`WHATWG-entities.txt`・`Unicode-License-V3.txt`・`OpenSSL-LICENSE.txt`・`MinGW-w64-runtime.txt`・`winpthreads.txt`・`LLVM-LICENSE.txt`・`ca-certificates-MPL-2.0.txt`。DLL は無い。一覧は `plan/ws334/tests/zip-contents.txt`。

確かめ:

| 確かめ | 結果 |
| --- | --- |
| `timeout 5400 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` | `cross-build: PASS`。新しい項 7: `make browser-win64-zip`・`browser-winarm64-zip` の zip が `zip-contents.txt` の 23 file と同じ、同じ directory から作り直すと同じ bytes、`identify` が .ico を 8 枚と読む。項 6a: OmniGrammaton.exe の import は system の DLL 24 個と遅延の vulkan-1.dll だけ（両 arch）。libkeiland.a と libbrowser-ui.a の import 189（win64）・185（winarm64）が全部表の中 |
| `timeout 3000 sh plan/ws334/tests/zip-wine.sh` | `zip-wine: PASS`。zip（9,872,764 bytes）を新しい directory に展開し、`wine cmd /c dir /s /b` が 23 file（`OmniGrammaton.exe` と `share\certs\ca-certificates.pem` を含む）を出す。`wrestool -l` に icon 8 個と group_icon、取り出した group を `identify` が 8 枚と読む。版の resource: CompanyName Awe Morris・FileDescription OmniGrammaton Web Browser・FileVersion 0.1.0・LegalCopyright (C) 2026 Awe Morris・ProductName OmniGrammaton。展開した folder から Xvfb の中で起動して start.html を描いた |
| PNG（worktree の中） | `build/review/ws334-p031/zip-start.png`（展開した zip からの起動）・`icon-256.png`（256 px の icon） |
| `run-unit.sh ico`・`all` | PASS（test-ico 39722 checks posix・win32）・PASS |
| `check-skeleton.sh`（BASE d393d3267） | FAIL（platform.h だけ、基点と同じで p035） |
| `git diff --check` | 出力なし |

自分で決めた点:

- **icon**: compositor の `userland/desktop/wayland/icons.c` は `icons.h`・`<math.h>`・`<string.h>` だけに依るので、書き直さず make-ico に一緒に compile して `kwl_icon_tile(kwl_icon_for_app_id("browser"), …)`（App Home の tile: 色の帯に地球の絵を抜いた物）を使う。16・20・24・32・40・48・64 は premultiplied を straight に戻した DIB、256 は `kl_picture_png`（png-write.c、libz-compat と一緒に host の cc で build）。make-ico は `$(BROWSER_WIN32_OBJ)/host/make-ico`（`HOST_CC`、既定 cc）。窓の class にも resource id 1 の icon を付けた（Wine は WM が無いので見えない。本物の Windows の題の帯と taskbar 用）。
- **根の束**: ca-certificates の package の Makefile から release の名前・URL・SHA-256 を読み（値は 1 か所）、共有の `build/distfiles` に同じ file があれば読み取りで写し、無ければ取得、どちらも SHA-256 を照合して `share/certs/ca-certificates.pem` に。
- **license**: OpenSSL の `LICENSE.txt` は OpenSSL の build が展開した release から、mingw-w64 の runtime と winpthreads は llvm-mingw の `<triple>/share/mingw32/COPYING.MinGW-w64-runtime.txt`・`COPYING.winpthreads.txt`（arm64 は aarch64 の物、同じ文）。llvm-mingw の `LICENSE.TXT`（LLVM の Apache-2.0 with LLVM Exceptions。compiler-rt の builtins が静的に入る。例外で binary の表示は要らないが同梱した）。`README.txt` に各 file が何の license かを書いた。hid-input.c 由来の鍵の値（libkeiland の keys-table）は事実の値なので license の file は要らない（ここに記録だけ）。
- **zip**: python の `zipfile`（`python3 -I apps/browser-win32/tools/make-zip.py`）で名前の順、時刻 1980-01-01 00:00、mode 0644（.exe 0755）、deflate 9。`.zip.new` に書いて名前を替える。zip の target は `dist` の後に毎回作り直す。同じ directory から同じ bytes（cross-build の項 7）。build の directory が違うと .exe の debug 情報の path が違うので zip も違う（同じ build からは同じ）。
- 一番上の `browser-win64-zip`・`browser-winarm64-zip` は `all zip`。
- phase.md の冒頭の「zip の中身 OmniGrammaton.exe・cert.pem・share/」は p030 の Q1 の判断の通り `share/certs/ca-certificates.pem` に読み替えた（design §6.2 も直した）。

気づいた事（Q1 へ）:

- OmniGrammaton.exe は debug 情報（`-g`）込みで 20.8 MB（zip 9.9 MB）。配る物から debug 情報を抜く（`-s` か `llvm-strip`、別の .pdb）かは判断が要る（今は抜いていない）。

未実施:

- 本物の Windows での zip の展開と起動、Explorer での icon の見た目、SmartScreen（署名は後、U11）。
- winarm64 の zip の中の .exe の実行（link と表と zip の中身だけ）。
- Wine の上の https で `share/certs/ca-certificates.pem` が使われる事の確かめ（p030 の試験の範囲）。

消してよい物（Q1 へ）: `build/tmp/p031`・`build/tmp/p031-*`（Windows の試しの build・log・icon の写し）、`build/tmp/ws334-zip-build`・`build/tmp/ws334-zip-wine*`（zip-wine.sh の出力）。
