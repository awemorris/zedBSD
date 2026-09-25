<!-- awesome-plan project=zedbsd record=ws034 -->

# WS034: アプリケーション拡充とカーネル・libcの是正

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG002
Related Milestones: MG001, MG007
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし
Resume point: Phase 表の planning の項目（package の導入と libc/kernel の是正）
<!-- awesome-plan-current:end -->

## 単一目標

ユーザーがzedBSD上で使いたいアプリケーション一式（下表）をzedBSDへ追加し、
ターゲット上で実際に動かす。その過程で見つかったカーネル・libcの設計/実装ミスと
機能不足を、場当たりの回避策ではなく原因側で修正する。アプリケーションは
libc・カーネルの動作確認の役割も兼ねる。

到達点: 下表の全アプリケーションが選択したdisk imageに入り、amd64ターゲット上で
代表操作が通る。発見したlibc/カーネル不足は修正済み、または証拠付きでBug/Future
Workへ移されている。

2026-09-23 ユーザー指示「アプリケーション拡充を目的とした wsを作成します。このWSは、
私がこのOSで利用したいアプリケーションを追加しながら、カーネルやlibcの設計・実装ミスや
機能不足を修正していくものです。」

| アプリ | 置き場所（2026-09-23ユーザー指定） | 形態 | 備考 |
| --- | --- | --- | --- |
| which | `userland/base/which/` | 独自実装（base、Zlib） | |
| lspci | `userland/base/lspci/` | 独自実装（base） | `/dev/system` ioctl（決定済み） |
| lsusb | `userland/base/lsusb/` | 独自実装（base） | 同上 |
| bash | `userland/packages/shell/bash/` | 外部tarball＋パッチ | |
| GNU coreutils | `userland/packages/utils/coreutils/` | 外部tarball＋パッチ | `/usr/bin`へ入れ、PATHで`/bin`のbaseより優先 |
| vim | `userland/packages/editors/vim/` | 外部tarball＋パッチ | |
| emacs | `userland/packages/editors/emacs/` | 外部tarball＋パッチ | libc・カーネルの動作確認を兼ねる |
| git | `userland/packages/development/git/` | 外部tarball＋パッチ | 依存ライブラリも再帰的に追加 |
| gcc 16 | `userland/packages/lang/gcc-16/` | 外部tarball＋パッチ | gcc/g++/gfortran。binutils・GMP・MPFR・MPC等も再帰的に追加 |
| curl | `userland/packages/network/curl/` | 外部tarball＋パッチ | 2026-09-23追加。libcurlはgitも使う |
| wget | `userland/packages/network/wget/` | 外部tarball＋パッチ | 2026-09-23追加（GNU Wget、GPL） |
| VLC | `userland/packages/multimedia/vlc/` | 外部tarball＋パッチ | 2026-09-23追加。音声・映像の出力はWS035に依存 |
| ca-certificates | `userland/packages/security/ca-certificates/` | 外部配布物（CA bundle） | 2026-09-23追加。curl・wgetの前提 |
| GTK4 | `userland/packages/desktop/gtk4/` | 外部tarball＋パッチ | 2026-09-23追加。分類 `desktop/` はユーザー指定 |
| Qt6 | `userland/packages/desktop/qt6/` | 外部tarball＋パッチ | 同上 |
| GTK3 | `userland/packages/desktop/gtk3/` | 外部tarball＋パッチ | 同上 |
| Qt5 | `userland/packages/desktop/qt5/` | 外部tarball＋パッチ | 同上。VLC 3.0のGUIが使う |
| libwayland（本家） | `userland/packages/desktop/libwayland/` | 外部tarball＋パッチ | 2026-09-23追加（ユーザー指定）。`/usr/lib` に入る。GTK・Qtが使う |
| gdb | `userland/packages/development/gdb/` | 外部tarball＋パッチ | |
| Rust | `userland/packages/lang/rust/` | 外部tarball＋パッチ | |

配置は2026-09-23のユーザー指定（分類あり）。表にない依存パッケージの置き場所はp001で決める。
エージェントの既定案は次のとおり。binutilsは`development/`、
zlib・expat・GMP・MPFR・MPC・ISLは新しい`libs/`。既存の`devel/libcxx`（WS032）は移動しない。

WSは1つの目標を持つというルールがある。ここでは上表の一式を1つの目標とする。
WSが完了する前にユーザーが対象アプリを追加した場合は、この目標の範囲変更として
Phaseを追加する。完了後の追加は新しいWSで扱う。

## ライセンス境界

[設計方針 §2.1](../master-design-policy.md) により、packagesとして配布するソフトウェアは
GNUを含む第三者ライセンスのままビルド・提供してよい。base systemの方針は変わらない。
したがって bash / coreutils / emacs / gcc / binutils / gdb / git / wget（GPL）とVLC・FFmpeg（LGPL主体）は
`userland/packages/` に置き、ソースツリーへは取り込まない（取得したtarballと、zedBSD側の
パッチ・Makefileだけを置く）。WS032の停止条件「GPL系ライセンスの混入」はWS032の
3パッケージに限った条件であり、このWSには適用しない。ライセンス表示とprovenanceは
WS032と同じ形式（`/usr/share/licenses/<pkg>/`、版・SHA-256・パッチ一覧）で残す。

