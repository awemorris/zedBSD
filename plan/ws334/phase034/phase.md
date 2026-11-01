<!-- awesome-plan project=zedbsd record=ws334-p034 -->

# ws334-p034 試験: libbrowser の試験を Windows（Wine）で

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §7.3 の「既存の libbrowser の試験のうち Windows で流す物」、O28・O29。
- 状態: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-02 q1887-i01、Sonnet。2026-11-01 p003 で作成）。
- **2026-11-01 の変更（DLL を作らず 1 つの .exe、libkeiland の Windows は pthread 無し。ws.md のユーザーの決定、design §6.2・§6.6、build の形は p036）**: `omnigrammaton-cli.exe` は作らない（2026-11-01 ユーザーの答え: 配るのは `OmniGrammaton.exe` だけ）。本文の `omnigrammaton-cli.exe` は試験の program（`plan/ws334/tests` の配らない .exe、p036 の `test-program` で同じ static library を link）に読み替える。
- 依存: p032。
- 担当の想定: Opus（phase-runner）を 1 回（約 5 LW）。WS074 の試験の file と runner は WS074 の物: 変えるなら差分を Q1 に（P8 と調整）。

## 中身

1. host-*.c（WS074 の 91 本前後。今の数を数える）を llvm-mingw で .exe にする script `plan/ws334/tests/host-tests-windows.sh`（`plan/ws074/tests/host-build.sh` の source の一覧と同じ物を `list-sources.sh` から、win32 の OS の層で）と Wine で流す。setenv(XDG_DATA_HOME) は p023 で要らなくなった物以外を記録。
2. `omnigrammaton-cli.exe` を program にして `run-js-tests.py`・`run-test262.py`（`fetch-suites.sh` の取得物、共有の場所を Q1 に確かめる）・`run-html5lib-tree.py`・`run-url-tests.py` を host の python から Wine の program で流す（runner が program の path を引数に取れない時は、runner を変えずに `wine` を前に付ける小さな wrapper の script を PATH に置く形を試す。それで足りなければ runner の変更の差分を Q1 に）。
3. `--render-gpu` の数枚を lavapipe（Wine）と Linux の lavapipe で比べる（`gpu-compare.py`）。
4. Interop 2025 の代表の 100 件（WS074 の一覧から、選び方を結果に書く）。
5. 差の表: 試験ごとに Linux の件数・Wine の件数・差と理由（O28 の strerror の文、時刻帯、path の形、Wine の不具合の疑い）。

## 受け入れ

- 件数の表が結果の節にある（PASS の基準は「差の全部に理由がある」。Windows の不具合と分かった物は Bug として Q1 に）。commit、ws.md の行。

## 結果（2026-11-02 04:40、Sonnet、q1887-i01、基点 4f58e7b9b）

Wine の結果は参考で、実機の Windows の判定ではない（ws.md の決定 20）。PASS の基準は「差の全部に理由がある」。

### 道具と commit

| SHA | 中身 |
| --- | --- |
| 77cdd4590 | `userland/desktop/libbrowser/paint/vulkan.c`: `--render-gpu` の offscreen（`offscreen_device`）が `vkCreateInstance` の前に `wb_os_library_open` で `vulkan-1.dll` を確かめ（`_WIN32` の時だけ）、無ければ `VK_ERROR_INITIALIZATION_FAILED`（operation は `loading vulkan-1.dll`）。確かめた後は閉じる（遅延の import が改めて読む）。libkeiland の `win32/present.c` の `kl_window_vulkan_extensions` と同じ考え |
| 34718204f | `plan/ws334/tests/`: `host-tests-windows.sh`（1 本の script）、`binary-mode.c`・`win32-posix.h`（Windows の host 試験の下請け）、`compare-ppm.py`・`compare-reftests.py`（比べる道具） |

`plan/ws334/tests/host-tests-windows.sh`（`ONLY="build host runners gpu loader interop"` で部分、`TEST262_FILTERS` で test262 の範囲。Wine の各命令は `flock /tmp/zedbsd-wine.lock`、script ごとの flock は取らない）:

