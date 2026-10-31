<!-- awesome-plan project=zedbsd record=ws334-p031 -->

# ws334-p031 統合: 配る形（zip・license・icon・版の情報）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §6.2・§6.4・§6.7・§6.8、決定 21（U9: 名前 OmniGrammaton・icon は今の物・発行者 Awe Morris、U11: zip）。
- 状態: planned（2026-11-01 p003 で作成）。
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