一方、which / lspci / lsusb はbaseに置くため、独自実装（Zlib）とする。GNU等のコードは
参照・転記しない。

## 実行開始条件（2026-09-23ユーザー指示）

このWSの実行は、別エージェントが作業中のWS031とWS032が完了するまで始めない。
計画はそれ以前に進めてよい。両WSの完了後は、WS035のrefactor（p002–p004）を最優先で行い、
その後にこのWSを実行する。

2026-09-23 ユーザー連絡: WS031のGPU作業は一区切りとして停止した（未完了、残課題Phaseは
`plan/ws031/ws.md`）。残る待ちはWS032（q315 active）である。

2026-09-23: WS032はcompleted（q315の10件すべてcleared）。upstreamをマージし、待ちは解消した。
以後はこのエージェントが唯一の実行者である（Master「実行体制とQueue運用方針」）。

## 前提・依存

- WS032（q315 active、別エージェント）の成果に依存する。
  - p002 `userland/packages/external.mk`（取得・SHA-256・展開・パッチ）: cleared
  - p003 クロスビルド契約（wrapper、autoconf cross cache、CMake toolchain file、動的リンク）: cleared
  - p006 OpenSSL: cleared（gitのHTTPS経路で使う）
  - p004 libc不足補完: in-progress。p005 C++ランタイム、p008 clang: planning
- `external.mk` のtripleは現状amd64/i386だけである。このWSはamd64を対象とする。
  arm64（RPi4）などへの拡張は範囲外とし、必要ならユーザーが決める。
- 受入環境は**QEMU amd64のみ**（2026-09-23ユーザー決定）。実機での受入は行わない。
  GUIアプリはhost側をLavapipeにしたVenus（WS014のvirtio-gpu経路）で確認する。
- ptraceはWS032の作業でlldbが動く水準にある（commit `cde8e875`）。gdbは独自の
  native targetの移植が要る。
- `/dev/system` には `KERN_SYSTEM_GET_DEVICE` 等がある。PCI/USBの列挙ioctlはまだ無い。
  カーネル内部には `drv_pci_foreach_device()` と `drv_usb_foreach_device()` がある。

## 制約（Guardrail）

- HAL（`include/hal/hal.h`、`src/hal/`）の変更は、具体的な差分ごとにユーザーの事前承認を得る。
  UAPI（`include/uapi/`）の追加は該当Phaseの設計として提示し、合意してから実装する。
- libc/カーネルの修正は、アプリ側の回避パッチで済ませず原因側で直す（WSの目的）。
  ただし、別エージェントが担当するWS032/WS031の領域（`src/drivers/gpu/i915/`、
  `plan/ws031/`、WS032のlibc作業中ファイル）と衝突する場合は調整してから進める。
- aggregate `make check` は使わない。Phaseごとに有限の確認を行う。git add/commit/pushはユーザーが行う。
- 全文規約 [coding-style.md](../coding-style.md) を、base実装とカーネル/libc修正に適用する。
  外部パッケージへのパッチはupstreamの書式に合わせる。

## Phase一覧

近い順に具体化し、遠い項目は目的と主要リスクだけを書く。見積はQueue作成時に確定する。
2026-09-23に、1 Queueのスロットで終わる大きさへ分解した（分割した元のPhaseのIDは、範囲を縮めて残す）。
並びは優先順位（Master「Upcoming Work Outlook」）の順である。

