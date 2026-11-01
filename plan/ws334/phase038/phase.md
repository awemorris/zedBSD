<!-- awesome-plan project=zedbsd record=ws334-p038 -->

# ws334-p038 Windows の file: の頁の副資源（画像・CSS の url()）

- 親: [WS334](../ws.md)。2026-11-02 06:10 Q1 が p034 の結果から作成。
- Status: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-02 06:52 P5 q1909-i01）
- 依存: p034。

## 目的

Windows（Wine）で file: の頁の `<img>` と CSS の `url()` の画像が出ない不具合を直す（p034 の結果の Bug 候補 1）。

- `page/link.c` の `link_base_local` は `base[0]=='/'` だけを local とみなすので、`Z:\...` の base で `page_fetch(base, 相対 href)` が EACCES になる。
- `page/images.c` の `images_fetch` は `page_resolve_location` が返す local の path を href として渡すので、`Z:\...` が scheme `z` の URL と解され EPROTONOSUPPORT になる。
- 案（p034）: base が local かは `wb_os_path_is_absolute` と file: URL で判定する。location が local の path の時は `net_url_from_file_path` で file: URL にして渡す。http の頁の `/style.css` を壊さないよう、base が local の時だけにする。
- p034 の Bug 候補 2（`vm_heap_set_stack_base` の契約、header の comment）も同じ Phase で header の comment に足す。

## 受け入れ

- `plan/ws334/tests/host-tests-windows.sh` で host-canvas-background・host-file-subresource・host-svg-length・host-svg-text が host と同じになる（GC の揺れの 3 本は除く）。
- host の WS074 の試験（host-build、段 1）の前後が同じ。cross-build・storage-test・scheme-test が PASS。

## 結果（2026-11-02 06:52、P5 q1909-i01、基点 cd8a1e7c58、commit 70b0e9f168）

Wine の結果は参考で、実機の Windows の判定ではない（ws.md の決定 20）。

### 変えた所

| file | 中身 |
| --- | --- |
| `userland/desktop/libbrowser/page/link.c` | `link_base_local` は `base[0] == '/'` の代わりに `wb_os_path_is_absolute(base)`（POSIX では同じ判定）。`link_resolve` は、base が local の頁（絶対 path か file: URL）で href が drive で始まる絶対 path（`link_drive_path`: `wb_os_path_is_absolute` で、先頭が `/`・`\` でない物。POSIX では常に偽）の時、href を `net_url_from_file_path` で file: URL にして base 無しで解く。`page_resolve_location` が Windows の file に書く `Z:\…` を、それを href に `page_fetch` を呼ぶ所（images・sheets・fonts・media・frame-load・script）が読めるようになる。先頭が区切りの path（POSIX の全部、Windows の UNC）は URL の path でもあるので今までどおり URL として解く。http の頁の `/style.css` は変わらない |
| `userland/desktop/libbrowser/vm/heap.c` | `vm_heap_set_stack_base` の comment（契約）に、frame address が bound するのはその関数が呼ぶ frame だけで（Win64 の compiler は frame の天辺でなく途中を指す）、base を取る関数は cell を自分の変数でなく呼ぶ関数に持つ、を足した（p034 の Bug 候補 2）。動作は変えていない |
| `plan/ws334/tests/win32-posix.h` | `realpath` の代わりは `/Z:/…` でなく `Z:/…`（Windows の絶対 path）を返す。host-file-subresource はこれを頁の path にも `file://` の後にも使う（`file://Z:/…` は URL の標準の file host の drive letter の扱いで `file:///Z:/…`） |
| `plan/ws334/tests/host-tests-windows.sh` | host-svg-length・host-svg-text は `"/tmp/ws074-…"` の template（POSIX の絶対 path、Windows では今の drive からの path で local の頁の location にならない）を `"Z:/tmp/ws074-…"` にした複写から作る（host-heap と同じ形、同じ directory）。WS074 の試験の file は変えていない |

### 確かめ

| コマンド | 結果 |
| --- | --- |
| `ONLY=host sh plan/ws334/tests/host-tests-windows.sh`（最後、`build/tmp/ws334-host-tests-windows.run.90HyJU`） | `92 host-*.c, 92 built as .exe, 89 the same on both sides, 3 other`。host-canvas-background・host-file-subresource・host-svg-length（60/60）・host-svg-text（42/42）が host と同じ。残りは GC の揺れの host-heap・host-interp・host-object（exit 0・0 failed、生存の cell の数だけ） |
| 同じ（script の svg の複写の前、`run.8PAODy`） | 87 same・5 other: canvas-background・file-subresource は同じになった。svg の 2 本は Wine で `Cannot read properties of null`（template の `/tmp/…` が原因）。`Z:/tmp/…` の複写を手で build して Wine で流すと host の出力と同じ（`build/tmp/p038/svg`）ので、script に足した |
| host の build（script の中の `host-build.sh plain`） | 94 本 warning 0。Windows の library（script の中の make）も warning 0 |
| `sh plan/ws334/tests/stage1.sh TREE plain` の前後（前は `git archive cd8a1e7c58`、port offset 100、後はこの tree） | `summary.txt` が同じ（host-build exit 0・warning 0・built 94、host-tests 75 exit 0・17 other、js 17/17、loader 11/11、http 15/15、http-async 19/20（前も同じ）、fetch 38/38、component 83 checks PASS）。`host-exits.txt` も diff なし |
| `flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` | `cross-build: PASS` |
| `PORT=18638 sh plan/ws334/tests/storage-test.sh plain` | `test-storage: PASS (222 checks)` |
| `sh plan/ws334/tests/scheme-test.sh plain` | `test-scheme: PASS (83 checks)` |
| `make ZEDBSD_CONFIG=plan/ws334/tests/config-amd64-p027-guest.mk BUILD=build/p038 build/p038/dynamic/libbrowser.so` | exit 0、warning 0 |
| `git diff --check` | 出力なし |

### 未実施

- 実機の Windows・QEMU の Windows の guest、winarm64 の実行（script は Wine 専用）。
- `ONLY=host` 以外（runners・gpu・loader・interop）の再実行。変えたのは local の頁の副資源の解決だけで、それらは http の server か data: を読む（p034）。
- asan の stage1。
- browser の shell（OmniGrammaton.exe）で file: の頁の画像を目で見る確認。

### 消してよい物（Q1 へ）

`build/tmp/p038`（svg の複写の試しの build）、`build/tmp/p038-base`（基点の `git archive` と中の stage1 の出力）、`build/tmp/p038-*.log`、`build/p038`（zedBSD amd64 の試しの build）、`build/tmp/ws334-host-tests-windows.run.8PAODy`（最初の run。最後は `run.90HyJU`）、`build/tmp/ws334-stage1-plain`、`build/tmp/ws334-cross.run.uUoMUj`・`ws334-storage.run.y7uINT`・`ws334-scheme.run.UwFVl6` と各 symlink、`build/ws074-host`・`build/ws074-images`・`build/ws074-tls`・`build/ws107-host`・`build/amd64`（この worktree の build の出力）。`build/tmp/ws334-wine`（prefix）・`build/tmp/ws334-wine-build`・`build/tmp/ws334-win32-sources` は次の Windows の試験で使える。