1. Windows の library（`make -f apps/browser-win32/Makefile layers libbrowser-core libraries vulkan headers`、`test-program browser-cli`）と host の build（`host-build.sh plain`）。
2. `plan/ws074/tests/host-*.c` の 92 本を、Windows の build と同じ flag で .exe にし（全 static library と link）、host と Wine で repository の根から流して exit と出力（CR を除く）を比べる。
3. runner を host の program と、前に `wine` を付けた wrapper の program で 2 回: `run-js-tests.py`（browser-cli.exe）、`run-test262.py`（host-js.exe）、`run-html5lib-tree.py`（host-tree.exe）、`run-url-tests.py`（host-url.exe）。runner は変えていない。
4. `gpu-compare.py` を host と Wine（どちらも lavapipe）で流し、7 頁 × GPU・CPU の PPM を host と Wine で比べる。
5. `vulkan-1.dll` 無し（`WINEDLLOVERRIDES=vulkan-1=d`）の `--render-gpu`。
6. Interop 2025 の代表の 100 件（`run-wpt-reftests.py`）。

最後の試験の program 側の下請け（WS074 の試験の file は変えていない）:

- `binary-mode.c`: 標準の stream と file を binary にする（`_CRT_fmode`・`_setmode`）。これが無いと LF が CRLF になり host-base の file の試験が落ちる。
- `win32-posix.h`（`-include`）: `setenv`（`_putenv_s`）と `realpath`（`/X:/…` の形）。`setenv("XDG_DATA_HOME", …)` を呼ぶ試験は host-form・host-link・host-position・host-relayout・host-sticky・host-view の 6 本で、p023 の後も残っている（Windows では XDG_DATA_HOME を読まないので効果は無い）。
- host-heap は、64 bit の state を `unsigned long` に持つので（Windows は 32 bit）`unsigned long long` に直した複写から作り、`-fno-inline` を付ける（下の「差の表」）。

### 件数の表（最後の通し: `timeout 9000 sh plan/ws334/tests/host-tests-windows.sh`、1209 秒、`host-tests-windows: done (7 differences to explain)`）

| 試験 | host | Wine | 差 |
| --- | --- | --- | --- |
| host-build（plain） | 94 本 warning 0 | — | — |
| host-*.c（.exe にできた数） | 92 | 92 | なし（最初は host-heap の 64 bit 定数・setenv・realpath で 8 本が build できず、下請けで解いた） |
| host-*.c の exit と出力が同じ | — | 85 / 92 | 7 本（下の表） |
| run-js-tests（`--js`） | 17/17 | 17/17 | なし（出力も同じ） |
| run-url-tests | url 896/896・data 72/72 | url 896/896・data 72/72 | なし（出力 byte 一致） |
| run-html5lib-tree | 1854/1959（fragments 206/206） | 1854/1959（fragments 206/206） | なし（出力 byte 一致） |
| run-test262 test/built-ins/Math/ | 312/327 | 312/327 | 失敗の一覧が同じ（15） |
| run-test262 test/built-ins/JSON/ | 142/165 | 142/165 | 同じ（23） |
| run-test262 test/built-ins/Array/prototype/map/ | 201/216 | 201/216 | 同じ（15） |
| run-test262 test/language/statements/for/ | 368/385 | 368/385 | 同じ（17） |
| run-test262 test/annexB/ | 392/1086 | 392/1086 | 同じ（694） |
| gpu-compare（GPU と CPU の一致、7 頁） | 7 agree | 7 agree | なし |
| 絵（7 頁 × GPU・CPU、800×600）の host と Wine | — | — | 14 枚とも 1 画素も違わない（最大の channel の差 0） |
| `--render-gpu` の `vulkan-1.dll` 無し | — | exit 1、`browser: cannot draw on the GPU: loading vulkan-1.dll failed (-3)` | 直す前は遅延の import の例外（p032 の記録。直す前の再現はこの Phase では取り直していない） |
| `--render-gpu` の `vulkan-1.dll` 有り（lavapipe） | — | exit 0、PPM 360015 bytes | — |
| Interop 2025 の 100 件（css-flexbox・css-grid・css-backgrounds・css-transforms・css-text の各 20） | pass 12・19・7・8・5 / fail 8・1・12・12・15 / error 0・0・1・0・0 | 同じ | 各件の状態が全件同じ、different_pixels も全件同じ |