| Combined ID | Phase | Status | 依存 | 主なファイル範囲 |
| --- | --- | --- | --- | --- |
| [ws034-p001](phase001/phase.md) | 設計固め: 版・入手元・SHA-256、依存パッケージの配置、依存グラフ、meson等のbuild系、CA bundleの入手元 | cleared（q316-i02） | — | 文書 |
| ws034-p005 | libc・カーネル是正の受け皿（各パッケージで見つかった不足を集約して修正）。p001で判明した不足: iconv（glib・wget）、termcap API（`tgetent`・`tputs` 等、vim・emacs・gdb）、epoll・timerfd・signalfd（本家libwayland）。2026-09-23決定: iconvはlibcに実装する（ASCIIとUTF-8だけ）→ **ws034-p040 へ切り出し、cleared**。termcap API（`tgetent`・`tgetstr`・`tgoto`・`tputs` 等）はbaseのcursesに足し、ncursesは使わない → **ws034-p041 へ切り出し、cleared** | planning | WS035のrefactor | libc、kern（随時） |
| [ws034-p040](phase040/phase.md) | libc の iconv（UTF-8 と ASCII、`//TRANSLIT`・`//IGNORE`）。p005 から切り出し | cleared（q331-i01） | WS035のrefactor | `src/libc/iconv.c`、`include/libc/iconv.h` |
| [ws034-p041](phase041/phase.md) | curses の termcap API（`tgetent`・`tgetstr`・`tgoto`・`tputs`・`tparm`）。`/lib/libcurses.a` を PIC に。p005 から切り出し | cleared（q333-i01） | WS035のrefactor | `userland/base/curses` |
| [ws034-p042](phase042/phase.md) | kernel の TCP: loopback で MSS（1024）を超える write が届かない。p017 の試験で判明、p005 から切り出し | cleared（q335-i01） | — | `src/kern/net/tcp.c` |
| [ws034-p043](phase043/phase.md) | VM: fork した子の blocking write（pin した page）が親の copy-on-write fault を止める。p042 で判明 | cleared（q337-i01） | — | `src/kern/syscall.c`、VM |
| [ws034-p044](phase044/phase.md) | TCP の throughput（1 MiB が 20 秒で終わらない）。p042 の後続 | cleared（q338-i01。改善。重複再送・512 byte の segment は今後の課題） | p042 | `src/kern/net/tcp.c` |
| [ws034-p045](phase045/phase.md) | fork した子の program 名（`ps` に `kernel` と出た） | cleared（q339-i01） | — | `src/kern/process.c` |
| [ws034-p046](phase046/phase.md) | TCP の MSS option と window update（1 MiB が 5 秒ごとに止まる。ws035-p039 で発見） | cleared（q344-i01。close の FIN、RST も直した） | p044 | `src/kern/net/tcp.c` |
| [ws034-p047](phase047/phase.md) | shell の job 表と `kill %N`（`kill` を builtin にし、`%N`・`%%`・`%-` を解決する。2026-09-24 ユーザー指示） | cleared（q352-i01。job 表、kill・jobs・fg・bg・wait、prompt での報告） | — | `userland/base/sh` |
| [ws034-p048](phase048/phase.md) | `FD_SETSIZE` を 1024 へ（他の POSIX と同じ値。今は 32 で `select()` が fd 32 以上を扱えない。2026-09-24 ユーザー決定） | cleared（q351-i01。kernel は nfds までの語だけを読み書き） | — | `include/libc/sys/select.h`、kernel の select |
| [ws034-p051](phase051/phase.md) | 1 process が開ける descriptor の数（`KERN_OPEN_MAX` = 32）を増やす。表を必要に応じて伸ばし、`RLIMIT_NOFILE`・`sysconf(_SC_OPEN_MAX)`・`getdtablesize()` と合わせる（p048 で発見） | cleared（q367-i01。1024、32 slot から倍に伸びる表、stack に大きな配列を置かない。guest 20/20） | p048 | `include/kern/filedesc.h`、`src/kern/filedesc.c`、poll |
| [ws034-p052](phase052/phase.md) | root の UFS image（`build/arch-images/amd64.ufs`、pcat と pc98 が共有する `i386.ufs`）を BUILD ごとに置く。今は全 BUILD が共有し、別の config の BUILD が書いた image を古い tree の BUILD がそのまま使う（ws041-p002 で、測定の image の root が SSH ハーネスの image に入り sshd が消えた）。`ZEDBSD_TEST_IMAGE_TAG` の回避策も不要になる | cleared（q360-i01。`$(BUILD)/arch-images` へ） | p036 | Makefile、`platform/*/vmunix.mk` |
| [ws034-p049](phase049/phase.md) | 動かない host の試験を**削除する**（書き直さない。2026-09-24 ユーザー指示）: `plan/ws025/tests` の VM、`plan/ws004/tests` の xHCI・zero-packet・ECM・NCM など、今の source で build・実行できないもの | cleared（q353-i01。host の runner 307 本を走らせ、185 本と fixture の計 430 file を削除。残る 112 本は通る。QEMU 等の 82 本は未確認） | — | `plan/*/tests` |
| [ws034-p050](phase050/phase.md) | 調査: Wayland（独自 libwayland、zdesktop、GTK4・Qt6 の Wayland backend）を POSIX（`poll`）の範囲で作れるか。epoll・timerfd・signalfd が要る箇所と代わりの方法。難しければ実装の相談を用意する（2026-09-24 ユーザー指示） | cleared（q362-i01。**POSIX の `poll` で足りる**。epoll・timerfd・signalfd を使うのは upstream wayland の server だけ。libc の `mkostemp`・`posix_fallocate` を p053 で） | — | 文書 |
| [ws034-p053](phase053/phase.md) | libc: `mkostemp`・`posix_fallocate`（p050 で発見。upstream wayland-cursor の代替の経路などが使う） | cleared（q363-i01。pipe・socket の `fstat` が EINVAL だった kernel の不足も直した） | p050 | libc |
| [ws034-p054](phase054/phase.md) | pipe・device の I/O が 512 byte ずつ（`dd if=/dev/zero bs=1M count=10` が 640 KiB 以下しか作らない）、pipe の容量 4 KiB、libc の `read`/`write`/stdio の余分な system call（ws036-p012 で発見） | cleared（q368-i01。stream の bounce buffer を heap に最大 64 KiB、待たない device は読み続ける、pipe 16 KiB、thread 1 つの間は TCB を聞かない、cancel が無ければ testcancel は即戻る、stdio の lock を 3 状態に、`posix_memalign`） |
| [ws034-p055](phase055/phase.md) | kernel の乱数: ChaCha20 の CSPRNG（BLAKE2s で集め、`hal_entropy_fill` と割り込みの時刻を種に）、`getentropy` を RDRAND の無い機械でも成功させる、`/dev/random`・`/dev/urandom`、`arc4random` を `getentropy` に（p054 で発見。設計 [design.md](phase055/design.md)） | cleared（q369-i01。ChaCha20＋BLAKE2s、tick の揺らぎと RDRAND で seeded、`/dev/random`・`/dev/urandom`、RDRAND の無い CPU・rpi4 でも成功） |
| ws034-p056 | pipe・socket・tty などで待っている `read`/`write` 等に `pthread_cancel` を届ける（今は cancelable な usync の待ちだけが起きる。POSIX の cancellation point。p054 で発見） | planning |
| [ws034-p057](phase057/phase.md) | userland/base のプログラムを動的リンクに（2026-09-24 ユーザー指示。sh・init・基本と network の command・Noct。4 platform） | cleared（q370-i01。PIE ＋ libc.so、静的な実行 file 0、4 platform で login。試験用 ELF は静的のまま） |
| ws034-p058 | 動的な process の起動を速くする（200 回の exec が静的の 2.5 倍。libc.so の自己参照の再配置 467 の起動時解決、ld.so と libc.so の page fault。p057 で判明） | planning |
| ws034-p059 | tmpfs の file 数の上限: `/tmp` に同時に 229 個までしか file を作れない（`ENOSPC`、df は 3%）。tmpfs は自前の inode 割り当てを持たず、kernel 全体で共有する `INODE_COMMON_MAX`（256）の pool と、追い出せない inode で埋まる `INODE_CACHE_MAX`（512）の cache に縛られる。tmpfs の mount ごとの上限（`TMPFS_DEFAULT_NODES` 1024）も小さい。inode を必要に応じて確保し、cache を tmpfs の固定された inode で塞がないようにする（ws042-p004 の guest 差分試験で発見）。**影響が大きい**: 上限に達すると `/dev/null` の open（devfs の inode）も `ENOSPC` になり、sshd も新しい session を作れなくなる。1 つずつ作って消す分には回収される（5000 回で可）。pipe は漏れない（300 回で容量不変） | planning |
| [ws034-p002](phase002/phase.md) | which（base独自実装） | cleared（q323-i02） | WS035のrefactor | `userland/base/which` |
| ws034-p006 | bash | planning | p001, p005, p036 | `packages/shell/bash` |
| ws034-p007 | GNU coreutils と既定の探索順（PATHは `/usr/bin` を `/bin` より前、ライブラリは `/usr/lib` を `/lib` より前） | planning | p001, p005 | `packages/utils/coreutils`、PATH定義の各所、`src/rtld/rtld.c` |
| [ws034-p019](phase019/phase.md) | ca-certificates（`packages/security/ca-certificates`）。CA bundleは単一のpemなので、`external.mk` に単一ファイルの取得を加える | cleared（q330-i01） | p001 | `packages/security` |
| [ws034-p021](phase021/phase.md) | zlib・expat（`libs/`） | cleared（q329-i01） | p001 | `packages/libs` |
| [ws034-p017](phase017/phase.md) | curl（libcurl含む）。`--without-libpsl`（またはlibpslを追加） | cleared（q336-i01。q334-i01 は uncleared: kernel の TCP が大きな write を届けず → p042 で直して再実行） | p019, p021, p042 | `packages/network/curl` |
| ws034-p020 | wget | planning | p019, p021 | `packages/network/wget` |
| ws034-p009 | git。`NO_RUST=1` でRustを外す（Git 3.0でRust必須の予告あり） | planning | p017, p021 | `packages/development/git` |
| ws034-p010 | emacs（端末版） | planning | p005 | `packages/editors/emacs` |
| ws034-p008 | vim | planning | p005 | `packages/editors/vim` |
| ws034-p011 | binutils と GMP / MPFR / MPC / ISL。GMP/MPFR/MPC/ISLは静的ライブラリ。ターゲット上のbinutilsとbuild機上のクロスbinutilsの両方 | planning | p005 | `packages/development/binutils`、`packages/libs` |
| ws034-p012 | gcc 16: build上のクロスgcc（x86_64-zedbsd向け、C/C++、libgcc・libstdc++）。build機用のGMP等は同じdistfileからgccのbuildの中で作る | planning | p011 | `packages/lang/gcc-16` |
| ws034-p022 | gcc 16: ターゲット上で動くgcc/g++（Canadian cross） | planning | p012 | 同上 |
| ws034-p023 | gcc 16: gfortran（libgfortran・libquadmath） | planning | p022 | 同上 |
| ws034-p013 | gdb（zedbsdのnative target） | planning | p005, p011, p021 | `packages/development/gdb` |
| ws034-p025 | meson・ninjaのクロスbuild契約（cross file生成）。GTK系の前提。gperf（host tool、sourceから）、pkg-config wrapper、package prefix、config.sub・libtoolの共通対応、symbol versioningの無効化 | planning | p001 | `packages/tools`、external.mk |
| ws034-p026 | glib（libffi・pcre2を含む） | planning | p025, p021, p005（iconv） | `packages/libs` |
| ws034-p027 | フォント系: freetype・harfbuzz・fontconfig（libpngを含む） | planning | p021, p025, p026 | `packages/libs` |
| ws034-p034 | 独自libwayland（`userland/base/libwayland`）を拡張し、GTK・Qtが使うlibwayland-clientのAPIと互換にする（`libwayland-client.so`）。wayland-cursor・wayland-eglの扱いとwayland-scanner（host道具）を含む | planning | p001 | `packages/desktop/libwayland` |
| ws034-p028 | 描画系: pixman・cairo・pango・fribidi・gdk-pixbuf・libjpeg-turbo・graphene・libepoxy・libxkbcommon。libtiffを加える（GTK4が必須） | planning | p026, p027 | `packages/libs` |
| ws034-p029 | GTK4（Wayland backend、GSKはVulkanまたはcairo） | planning | p028, p034, p038, WS035-p028 | `packages/desktop/gtk4` |
| ws034-p030 | Qt6（qtbase＋qtwayland） | planning | p027, p028, p034, WS035-p028 | `packages/desktop/qt6` |
| ws034-p031 | GTK3（atkを含む）。atkは単体のatk 2.38.0を `packages/desktop/atk` に置く（2026-09-23決定） | planning | p028, p034, WS035-p028 | `packages/desktop/gtk3` |
| ws034-p032 | Qt5（5.15、qtbase＋qtwayland）。qtsvgを加える（VLCのQt GUIが必須） | planning | p027, p028, p034, WS035-p028 | `packages/desktop/qt5` |
| ws034-p024 | FFmpeg（LGPL構成） | planning | p005 | `packages/multimedia/ffmpeg` |
| ws034-p018 | VLC（音声はOSSとpulse、GUIはQt5） | planning | p024, p032, WS035-p019 | `packages/multimedia/vlc` |
| [ws034-p003](phase003/phase.md) | PCI列挙UAPI（`/dev/system`）とlspci | cleared（q325-i03。q323-i08 は未着手の uncleared） | WS035のrefactor | system-device、pci、`userland/base/lspci` |
| [ws034-p004](phase004/phase.md) | USB列挙UAPI（`/dev/system`）とlsusb | cleared（q328-i01） | p003 | system-device、usb、`userland/base/lsusb` |
| ws034-p014 | Rust: target spec・`libc` crate・stdのクロスbuild（host上） | planning | p005 | `packages/lang/rust` |
| ws034-p033 | Rust: rustc・cargoをターゲットへ載せる | planning | p014 | 同上 |
| ws034-p035 | 従来のテスト版libwayland（`userland/base/libwayland`、`/lib/libwayland-client.so`）の削除と、利用者の本家版への移行 | canceled（2026-09-23、独自libwaylandを拡張する方針に変わったため不要） | p034, WS031の完了 | `userland/base/libwayland`、libvulkan、wltest等 |
| [ws034-p036](phase036/phase.md) | rootfsのtree化: host上に `build/<arch>/rootfs` のツリーを正本として作り、UFSのdisk imageはそのツリーから作る（`rootfs.tar.gz` は作らない）。symlinkを扱えるようにする（SONAMEのsymlink等）。開発用ファイル（`/usr/include`、`.so`、`.pc`）をツリーへ入れる | cleared（q323-i03） | WS035のrefactor | Makefile、`tools/build/`、package.mk |
| [ws034-p039](phase039/phase.md) | menuconfig: 開発用ファイル（`/usr/include`、`.so`、`.pc`）を入れるかのoption（組込み用に外せる。既定は入れる）と、baseのプログラムの既定を全部ONにする | cleared（q349-i01。amd64 専用の 9 個の platform を直し、既定を platform で絞る） | p036 | `tools/menuconfig.py`、config、`userland/base/*/Makefile` |
| [ws034-p037](phase037/phase.md) | packagesの置き場所を `/usr` へ揃える: WS032のOpenSSL等が `/lib` に置くライブラリを `/usr/lib` へ移し、packageのinstall先の規則を `/usr`（`/usr/bin`、`/usr/lib`、`/usr/include`、`/usr/share`）にする | cleared（q348-i01。openssl の library と remacs の辞書を移した） | p036 | `userland/packages/`（WS032の各package） |
| ws034-p038 | 調査: GTK4（あわせてGTK3・Qt・Chromium）が、Vulkanだけ（EGL無し）でbuild・起動できるか。buildに要るもの（libepoxy、wayland-egl等）と実行時に要るものを分けて確かめ、EGLを加えるかの判断材料を作る | planning | p001 | 文書 |
| ws034-p015 | イメージ統合・menuconfig・ライセンス表示・provenance | planning | 各パッケージ | packages全体 |
| ws034-p016 | 全文規約確認・回帰・制限整理（必須の最終確認） | planning | 全Phase | 全体 |

