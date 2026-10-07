<!-- awesome-plan project=zedbsd record=ws168-p004 -->
# ws168-p004: Files・Settings を keiland-preview の子に切り替える

Phase ID: `ws168-p004`
Parent: [WS168](../ws.md)
Status: cleared（2026-10-08 Q1 判定、T1-395）（旧: in-progress（2026-10-07 q834 P2: 実装、host（Linux）の試験 PASS、zedBSD・Linux の build warning 0。QEMU は T1 に依頼、判定は Q1））
Queue: q834（2026-10-07、P2）
設計: [p001](../phase001/phase.md) §5・§7

## 範囲（Q1 の ACK 2026-10-07）

正常系。Q1 の変更: FreeBSD は Capsicum の口ができるまで in-process の復号のまま（復号の library の link を残す）、backlog に「FreeBSD の Capsicum の子へ移す」。

## 実装（2026-10-07 P2）

- `userland/desktop/preview/`:
  - `make.c`（`preview_make`: 入力の fd を読み、復号・縮小・P6 を出力の fd へ。main.c から分けた）。
  - `client.h`・`client.c`（呼び出し側: 入力を `O_NONBLOCK` で開き通常の file だけ、`preview_start`・`preview_poll`（時間切れ 5 秒・PDF 10 秒で SIGKILL）・`preview_wait`・
    `preview_picture`（`$XDG_RUNTIME_DIR` の一時 file に作らせて読む）・`preview_read`（子の P6 を信用せず、一辺 4096 まで・画素の数を確かめる））。
  - `zedbsd/spawn.c`（`sandbox_spawn`: image の fd、fd {入力→0, 出力→1}、argv、memory 1 GiB・CPU 10 秒・書く大きさ 48 MiB）、
    `linux/spawn.c`（fork → dup2・`close_range`・rlimit → 空の環境で execve。子は seccomp）、`freebsd/spawn.c`（`preview_make` をこの process で）。
  - `linux/confine.c`: open（openat）は fatal でなく EACCES に（共有の libpdf が代わりの font の file を探すため。他の call は今どおり KILL）。
  - `Makefile.linux`（keiland-preview を libexec に）、`userland/desktop/keiland-linux.mk` に include。
- Files（`thumb.c` を書き直し、`thumb-cache.c`・`ui.c`・`files.h`）: 縮小画像は子が cache の記録（stamp `keiland-thumbnail mtime= size=`）を記録の隣に書き、成功で rename して読む。
  1 つずつ、main loop が毎回 poll（その間 `fm_ui_wait` は 20 ms）。preview・Quick Look・Today の絵は `preview_picture` で待って読む。Files の中の PNG・JPEG・GIF の復号と
  libpdf の dlopen（`fm_thumb_pdf`）、`fm_thumb_cache_write` を除いた。zedBSD・Linux の Files は復号の library を link しない（package は `desktop/preview` を要る）。
- Settings（`look.c`）: 背景の tile を `preview_picture`（240x150 cover）で。`wallpaper.c` と復号の library を zedBSD・Linux の build から外した。
- FreeBSD の Files・Settings: preview の make・decode・scale・picture.c と libpdf・復号の library を link し、`freebsd/spawn.c` で in-process（今までどおり絵が出る）。
- `platform/amd64/vmunix.mk`: files・settings の link から復号の library を外した。

## 試験の追従

- `plan/tools/files/host-build.sh`: Files の host の build に preview の client・freebsd/spawn.c（in-process）・make・decode・scale と libpdf を足した（object の名前は directory 付き）。
- `plan/tools/files/host-model.c`: `fm_thumb_is_pdf` の確かめを除いた（関数が無くなった）。大きすぎる PPM は EFBIG、300x200 の縮小は 256x171（keiland-preview の丸め）。
- `plan/ws168/tests/host-preview.py`: open の escape は EACCES で拒まれ status 70。
- `plan/ws168/tests/host-client.c`（新）: `run-host-preview.sh` の中で client を子（linux/spawn.c）と in-process（freebsd/spawn.c）の両方で。

## 確認（2026-10-07）

- `sh plan/ws168/tests/run-host-preview.sh` → host-preview PASS、host-client PASS ×2（子: picture 256x192、job の poll、stamp、/dev/null と text は EINVAL。in-process も同じ）。
- `sh plan/tools/files/host-model.sh` → files-model PASS。
- zedBSD: `make ZEDBSD_CONFIG=plan/ws168/tests/config-amd64-preview.mk BUILD=build/ws168-zed build/ws168-zed/bin/files build/ws168-zed/bin/settings build/ws168-zed/bin/keiland-preview` warning 0。
- Linux: `make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws168-linux …/bin/files …/bin/settings …/libexec/keiland-preview` warning 0。
- FreeBSD: build は未実施（環境なし）。
- style-check 0（preview の全部、files の thumb.c・thumb-cache.c・ui.c、settings の look.c）。
- AAT: `plan/tools/aat/scenarios/helpers_preview.py` と `tests/scenarios/apps/files/thumbnails.md`（新）。QEMU: 未実施（T1 に依頼）。

## 積み残し

[WS177 backlog-p2](../../ws177/backlog-p2.md) の WS168 の行。

## q875（P1、2026-10-08）: T1-319 の thumbnails の fail（helper の座標）の直し

- 原因（T1-319 の log `t1/build/t1-318/logs/apps.files.thumbnails.log` で確かめた）: helper の `sized()` が窓の大きさを `ZFILES READY width= height=` から取っていた。desktop も Files なので、mark の後に desktop の READY（`width=1280 height=756`、bar の下の画面全体）が先に合い、窓（client 16、実際は 1120x680、`KWL MAP ... x=80`）の close の button を 80+1280−26=1334 に探して画面の外になった。
- 直し（`plan/tools/aat/scenarios/helpers_preview.py`）: 大きさをその client の buffer の import（`KWL IMPORT client=N buffer=… width= height=`）から取る。close の button は 80+1120−26=1174 で画面の中。
- 確認: `python3 -m py_compile`。QEMU（T1）: AAT `--only 'apps\.files\.thumbnails'` の再試験（2 度目の cache・Settings の背景の tile・`SANDBOX deny` が増えないことを含む）は未実施。


## Q1 の判定（2026-10-08）

T1-395: apps.files.thumbnails は fail でない（sample.png・jpg・pdf が status=0、broken.png は拒否、cached=1、Wallpaper 7 of 7、SANDBOX deny 0）。PNG を Q1 が目視（build/review/t1-39x/apps.files.thumbnails-files.png、3 枚の縮小画像と broken.png の汎用の icon）。