test262 の範囲の選び方: 全部（47792 件）を host は 151 秒で流せるが、Wine は 1 つの driver が `flock` で 1 本ずつになり、chunk（400 件）の中に止まる試験が 1 つあると 100 秒で打ち切って 1 件ずつの再試行になるので、全部は数時間かかる。runner の変更はせず、Phase の範囲の 5 つの directory（約 2200 件）にした（`TEST262_FILTERS=all` で全部）。Interop の 100 件の選び方: WS074 の Interop の選別の入力（外の metadata の repository と WPT の MANIFEST.json）がこの機械に無いので、Interop 2025 の focus area に当たる 5 つの directory から `run-wpt-reftests.py --limit 20`（runner の層化の選び、path の順）で各 20 件。

### 差の表（host-*.c 7 本、`host-table.txt`）

| 試験 | host | Wine | 理由 |
| --- | --- | --- | --- |
| host-canvas-background | pass | FAIL（canvas の背景の画像が出ない） | Windows の不具合（下の Bug 候補 1）。`--dump=paint` でも画像の item が無い。data: の画像は出る |
| host-file-subresource | PASS | 9/11 | Bug 候補 1。ただし失敗した 2 本は `file://` + `realpath` の形を試験が作る前提（POSIX の path）で、`/Z:/…` の base は Windows の絶対 path でない（`EINVAL`）。試験の前提と本物の不具合が混ざる |
| host-svg-length | 60/60 | 52/52 で exit 2（`Cannot read properties of null (reading 'width')`） | 試験が `/tmp/…` の path を頁の base にして SVG を読む。同じ file: 副資源の系統と見る（個別には切り分けていない） |
| host-svg-text | 42/42 | 33/34 で exit 2 | 同上 |
| host-heap | 31 checks, 0 failed | 31 checks, 0 failed、生存 cell 数が 8783 → 8781 | 保守的な stack の走査が見る語が違う（OS・compiler で揺れる）。理由あり |
| host-interp | 生存 cell 103861 | 103856 | 同上 |
| host-object | 回収 1305・生存 541 | 回収 1308・生存 538 | 同上 |

### Bug の候補（Q1 へ。この Phase では直していない）

1. **Windows で file: の頁の副資源（`<img>`・CSS の `url()`）が読めない**。原因は 2 つ（`/home/awe/zedBSD-worktrees/s-ws334-p034/userland/desktop/libbrowser/page/link.c` と `page/images.c`）。debug の program（`build/tmp/p034/dbg/dbg.c`、Wine）で確かめた: `page_load_file` の base は `Z:\…\t.html`、`page_resolve_file(base, "a.png")` は `Z:\…\a.png` を返し読める、ところが
   - `page_fetch(base, 相対 href)` が `EACCES`: `link_base_local` が `base[0] == '/'` だけを local の頁とみなす。Windows の base は `Z:\…`（と UNC）。
   - `images_fetch`（`page/images.c`）が `page_resolve_location` の返す local の path を href として `page_fetch` に渡し、Windows の `Z:\…` は scheme `z` の URL と解されて `EPROTONOSUPPORT`（POSIX の `/abs/path` は file の base に対する相対 URL として読めていた）。
   - 直しの案: `link_base_local` は `wb_os_path_is_absolute(base)`（と file: URL）で判定する。`images_fetch`（と `page_fetch` を local の path の location で呼ぶ所）は、location が local の path の時は `net_url_from_file_path` で file: URL にして渡す（または `page_resolve_location` が file の頁では file: URL を返す）。href 全般を `wb_os_path_is_absolute` で判定すると、http の頁の `/style.css`（ルートからの相対）を壊すので、base が local の時の location だけにする。
   - 影響: Windows の OmniGrammaton で file: の頁（ローカルの HTML）の画像・CSS の画像が出ない。http・https の頁と data: は影響なし（reftest の 100 件は local の HTTP server から読むので host と同じ）。`wb_os_path_from_url`・`wb_os_url_from_path` 自体は設計どおり。