p005はWS032-p004と同じように一度では閉じない。後続Phaseで見つかった不足はp005へ戻して直す。
p017〜p033は2026-09-23に追加・分割したPhaseで、IDは追加順である（p017 curl、p018 VLC、p019 ca-certificates、
p020 wget、p021 zlib・expat、p022・p023 gcc、p024 FFmpeg、p025〜p032 GUI toolkitとその依存、p033 Rust）。
lspci・lsusb（p003・p004）は優先度を中くらいとして空いているスロットに入れ、Rust（p014・p033）は後回しにする
（2026-09-23ユーザー決定）。
p034（本家libwayland）とp035（従来版の削除）は2026-09-23のユーザー決定で加えた。p035はWS031が従来版を
使い終えてから、折を見て行う。

### 依存関係の解析（2026-09-23）

- p001（文書だけ）以外のPhaseは、WS035のrefactor（p002〜p004、p023）の完了を前提にする。`libc/` と
  `include/` の移動がlibc・カーネルの修正（p005）とパッケージのsysroot参照に直接効くため。
- WS032はcompleted。`external.mk`、クロスビルド契約、OpenSSL、OpenSSH、clang、libc++が使える。
- **curl・wgetの前提**: どちらもHTTPSの検証にCA bundleを使うので、ca-certificates（p019）を先に行う。
  curlはzlib（p021）も使う。gitはlibcurl（p017）を使う。
