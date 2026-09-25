# ws034-p001 結果: 版・入手元・依存グラフの確定

Queue q316 / 項目 q316-i02。実行 2026-09-23（phase-runner、Opus 5.5）。
成果物の正本は [package-inventory.md](../package-inventory.md)。ここには実行した内容と受け入れ条件の達成状況を書く。

## 1. 実行したコマンド

| # | コマンド | 結果 |
| --- | --- | --- |
| 1 | upstreamの配布ディレクトリ・GitHub API（`gh api repos/…/releases`）・GNOMEの `cache.json`・freedesktop GitLabのrelease API・Qtのmirror一覧・Rustの `channel-rust-stable.toml` を `curl --max-time 60` で読み、各packageの最新版を調べた | 版は package-inventory.md §2 |
| 2 | `plan/ws034/phase001/fetch-distfiles.sh /home/awe/zedBSD-rpi4`（`distfiles.tsv` の全行、1件 `--max-time 600`、全体 `timeout 3600`） | exit 0、93ファイル取得、失敗0（`fetch.log`） |
| 3 | 同スクリプトで追加取得（ffmpeg 8.1.3、libtiff、gperf、libiconv、ncurses）→ `fetch2.log`、atk 2.38.0 → `fetch3.log` | どちらもexit 0、失敗0 |
| 4 | bash公式パッチ20件と `.sig` を `build/distfiles/bash-5.3-patches/` へ `curl --max-time 120` | 40ファイル、失敗0 |
| 5 | 鍵: `https://ftp.gnu.org/gnu/gnu-keyring.gpg`、`https://daniel.haxx.se/mykey.asc`、`https://ffmpeg.org/ffmpeg-devel.asc` を使い捨ての `GNUPGHOME=/tmp/ws034p001/gnupg` にimport。残りは署名の発行者fingerprintでkeys.openpgp.org／keyserver.ubuntu.comから取得 | VLCの鍵（`A341FD76…835B911E`）だけ入手できず |
| 6 | `python3 plan/ws034/phase001/measure-distfiles.py /home/awe/zedBSD-rpi4 /tmp/ws034p001/gnupg plan/ws034/phase001/distfiles-measured.json`（`timeout 3000`。最終実行は3回目） | rc 0。tarball 53件すべて `archive.sh verify` ok、署名・公開digestの照合結果は `measure.log` と inventory §1.2 |
| 7 | bashパッチ: 20件を `gpg --verify`、展開した `bash-5.3` に `patch -p0` で順に適用、`patch -p1 --dry-run` で `bash53-001` を試す | 署名20件good、`-p0` 全件適用・`PATCHLEVEL 20`、`-p1` は "No file to patch" |
| 8 | 公開digestの無いものの独立照合: `gzip -dc … \| git get-tar-commit-id` と `git ls-remote`（vim、ninja、libxkbcommon）、isl 0.24を libisl.sourceforge.io から再取得してSHA-256比較、libpngのMD5をSourceForge RSSと比較し、展開内容をgit tag archiveと `diff -rq` | すべて一致（libpngの差分0件） |
| 9 | 展開（`/tmp/ws034p001/src`、repository外）して `configure`・`meson.build`・`CMakeLists.txt`・`Makefile`・`NEWS` 等を `grep` で調査。`config.sub x86_64-unknown-zedbsd` を21個に対して実行 | 21個すべて "OS 'zedbsd' not recognized" |
| 10 | meson 1.12.0（distfileから展開）で `system='zedbsd'` のcross fileを使った小projectのsetup・build（compilerはhostのcc、`timeout 120`） | 警告なしで受理、`host_machine.system()`＝`zedbsd`、実行ファイルと共有ライブラリ（SONAME `libu.so`）を生成 |
| 11 | `git -C /home/awe/zedBSD-rpi4 status --short` | 変更は `plan/ws034/` 以下の未追跡ファイルだけ |

同じ条件で変更なしのretryはしていない（measureスクリプトは、UnicodeDecodeErrorの修正後と、追加取得後に再実行した）。
aggregate `make check`、package Makefileの作成、ソース変更、HAL・UAPIへの変更はしていない。

## 2. 成果物

| パス | 内容 |
| --- | --- |
| `plan/ws034/package-inventory.md` | 正本。版と理由、入手元、検証、実測size・SHA-256・ROOT、ライセンス、build系、既知のクロスbuild問題、置き場所、横断的問題、追加依存、依存グラフ（表と図）、Phase表の修正案、CA bundle、meson案、Rust・Chromium参考、人間の判断が要る点 |
| `plan/ws034/phase001/distfiles.tsv` | 取得一覧（key、版、ファイル、URL、検証手段、検証URL） |
| `plan/ws034/phase001/fetch-distfiles.sh` | 取得スクリプト |
| `plan/ws034/phase001/measure-distfiles.py` | 実測・照合スクリプト |
| `plan/ws034/phase001/distfiles-measured.json` | 実測値と照合結果（署名の40桁fingerprintを含む）。`key_source` は最終実行時の値で、keyserverから取った鍵も2回目以降は `local-keyring` と出る（鍵の出所はinventory §1.3の表が正） |
| `plan/ws034/phase001/fetch.log`、`fetch2.log`、`fetch3.log`、`measure.log` | 実行ログ |
| `build/distfiles/`（git管理外） | tarball 53件、`cacert-2026-08-13.pem`、各 `.sig`/`.asc`/`.sha256*`/`.dsc`/`.gh-digest`、`bash-5.3-patches/`。合計 559,867,050 byte |