2. **`vm_heap_set_stack_base` の契約（WS074 の heap）**: base は「cell を持つ一番外の関数の `__builtin_frame_address(0)`」だが、Win64 の clang は rbp を frame の天辺でなく途中（`lea 0x80(%rsp), %rbp`）に置くので、その関数自身の local（rbp より上）は走査の範囲に入らない。host-heap は main の local に cell を持ち、-O2 で試験が main に inline されると生存の cell を回収して落ちる（`-fno-inline` で 31 checks 0 failed）。shell の本体は cell を base の関数の local に持たないなら実害は無いが、`browser_main` など base を取る関数が cell を持つ形にしないという条件を header の comment に足すか、base を取る関数を cell を持つ関数の外側にする。
3. **test262 の `built-ins/Array/from/iter-map-fn-err.js` が host でも Wine でも止まり、メモリが約 4 GB まで増える**（`--js` で harness と test を繋いで確かめた、host も 20 秒で終わらない）。Windows の不具合でなく WS074 の engine の不具合（iterator を閉じる所）。runner は host で 120 秒の打ち切り → 1 件ずつの再試行で通る。Wine では打ち切りの後の Wine の server の立ち直しで続く数件が 10 秒の制限に掛かり「timeout-or-crash」になるので、全部の test262 を Wine で流す時はこの試験を除くか engine を直す。

### 気づいた事（Q1 へ）

- 他の担当の `cross-build.sh`（外の `flock /tmp/zedbsd-wine.lock` を 5400 秒まで持つ）の間は、この script の Wine の各命令がそれを待つ（待ち時間が 10〜30 分出た）。
- 本物の Windows では ws.md の決定 20 のとおり host の Windows の機械で別の担当が流す。この script は Wine 専用（`wine`・lavapipe の ICD の path・Linux の python の runner を使う）。

### 他の確かめ

| 確かめ | 結果 |
| --- | --- |
| zedBSD amd64（`make ZEDBSD_CONFIG=plan/ws334/tests/config-amd64-p027-guest.mk BUILD=build/p034 build/p034/dynamic/libbrowser.so`） | exit 0、warning 0 |
| host の build（`host-build.sh plain`） | 94 本 warning 0 |
| Windows の build（libraries、browser-cli） | warning 0 |
| `git diff --check` | 出力なし（commit 前に確認） |

### 未実施

- 実機の Windows と QEMU の Windows の guest での同じ試験（`plan/ws334/tests/host-tests-windows.sh` は Wine 専用）。
- winarm64 の host 試験。
- test262 の全部と Interop の本来の選別（外の metadata が無い）。
- `vulkan-1.dll` 無しの `--render-gpu` の「直す前」の再現（p032 の記録に依る）。

### 消してよい物（Q1 へ）

`build/tmp/p034`（試しの build・log・debug の program・PPM）、`build/tmp/ws334-host-tests-windows.run.*` のうち `build/tmp/ws334-host-tests-windows` が指していない物、`build/tmp/ws334-host-tests-windows.old.*`（無ければ無し）、`build/p034`（zedBSD amd64 の試しの build）、`build/ws074-suites`（`build/tmp/p034/suites` への symlink。suite の実体は p8-ws074 の wpt と p7-ws334 の test262 への symlink で、消さずに他の worktree の物を指しているだけ）。