- **GUI toolkitの前提**: GTK系はmesonでbuildするため、mesonのクロス契約（p025）が要る。GTK・Qtの
  Wayland backendは、zdesktopがtoolkitの要るWaylandの範囲（WS035-p028）を持つことが前提になる。
  VLC 3.0のGUIはQt5を使う。
- p003（PCI）とp004（USB）は、同じ `/dev/system` のUAPIを変えるので直列にする。
- 並行しやすい組み合わせ: 独立したpackage同士（例: bash と vim）は `userland/packages/` の別ディレクトリなので
  並行できる。ただし、両方がp005（libc・カーネル）へ修正を戻す場合は、p005の変更を直列にする。

## p001の結果（2026-09-23）

[ws034-p001の結果](phase001/results.md)、成果物は [package-inventory.md](package-inventory.md)。tarball 53件を取得し、
sizeとSHA-256を実測した（`archive.sh verify` 全件ok、`build/distfiles` はgit管理外）。主な版: gcc 16.2.0、
binutils 2.47、gdb 17.2、git 2.55.0、emacs 31.1、vim 9.2.1125、Qt6 6.11.2、Qt5 5.15.19、glib 2.90.0（GTK 4.24が
要求）、FFmpeg 8.1.3（VLC 3.0.24が前提とする系列）、ISL 0.24（gcc 16.2の指定）。Rustは1.98.1を仮に記録。
inventory §6の修正案をPhase表へ反映した（依存の追加と範囲の追記）。

