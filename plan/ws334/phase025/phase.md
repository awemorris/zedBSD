<!-- awesome-plan project=zedbsd record=ws334-p025 -->

# ws334-p025 統合: script の core の archive と link の確かめ

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §3.2・§3.3（決定 9 の「core だけで build して library にできる」）。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p021。
- 担当の想定: Opus（phase-runner）を 1 回（約 2 LW）。

## 中身

1. `libbrowser/sources.mk` の `LIBBROWSER_SCRIPT_SOURCES`（p020 で分け、p021 で OS の層の posix の 3 つを足した物）だけで host の静的な archive `libbrowser-script.a` を作る script `plan/ws334/tests/script-core-link.sh`（host の cc、`-std=gnu11 -Wall -Wextra -Werror`。生成の表が要れば libbrowser の Makefile の生成の規則と同じ物を build/tmp/ に作る）。
2. `nm -u` の照合: archive の未定義の symbol が libc・libm・libpthread・libdl の関数と archive の中で定義された物だけ（dom・bind・page・net・html・css・layout・paint・text・image の symbol が無い）。許す表は script の中に（libc の関数の名前の一覧を host の `nm -D /lib/x86_64-linux-gnu/libc.so.6` から作る）。
3. archive だけの program: `plan/ws334/tests/script-core-main.c`（`main` が `browser_script_tool(argc, argv)` を呼ぶだけ。`browser_script_tool` が js/tool.c にあることを確かめる。bind に依れば止めて Q1 に）を archive と link し、`plan/ws074/tests/` の js の試験の一部（`run-js-tests.py` が program の path を引数に取れればそれで。取れなければ数本の .js を直に流して出力を host の `/bin/browser --js` 相当の結果と比べる）を流す。
4. Windows: `apps/browser-win32/Makefile` に `libbrowser-script.a` の target（win32 の OS の層の物で）と `script-core.exe`。Wine で同じ .js の数本（Date の文字・Math・JSON・Promise・正規表現）が host と同じ bytes を出す（O29 の binary の mode は `script-core-main.c` が `_setmode` を呼ぶ。posix では呼ばない: 試験の file なので `#ifdef _WIN32` を使ってよい）。setjmp・longjmp と GC（O26・O27、design §12 R4）の確かめとして、例外を多く投げる .js と GC を起こす .js を Wine で流す。
5. 結果: 未定義の symbol の一覧、試験の件数、Wine の結果。決定 9 の「core だけで library にできる」が成り立つかを結果の節に書く。

## 試験（受け入れ）

- `timeout 1800 sh plan/ws334/tests/script-core-link.sh` → 最後の行 `script-core-link: PASS`（host と Wine の両方）。`cross-build.sh` PASS。`git diff --check`。

## 変えてはいけない物と終わり

- libbrowser の source（この Phase は build と試験だけ。link の閉包が破れていれば直さずに止めて Q1 に）。module の header と .c、試験の道具。HAL・toolchain・共有の build/。rm をしない。push しない。
- commit、SHA と結果を Q1 に、結果の節と ws.md の行。
