<!-- awesome-plan project=zedbsd record=ws198-p001 -->

# ws198-p001: self-build の前提の調べと工数

Phase ID: `ws198-p001`
Parent: [WS198](../ws.md)
Status: cleared 候補（2026-10-09 深夜 P1。調べと表だけ、code は変えない。判定は Q1）
Phase disposition: normal
Queue: Q1 の投入（2026-10-09「WS198 p001（前提の調べと工数、code は変えない。make が GNU の機能に足りるか・Python・Noct の CMake・その他の道具・distfile の取得。足りない物ごとの工数の表を phase.md と Q1 へ）」）

## 調べた方法

- root の Makefile と、それが読む全部の `*.mk`・`Makefile`（405 file）の GNU make の関数・特別な target・記法の使い方を数えた。
- **zedBSD の base の make（`userland/base/make`）を Linux の host の cc でそのまま build し**（`build/p1-ws198/zmake`、warning を見ない試しの build。製品の code は変えていない）、同じ tree・同じ release の config（`config/release/config-amd64-beta2.mk`）で GNU make と並べて流した:
  - `list-user-programs`: 最初は失敗（下の M1）。scratch の写し（`build/p1-ws198/src`、M1・M2 だけを仮に直した物）では GNU make と 319 行が完全に一致（全 makefile の読み込みと変数の展開は通る）。
  - `-n disk-image`: GNU make の 4938 行（途中の `-C` の sub-make で dry-run の限界）に対し、scratch の make は 933 行で M3 で止まった。**その先は未確認**。
- build が呼ぶ Python の script の import（全部 stdlib: argparse・pathlib・struct・zlib・subprocess・tempfile・hashlib・concurrent.futures など）、外の命令（tar・sha256sum・cmake・perl など）と zedBSD の program の一覧（`make list-user-programs`）の突き合わせ。
- 証拠の出力: worktree の `build/p1-ws198/`（g.txt・z.txt・g-n.txt・z-n.txt・*.err、試しの make）。消すのは Q1。

## 1. 足りない物と工数（1 LW ≈ エージェントの実時間 20 分）