検証の限界: 署名のうち12件はkeyserverから引いた鍵で、配布元が公開するfingerprintとの独立照合はしていない。
VLCの署名は鍵が得られず未検証（公開SHA-256とは一致）。

## p001で見つかった、判断が要る点（2026-09-23）

1. ~~iconv~~ → 決定（2026-09-23）: libcに実装する。対応はASCIIとUTF-8だけ（p005）。他の文字コードへの変換は失敗を返す。
   glibを使うのはGTK4・GTK3とその依存（pango、cairo-gobject、harfbuzz-gobject、gdk-pixbuf、atk）。iconv自体はwget（IRI）、
   git、VLC（字幕）、gdbも使う。
2. ~~termcap API~~ → 決定: baseのcursesに足す。ncursesは使わない（p005）。
3. ~~本家libwayland~~ → 方針変更（2026-09-23）: epoll・timerfd・signalfdはまだ実装しない。本家のpackageはやめ、独自の
   `userland/base/libwayland` を拡張して `libwayland-client.so` でGTK・Qtに対応する（p034を変更、p035を取消し）。
   以前の「本家を `packages/desktop/libwayland` に入れる」決定はこれで置き換わった。
4. ~~GTK3のatk~~ → 決定: 単体のatk 2.38.0を使う。置き場所は `packages/desktop/atk`。
6. ~~SONAMEとimage~~ → 決定（2026-09-23）: `rootfs.tar.gz` は当面作らない。ツリーからUFSのimageを作る。開発用ファイルを
   入れるかをmenuconfigのoptionにし（既定は入れる）、baseの既定を全部ONにする（p039）。
   それ以前の記録: host上にrootfsのツリーを作り、それをimage化する（ユーザーがツリーを見やすい）。
   SONAMEはUFS上のsymlinkにする（p036）。packagesは `/usr` 以下に置き、OpenSSL等も移す（p037）。ターゲット上で
   buildするので `.so`（開発用のsymlink）と `/usr/include` もツリーに入れる。
7. ~~GTK4のEGL~~ → 調査Phase（p038）で確かめてから、EGLを加えるかを後で検討する（2026-09-23）。zedBSDのGPUドライバは
   DRMを使わないので、libEGLを作るならほぼ完全な独自実装になる。Vulkanだけならlibepoxy・EGLは実行時に要らない見込み。
5. ~~host道具~~ → 決定: ホストへのpackage導入とsudo操作は自由に行ってよい（2026-09-23ユーザー承認、Guardrailに記録）。
   gperf等はhostのpackageを入れてよい。同版が要るもの（wayland-scanner、Qt 6.11.2、emacs 31.1）はsourceからhost用にbuildする。
6. **image上の置き方**: 版付きSONAMEとsymlinkをimageにどう載せるか、pkg-config wrapperとpackage prefix。
   p025で決める案。

## Phaseの要点とリスク

- **p001**: 版を確定する（bash 5.x、coreutils 9.x、vim 9.2、emacs 31.1、git 2.5x、
  gcc 16.x、binutils 2.4x、gdb 16/17、Rust stable）。取得元とSHA-256、各パッケージの
  cross-build方式、依存パッケージの配置、依存グラフ、QEMU amd64での受入手順
  （GUIはLavapipe Venus）を決め、以降のPhaseを具体化する。