## 3. 受け入れ条件の達成状況

| 受け入れ条件 | 状況 |
| --- | --- |
| `package-inventory.md` に対象全件の版・URL・SHA-256・size・ライセンス・build系・置き場所がある。取得できなかったものは理由を書く | **達成**。phase.mdの対象全件（bash〜VLC）を取得・実測した。Rustは版だけ（1.98.1）、Chromiumの依存は名前と入手元だけ（指示どおり取得していない）。取得できなかったものは無い。FFmpeg 9.0.2は取得後に不採用として削除し、理由を書いた（§1.4、§2.9）。ライセンスSPDXはファイルと `license:` 欄で確かめたが、fontconfigのSPDX名はp027での機械確認に残した |
| 依存グラフがあり、Phase表との食い違いが列挙されている | **達成**。§5（表: build時のターゲット用・host道具、実行時。図）、§6（修正案 20行。依存の追加はp020・p013・p026・p027・p034・p030・p032、範囲の追記はp009・p010・p008・p011・p012・p025・p028・p032・p019・p017） |
| CA bundleとmesonの案がある | **達成**。§7（curl配布のPEM、`/etc/ssl/cert.pem`、WS032のOpenSSLの `--openssldir=/etc/ssl` との対応、更新方法、`external.mk` との不整合と2案）、§8（cross file・native file・pkg-config wrapperの生成案、meson 1.12.0をdistfileから使う理由、置き場所案、host ccでの試行結果） |
| ソースに差分が無い（`git status` で `plan/` 以外に変更が無い） | **達成**。`git status --short` は `plan/ws034/package-inventory.md` と `plan/ws034/phase001/*` の未追跡ファイルだけ |

## 4. 未実施の確認

- **zedBSDのクロスclangを使ったmesonのsetup**: このcheckoutに `build/llvm` と `build/<arch>/sysroot` が無いため、
  実際のwrapperでは試していない（hostのccで代用）。p025で行う。
- **各packageのクロスbuild**: p001の範囲外。§2の「既知のクロスbuild問題」はsourceを読んで確かめたもので、
  build結果ではない。
- **keyserverから取った鍵の真正性**: zlib、expat、pcre2、libjpeg-turbo、wayland、wayland-protocols、libtiff、
  git（kernel.org autosigner）、meson、freetype、xkeyboard-config、Debianの鍵は、署名の発行者fingerprintで
  keyserverから引いた。配布元が公開するfingerprintとの独立照合はしていない。
- **VLCの署名**: 鍵を入手できず未検証（公開SHA-256は一致）。
- **ライセンスの機械監査**（WS032の `audit-licenses.sh` 相当の全文検索）: 各packageのPhaseで行う。
- **curlのPEMとDebianの `certdata.txt` の突き合わせ**: p019で行う。

## 5. 残課題・人間の判断が要る点

inventory §10 に8項目をまとめた。要点:

1. **libcにiconvが無い**（glibが必須）。libcへの実装（p005）かGNU libiconvのpackageか。
2. **termcap API（`tgetent`・`tputs` 等）が無い**（vim・emacsが必須、gdbが使う）。baseのcurses拡張（p005）かncursesか。
3. **本家libwaylandがepoll・timerfd・signalfdを必須にしている**。zedBSDには無い。カーネル・libcへの追加、
   libwaylandへのパッチ、epoll-shim相当のどれにするか。p034とGTK・Qt全体の前提になる。
4. **git 2.55はRustを既定で有効**にし、Git 3.0でRust必須を予告している。当面 `NO_RUST=1` でよいか。
5. CA bundleの配布形（curlのPEM＝既定案、Debianのtarball）。
6. config.sub（21個すべてzedbsdを拒否）とlibtool（未知のOSで共有ライブラリを作らない）の共通対応。
7. atkのsource（at-spi2-coreは `atk_only` でもlibxml2が必須。単体atk 2.38.0なら不要）。
8. ws.mdの版の目安（emacs 30.x → 31.1、vim 9.1 → 9.2）。

計画に無かった依存（inventory §4）: **libtiff**（GTK4が必須）、**gperf**（fontconfigのhost道具）、**Qt5 qtsvg**
（VLCのQt GUIが必須）、iconv、termcap API、epoll系、同じ版のhost道具（wayland-scanner 1.26.0、Qt 6.11.2、
emacs 31.1、meson 1.12.0）、libxml2（at-spi2-core）。任意でoptionで外せるもの: libpsl（curl）、Lua（VLC）、
libxml2（libxkbcommon・waylandのDTD検証）、dbus。

そのほかの横断的な問題（inventory §3）: symbol versioningの無効化が要るpackageが12件、版付きSONAMEとsymlinkの
image投入（§3.4）、pkg-config wrapperとpackage prefix（§3.5）、mesonのwrapによるネットワーク取得の禁止（§3.9）。

## 6. 作業ディレクトリ

`/tmp/ws034p001/`（repository外）に使い捨てのkeyringと展開したsourceを置いた。展開物は作業後に削除し、
keyring（`gnupg/`）と鍵ファイルだけを残した（`measure-distfiles.py` を再実行するときに使える）。