| # | 項目 | 今 | 足りない物 | 案 | LW |
| --- | --- | --- | --- | --- | --- |
| 1 | **make** | base の make は GNU の関数を全部持つ（Makefile の使う 30 種、`$(file)`・`$(abspath)`・`$(eval)` など）、`-j` と jobserver、`.DEFAULT_GOAL`・`.DELETE_ON_ERROR`・`MAKECMDGOALS`、`--no-print-directory` | 見つけた差: **M1** rule の行の `|`（order-only）と `;` を展開の前に探すので、`$(if X,| Y)` が壊れる（`userland/packages/lang/clang/Makefile:143`、「unterminated variable reference」）。**M2** UTF-8 の BOM の付いた makefile（`src/libc/softfloat.mk`）が「missing separator」。**M3** static pattern rule（`$(OBJS): dir/%.o: src/%.c`、7 箇所、`platform/amd64/vmunix.mk:871,947` など）が無い。**M4** grouped target `&:`（3 箇所、wallpapers）が無い。M3 の先（dry-run の 81%）は未確認。menuconfig の進捗の `--trace` は無い（self-build の必須ではない） | (a) base の make を直す（M1〜M4 と、host の試しの make で dry-run を最後まで通して出る残り、WS046 の差分試験に足す）/ (b) GNU make を package にする（GPL-3。gperf と同じ扱い） | (a) 5〜8（残りの未確認を含む）/ (b) 3 |
| 2 | **compiler と binutils** | zedBSD の clang の package（`userland/packages/lang/clang`）は clang・ld.lld・llvm-ar・ranlib・nm・objcopy・objdump・readelf・strip を `/usr/bin` に入れる（`clang/Makefile:220`） | root の Makefile は `ZEDBSD_TARGET_LLVM_BIN := $(abspath build/llvm/bin)` と固定（96 行）。toolchain の build と検証（`toolchain/llvm/llvm.mk`）を飛ばす切り替えが無い | self-build の切り替え（例 `ZEDBSD_SELF_BUILD=y`: LLVM の bin を `/usr/bin`、`toolchain` の LLVM の build と検証を飛ばす、HOSTCC=cc）。**toolchain の規則の変更なので Q1 の許可が要る** | 3 |
| 3 | **sysroot と compiler-rt** | sysroot は tree の header と libc から作る（`toolchain/llvm/sysroot.mk`）が、compiler-rt の builtins を LLVM の source（`build/llvm-source`、llvm-project の 23.1.0 の .tar.xz）から compile する（73〜80 行） | LLVM の source の取得と展開（約 140 MB の .tar.xz） | (a) 動いている zedBSD の `/usr/lib/libclang_rt.builtins.a`（開発の file を入れた image）を使う切り替え / (b) llvm-project の tarball から compiler-rt だけを展開 | (a) 1 / (b) 2（＋#5） |
| 4 | **Python** | build は Python 3 を使う（image の道具 `make-arch-overlay-image.py`・`make-ufs-root-image.py`・`check-*.py`、`libkeiland/exports.py`、壁紙の `generate.py`、hand-hershey の `convert.py`、menuconfig）。全部 stdlib | zedBSD の python3 は [WS126](../../ws126/ws.md)（beta2.md ではベータ3 の列）。拡張 module の import の失敗（T1-495）は ld.so の直し（main に merge 済み）で、T1-508 で確かめ中。`generate.py` は `ProcessPoolExecutor`（multiprocessing・`sem_open`）を使う | WS126 の p005 の完了を待ち、self-build の config に python3 を入れる。壁紙は multiprocessing が動かなければ逐次に落とす | 1（WS126 の残りは別） |
| 5 | **tar・圧縮・SHA-256** | base に pax・zcat・uncompress・compress、`libz-compat` に inflate。`tar`・`gzip`・`xz`・`bzip2`・`sha256sum` は無い | distfile の検証と展開（`userland/packages/tools/archive.sh` は `sha256sum` と `tar -tf/-tvf/-xf` で .tar.xz 44・.tar.gz 13・.tar.bz2 2 を自動の圧縮の判定で読む）、firmware の SHA-256、Noct の source（.tar.gz）、LLVM（.tar.xz） | (a) base に作る: `sha256sum`（小）、`tar`（pax の上の前の口、圧縮の自動判定）、gzip の展開（libz-compat の inflate）、xz（LZMA2）と bzip2 の展開器を新規に / (b) package: libarchive の bsdtar（BSD-2）＋ xz（0BSD）＋ bzip2、sha256sum だけ base | (a) 9〜12 / (b) 5〜6 |
| 6 | **Noct** | host の Noct（`build/NoctLang`）は CMake で build（`userland/base/noct/Makefile:364`）。`NOCT ?=` なので上書きできる。target の Noct の package（`/bin/noct`）も CMake で build（348〜355 行） | zedBSD の上に CMake が無い | `NOCT=/bin/noct`（self-build の image に noct を入れる。build の .noct の道具は process の API を使わない、使うのは smoke の試験だけ）。target の Noct を self-build で作り直すなら、Noct の source を CMake を使わずに build する make の規則（**toolchain の規則、Q1 の許可**）か #7 の CMake | 1（`/bin/noct` を使う）/ ＋2（make の規則） |
| 7 | **CMake・Ninja** | 使う所: Noct、外の package の zlib・curl・libcxx・clang・expat・libpng・libjpeg-turbo・libtiff・pcre2（Ninja の generator は expat・libpng・libjpeg-turbo・libtiff と toolchain の LLVM） | zedBSD に無い | CMake（BSD-3、C++、libcxx の上）と Ninja（Apache-2）を package にする | 8〜10 |
| 8 | **Perl** | OpenSSL の `Configure`（`userland/packages/security/openssl/Makefile:64`）、fontconfig・graphene・gperf の一部 | zedBSD に無い | Perl の package（大きい）/ OpenSSL・curl・openssh を self-build の対象から外す | 8＋ / 0（外す） |
| 9 | **clang・libcxx の package 自身** | release の config は clang・libcxx を入れる。clang の package は toolchain の build が作る native の tablegen（`llvm-native-tools`、`clang/Makefile:100,154`）を要る | zedBSD の上で LLVM の tablegen を作る（CMake で LLVM の一部）、LLVM 全体の compile（guest で数時間以上） | 最後の段（任意）。最初は self-build の対象から外す | 6（＋長い実行） |
| 10 | **distfile の取得** | curl の package（TLS は openssl・ca-certificates）。`make download` は全部の source を固める用で build 用ではない（memory） | 取得の経路 | (a) 動いている zedBSD の curl で package ごとに取得（今の規則のまま）/ (b) host で取った `build/distfiles` を写す | 1 |
| 11 | **disk と tree** | root は 1024 MiB（`platform/amd64/vmunix.mk:2341` `AMD64_NATIVE_ROOT_MIB`）。host の `build/amd64` だけで 1.6 GB。zedBSD に git は無い | 大きい作業の場所、tree の持ち込み | self-build 用の config（root を 16 GiB 以上、または data の disk を mount）、tree は host の `git archive` の tar を持ち込む（VERSION の `+g<sha>` は git が無いと `+unknown`、`Makefile` の注記どおり） | 2 |
| 12 | **sh と道具の互換** | recipe は `/bin/sh`（host では dash）。zedBSD の sh・awk・sed・find・install・mktemp・patch などは base にある | dash と zedBSD の sh の差、各道具の option の差は未確認 | M3 の先の dry-run と実の build で出る物を直す（予備） | 2〜4 |
| 13 | **試験** | — | self-build は QEMU の guest で数時間（clang を除いても kernel の LTO full の link、全 userland、image）。T1 の 1 回が長い | T1 の夜間の枠（ユーザーの確認、protocol の負荷試験と同じ扱い）。CPU 数・memory を多めの guest | 2（依頼と結果の判定） |