- **p002 which**: POSIX外のコマンドなので挙動の基準を決める（`-a`、PATHの空要素、
  実行権限の判定）。host試験とターゲット試験で確かめる。
- **p003/p004**: UAPIは次のとおり（2026-09-23ユーザー承認）。`KERN_SYSTEM_GET_PCI_DEVICE` / `KERN_SYSTEM_GET_USB_DEVICE` を
  index指定の `_IOWR` とし、既存の `KERN_SYSTEM_GET_DEVICE` と同じ列挙方式にする。
  返す項目は、PCIがbus/dev/fn・vendor/device・class/subclass/progif・revision・
  subsystem・bound driver名、USBがbus/address/port path・VID/PID・class・
  bound driver名。ID→名前の表（pci.ids/usb.ids）は同梱しない（ライセンスとサイズのため。数値表示を基本とする）。
  UAPIのサイズ・layoutを`_Static_assert`で固定し、ILP32/LP64の両方で確かめる。
- **p006–p008**: autoconfのcross cacheで対応できる見込み。bashはjob control/termios/
  `wait`系、coreutilsは`stat`/`statfs`/locale/`fts`系、vimはterminfo/curses
  （baseの独自curses）が主な確認点になる。
- **p007のライブラリ探索順**: `src/rtld/rtld.c` の既定探索を `LD_LIBRARY_PATH` → `/usr/lib` → `/lib` に変え、
  コメントの方針（baseを隠さない）も書き換える。変更の前に、`/lib` と `/usr/lib` に同じ名前のライブラリが
  あるかを全package構成で調べる（WS032のOpenSSLは `/lib/libcrypto.so` などに入れている）。同名があれば、
  どちらを残すかを決めてから変える。ターゲット上で、既存の動的実行ファイル（base、WS032のpackage）が
  従来どおり動くことを確かめる。
- **p007のPATH順序**: coreutilsは`/usr/bin`、baseの同名コマンドは`/bin`に入る。
  GNUがある場合はGNUを優先する（ユーザー決定）ため、既定PATHは`/usr/bin`を`/bin`より前に置く。
  現状は`/bin`が先で、次の箇所に分散している（2026-09-23調査）。
  `userland/base/login/main.c`、`src/kern/exec.c`（init環境）、`userland/base/sh/main.c`と
  `builtins.c`、`userland/base/newgrp/main.c`、`userland/base/common/command.c`、
  `userland/base/libc/posix.c`（`confstr(_CS_PATH)`など3箇所）、`include/libc/paths.h`
  （`_PATH_DEFPATH`/`_PATH_STDPATH`）。まとめて順序を変え、base側のscriptやserviceが
  `/bin`のbase実装を前提にしている箇所が無いかも確認する。
- **p017 curl・p020 wget**: curlはOpenSSL（WS032）とzlibを使い、`libcurl.so`と`curl`を入れる。
  wgetもOpenSSLを使う（GnuTLSは使わない）。どちらもDNS解決（`getaddrinfo`）、
  TLS証明書の置き場所（CA bundle）、`poll`/非blocking connectの確認点になる。
  CA bundleの入手元とライセンスはp001で決める。
- **p018 VLC**: VLCはLGPL-2.1+（一部モジュールGPL）。デコードはFFmpeg（`multimedia/ffmpeg`、
  LGPL構成）を使う。依存ライブラリの範囲と、GUI（Qt）を入れるか、最初は`cvlc`（Qt無し）に
  するかはp001で決める。音声は、まずOSS出力（`/dev/dsp`直接）で鳴らし、次にpulse出力
  （WS035 p019の互換libpulse経由）を確認する。pulse出力は遅延・時刻情報の問い合わせ、
  一時停止・再開、flush、音量・ミュートを使うので、互換libpulseの確認にもなる。映像はzdesktopの
  X11またはWayland出力で、GUIの確認はLavapipe Venusで行う（受入環境の決定どおり）。
- **p009 git**: `NO_PERL`/`NO_TCLTK`/`NO_GETTEXT`で依存を絞る。HTTPSはp017のlibcurl＋OpenSSL
  （WS032）で行う。ssh経由のcloneはWS032のOpenSSH（p007 planning）に依存する。
- **p010 emacs**: cross-compileでは、ビルド時にtemacsの実行（pdumper）と.elcの
  byte-compileが必要なことが最大のリスクである。hostで同じ版のemacsを使ったlisp
  byte-compileと、ターゲットでのdump（初回またはイメージ作成時）を検討する。端末版
  （`--without-x`）を初期範囲とする。
- **p011/p012 gcc**: build上で「x86_64-linux → x86_64-zedbsd」のクロスgccを先に作り、
  それを使ってhost=zedbsdのgcc（Canadian cross）を作る。`config.gcc`/`config.sub`
  （binutilsも含む）へzedbsd targetを加えるパッチが要る。gfortranのためlibgfortranと
  libquadmathも扱う。ld.soとcrtの配置はzedBSDの動的リンク契約に合わせる。
- **p013 gdb**: zedbsdのnative target（ptrace・register・thread・`/proc`相当の代替）の
  移植が主な作業。lldbで既に使えているptrace ABIを基準にする。
