<!-- awesome-plan project=zedbsd record=ws168-p003 -->
# ws168-p003: `keiland-preview`（縮小表示を作る隔離された command）

Phase ID: `ws168-p003`
Parent: [WS168](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-395（p004 の QEMU で sandbox_spawn も通る））（旧: in-progress（2026-10-07 q834 P2: 実装と host（Linux）の試験 PASS、zedBSD の build warning 0。sandbox_spawn で起こす QEMU の確認は p004 の T1 にまとめる））
Queue: q834（2026-10-07、P2）
設計: [p001](../phase001/phase.md) §4・§7、判断 H2・H5・H6

## 範囲（Q1 の ACK 2026-10-07）

正常系。PDF の埋め込まれていない font の代わり（image に埋め込む口）と FreeBSD の Capsicum の口は backlog（Q1「backlog で可」）。

## 実装（2026-10-07 P2）

- `userland/desktop/preview/`（新）: `preview.h`・`main.c`（引数、fd 0・1 の fstat、confine、fd 0 を 64 MiB まで読み、復号・縮小、P6 を write で fd 1 へ）・
  `decode.c`（PNG・JPEG（EXIF の向き）・GIF の最初の frame・P5/P6・PDF の 1 頁目を白の上に、要る scale で）・`scale.c`（contain は大きくしない、cover は真ん中を切る、
  覆う面積で重みを付けた平均）・`zedbsd/confine.c`（何もしない: kernel の sandbox）・`linux/confine.c`（`PR_SET_NO_NEW_PRIVS` と seccomp-bpf: read・write・pread64・
  lseek・fstat・close・PROT_EXEC 無しの mmap と mprotect・munmap・mremap・madvise・brk・futex・clock_gettime・getrandom・rt_sigreturn・exit・exit_group、他は KILL_PROCESS）。
  終了の status は設計 §4.1。試験用の build（`PREVIEW_TEST_ESCAPE=1`）だけが `--test-escape=open|socket|fork` を取る（§6 の「埋め込まれた攻撃」の代わり）。
- `Makefile`: class `static`、`usr/libexec/keiland-preview`。libpdf・libtruetype・libz/png/jpeg/gif-compat・`picture.c` の source を一緒に compile。
- `platform/amd64/vmunix.mk`: static の program に package の外の object を足す口（`AMD64_USER_STATIC_EXTRA_<name>`）と、keiland-preview に libm と浮動小数点の
  解釈（`src/libc/math/*`・`softfloat.c`・`float-parse.c`、共有の libc と同じ `-mlong-double-64`。静的の libc に無い）。keiland-preview の object に `-DPDF_FONT_FILES=0`。
- `userland/base/libpdf/font.c`: `PDF_FONT_FILES`（既定 1）。0 の build は代わりの font の file を読まない（sandbox の中で open を呼ぶと子が SIGKILL になるため）。
  埋め込まれていない font の文字は描かない。

## 確認（2026-10-07）

- host（Linux）: `sh plan/ws168/tests/run-host-preview.sh` → PASS（25 の check）: PNG（透明は premultiplied で黒）256 contain → 256x192 と stamp の comment、
  JPEG の EXIF の向き 6 → 192x256 で赤が右、GIF の cover 240x150、小さな PPM は大きくしない、PGM、PDF（A4、埋め込まれない Helvetica を含む）→ 181x256 の色と真ん中の黒、
  形式不明 1・壊れた PNG 2・空 1・引数の誤り 64（試験 build でない `--test-escape` も）・pipe の入力 64、試験 build の open・socket・fork が seccomp で SIGSYS（出力なし）。
- zedBSD: `make ZEDBSD_CONFIG=plan/ws168/tests/config-amd64-preview.mk BUILD=build/ws168-zed build/ws168-zed/bin/keiland-preview` warning 0、静的 link の未定義 0（300 KiB）。
- style-check 0（preview の全部の C、`font.c`）。
- 未実施: zedBSD で sandbox_spawn から起こす確認（静的 libc の起動が集合の外の call を呼ばないか、子の出力）→ p004 の T1 の依頼にまとめる。FreeBSD。

## 積み残し

[WS177 backlog-p2](../../ws177/backlog-p2.md) の WS168 の行。