## 2. 段の案と合計

| 段 | 内容 | 要る項目 | LW |
| --- | --- | --- | --- |
| **S0** kernel だけ | zedBSD の上で `make … vmunix`（kernel は freestanding で sysroot を要らない。ただし root の Makefile は全部の makefile を読むので make の差は全部要る） | 1(a)・2・11・12 の一部・13 | 約 12〜15 |
| **S1** base と desktop の image（外の package なし） | release の config から外の package（clang・libcxx・libavcodec・openssl・curl・openssh・ca-certificates・zlib・fonts の package）を外した self-build の config で `disk-image`。Noct は `/bin/noct`、compiler-rt は system の物 | S0 ＋ 3(a)・4・5・6・10 の一部 | S0 ＋ 約 15〜18（計 約 27〜33） |
| **S2** 外の package（clang を除く） | CMake・Ninja・Perl（か OpenSSL 系を外す）で zlib・curl・libavcodec・openssl など | S1 ＋ 7・8 | ＋ 約 16〜20 |
| **S3** release の config の全部 | clang・libcxx の package も zedBSD の上で | S2 ＋ 9 | ＋ 約 6（＋数時間の実行） |

- 1(b)・5(b)（GNU make、libarchive）を選べば S1 は約 6〜8 LW 減る（外の実装に頼る代わり）。
- 前提: WS126（python3）の完了（beta2.md ではベータ3）。S1 以降は python3 が要る。

## 3. ベータ2 に入るか（Q1 とユーザーへの材料）

- beta2.md の必須の残りは 37 LW で、10/13 の凍結・10/16 の準備に対し P1 は T1・UAT の待ちと Bug の直しが続く。
- **S1（ユーザーの言う「self-build」に当たる最小の形）は約 27〜33 LW**（1(b)・5(b) で約 21〜25）に、WS126 の python3 の完了が前提で、T1 の夜間の実行（数時間）が要る。**ベータ2（10/16）には入らない見込み**。
- ベータ2 に何かを入れるなら **S0（kernel だけ、約 12〜15 LW）** だが、make の差（M3 の先が未確認）次第で増える。
- 推し: ベータ3 で S1 まで（1(a)・5(a) の base の再実装の方針どおり。外の実装に頼るなら 1(b)・5(b) を選ぶのはユーザーの判断）。S2・S3 は後。

## 4. ユーザーに尋ねる判断（推しは太字）

| # | 判断 | 選択肢 | 推し |
| --- | --- | --- | --- |
| D1 | 時期 | **ベータ3 で S1 まで** / ベータ2 に S0（kernel だけ）/ ベータ2 に S1（他の必須を押す） | **ベータ3 で S1** |
| D2 | make | **base の make を直す（M1〜M4 と残り）** / GNU make を package に（GPL-3） | **base を直す** |
| D3 | tar・圧縮 | **base に作る（sha256sum・tar・gzip・xz・bzip2 の展開）** / libarchive・xz・bzip2 の package | **base に作る**（使い回せる。工数は +4〜6） |
| D4 | 範囲 | **S1（外の package なし）を完了の条件に、S2・S3 は別の WS** / S3 まで | **S1** |
| D5 | toolchain の規則の変更（self-build の切り替え、Noct の make の規則） | **Q1 の許可で WS198 が行う** / 別の担当 | **Q1 の許可で WS198** |

## 5. 次の Phase の案（D1〜D5 の後に Q1 が Queue に）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | base の make の差を直す（host の試しの make で `-n disk-image` を最後まで GNU make と一致させる、WS046 の差分試験に足す） | D2 |
| p003 | self-build の切り替え（LLVM の bin、toolchain の build と検証を飛ばす、compiler-rt・Noct の場所）と self-build の config（disk の大きさ、外の package を外す） | D5 |
| p004 | sha256sum・tar・gzip・xz・bzip2（D3） | D3 |
| p005 | S0 の T1（kernel）→ S1 の T1（image、夜間） | p002〜p004、WS126 |
| p006 | 規約の全文の見直し | p002〜p005 |

## 確かめ

- code・Makefile・plan の他の WS は変えていない。試しの make は `build/p1-ws198/` だけ（製品に入らない）。
- 未実施: zedBSD の guest の上での実の build（QEMU は T1、この Phase の範囲の外）。M3 の先の make の差、sh の差、guest の build の時間。