- **p014 Rust**: 新しいOS targetとして、target spec、`libc` crate、std（unix系）の
  zedbsd対応が要る。host上でのcross-compile（std付き）を先に成立させ、次にrustc/cargoを
  ターゲットへ載せる。LLVMはRust同梱版かWS032のLLVMかを選ぶ必要がある。最も大きいPhaseである。

## 決定事項（2026-09-23ユーザー回答）

1. packagesは分類する: `lang/gcc-16`、`editors/emacs`、`editors/vim`、`shell/bash`、
   `utils/coreutils`、`development/git`、`development/gdb`、`lang/rust`。
2. lspci/lsusbは `/dev/system` のindex指定ioctl（`KERN_SYSTEM_GET_PCI_DEVICE` /
   `KERN_SYSTEM_GET_USB_DEVICE`）で実装する。
3. coreutilsはpackagesなので`/usr/bin`に入る。baseの`ls`等は`/bin`にある。PATHは
   `/usr/bin`を優先し、GNUがある場合はGNUが使われる。
4. 受入環境はQEMU amd64のみ。GUIアプリはLavapipeを使うVenusで確認する。

依存パッケージの置き場所は上記の既定案をp001で確定する。

## 決定事項（2026-09-24ユーザー回答）

1. **autotools の package は package ごとに patch する**（inventory §10 の (a)。OpenSSH の今のやり方）。
   共通の script や GNU config への登録はしない。wget（p020）、bash、coreutils 等はこれで進められる。
2. **git は `NO_RUST=1` でよい**（p009）。
3. **epoll・timerfd・signalfd は必須でない見込み**: 本家 libwayland は使わず独自実装なので、POSIX の範囲で
   Wayland を作れるかを先に調べる（p050）。できないか難しい場合に実装を相談する。
4. `FD_SETSIZE` は他の POSIX と同じ値にする（1024、p048）。
5. shell の `kill %N` は直す（p047）。
6. 動かない試験は書き直さず削除する（p049）。試験の保守の費用を払い続けない。

## GUI toolkit（2026-09-23追加）のリスク

- **libwayland（2026-09-23ユーザー決定）**: 本家libwayland（MIT）を `packages/desktop/libwayland` に入れる。
  `/usr/lib` にbuildされる。WS014で作った従来のテスト版（`userland/base/libwayland`、`/lib/libwayland-client.so`）は
  WS031がまだ使っているので残し、WS031が終わってから折を見て削除する（p035）。
  - **ローダの探索順**: 今の `src/rtld/rtld.c` は `LD_LIBRARY_PATH` → `/lib` → `/usr/lib` の順に探し、コメントにも
    「baseのライブラリは決して隠されない」とある。2026-09-23のユーザー決定で `/usr/lib` を先に変える（p007）。
  - ただし本家のSONAMEは `libwayland-client.so.0` で、GTK・Qtはこの名前を `NEEDED` に記録する。従来版は
    `libwayland-client.so` の名前で `/lib` にあるので、実行時に名前がぶつからず、本家版が読まれる見込みである。
    p034でSONAMEと `NEEDED` を実際に確かめる。
  - build時は、sysrootで本家のヘッダ（`/usr/include/wayland-*.h`）と、libcの `include/` にある従来版のヘッダ
    （`wayland-client.h` 等）が同じ名前で並ぶ。packageのbuildが本家のヘッダとライブラリを確実に拾うように、
    include・link経路をp034で決める。WS035のrefactor（libcヘッダの `include/` への移動）とも関わる。
- **フォント**: GTK・Qt・cairo・pangoはFreeType・fontconfig・harfbuzzを使う。packagesの境界ではFreeTypeの
  ライセンス（FTLまたはGPLv2）も使える。base側のlibtruetype（WS035-p010）とは別物として扱う。
- **描画**: EGLはゲスト実装を取り消したまま（WS030）なので、GTK4はGSKのVulkan rendererかcairo（ソフトウェア）を使う。
  Qtは `-no-opengl` 相当でbuildし、Vulkanかraster描画を使う。
- **入力**: libxkbcommonとxkeyboard-configのkeymapを、zdesktop（WS035-p028）が提供する必要がある。
- **GTK3**: atkが要る。accessibility bridge（at-spi2、dbus）は最初は入れない。
- **Qt**: C++17とCMakeのクロスbuild（WS032の契約）を使う。ICUは最初は使わない。

## 未決事項（2026-09-23）

1. ~~libwayland~~ → 決定済み: 本家を `packages/desktop/libwayland` に入れる（上記）。
2. ~~GUI toolkitの分類~~ → 決定済み: `packages/desktop/`（gtk4、qt6、gtk3、qt5、libwayland）。
3. ~~lspci・lsusb・Rustの時期~~ → 決定済み（2026-09-23）: lspci・lsusbは優先度を中くらいとし、空いているスロットに入れる。
   Rustは後回し（全体の最後）。
4. ~~ローダの探索順~~ → 決定済み（2026-09-23）: `/usr/lib` を `/lib` より優先に変える。PATHの `/usr/bin` 優先と
   同じ考え方で、packageがbaseの同名ライブラリより優先される。p007で、`src/rtld/rtld.c` の既定探索
   （`LD_LIBRARY_PATH` → `/usr/lib` → `/lib`）とそのコメントを直す。

## 現状

計画を作っただけで、Queue・実装・試験はまだ無い。GitHub Issueも未作成である。
