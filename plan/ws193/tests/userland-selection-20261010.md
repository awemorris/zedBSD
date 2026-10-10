# CPU共通のユーザーランド選択 / 2026-10-10

Source: この記録を含むWIP commit。Base/Main registry、FFmpeg package、既存menuconfig-target-host-testの政策の照合。
Environment: Linux host、Python 3.13.5、GNU Make 4.4.1、project clang 23.1.0（LLVM driver stamp d7f1bbac）。main 93914124cを基点にcodex/menuconfig-userlandの独立worktree。

## 原因・修正のレビュー

`make --no-print-directory list-user-programs ZEDBSD_PLATFORM=amd64` と `ZEDBSD_PLATFORM=rpi4` の出力は修正前から同一。独立したユーザーランド一覧は無かった。FFmpegのplatform登録がamd64のみで、menuのfilter・save・makeの実効選択filterが項目を落としていた。
rootの `ZEDBSD_USERLAND_PACKAGE` がBase/comp/Desktop/PackagesのPLATFORMSを `*` に統一する。menuだけの例外を入れず、一つのregistryを一覧・既定選択・保存・buildの依存展開/実効選択が使う。ユーザーの追加指示に従い、CPU未対応というdisabled項目も作らない。既存のFirmware/Tests/X11条件は保持する。
FFmpegはarchをamd64→x86_64、i386→x86_32、arm64→aarch64、sparcv9→sparc64、その他→architectureのままとし、nasmを要求するのはx86だけ。ELF checkerにはarm64をaarch64へ変換する。archive/source/LGPL境界・ライブラリsoname・dlopen方針は変更なし。

## 対象host検証

一時probeはown build/menuconfig-userland/verify.py（開発用、共有回帰の道具へ追加しない）。既存の `check_packages()` だけをimportして実行（cleanupを含む全fixtureは走らせない）。実registryと実関数を使用。

- i386/amd64/pc98/rpi4/sun4u/x68k: 各CPUのBase/comp/Desktop/Packagesの255項目の一覧が同一。全項目をpackage_selectで依存ごと選択、save/loadの値とMakeのZEDBSD_USER_PROGRAMS実効値にも全255項目が残る: PASS。
- real edit_program_group、rpi4/Multimedia: FFmpeg行が表示され、操作で選択して `[*]` になる: PASS。
- real PTY: `TERM=xterm-256color make menuconfig ZEDBSD_CONFIG=build/menuconfig-userland/rpi4.mk`。Packages→MultimediaでCurrent target: Raspberry Pi 4 Arm64、`[*] FFmpeg audio and video libraries (LGPL)`を確認し終了/保存: PASS。
- MakeのFFmpeg configure options展開を5 architectureで照合（x86だけnasm）: PASS。i386/SPARC/m68kのbuildは実施していない。
- `make --no-print-directory -s ZEDBSD_CONFIG=build/menuconfig-userland/rpi4.mk validate-image-config`: PASS。
- Python両fileのcompileによる構文確認、`git diff --check`: PASS。

## FFmpeg arm64 package build

共有LLVMはread-only symlink、既存arm64 sysrootとverified tarballはown buildへ複写。共有成果へ書き込まない。古いsysrootのheaderで最初にlibc compileが失敗（AI_ALL等の現行定数不足）したため、own copyのusr/includeだけを現行libc/uapi/desktop public headerの配置規則に従って複写した。LLVM/sysrootの再build・source変更無し。既存のcrt/builtinsを再利用、own dynamic libcは通常のpackage前提としてbuild。

```
make --no-print-directory -j16 \
  -o "$PWD/build/arm64/sysroot/.zedbsd-sysroot-complete" \
  ZEDBSD_CONFIG=build/menuconfig-userland/rpi4.mk libavcodec
```

構成はplatform=rpi4、architecture=arm64、board=rpi4、variant=default、user_programs=libavcodec。最初のFFmpeg生成後のchecker失敗（arm64という未受理名）は修正済み。最終実行exit 0、stage/.zedbsd-staged生成。ARCH_AARCH64=1、ARCH_X86=0、HAVE_NEON=1、HAVE_PTHREADS=1、FFMPEG_LICENSE=LGPL version 2.1 or later。

`tools/build/check-dynamic-elf.py --machine aarch64 --role shared-library` に各sonameと必要ライブラリを明示して全5本PASS:

| Library | NEEDED |
| --- | --- |
| libavutil.so.61 | libc.so |
| libswresample.so.7 | libavutil.so.61, libc.so |
| libavcodec.so.63 | libswresample.so.7, libavutil.so.61, libc.so |
| libavformat.so.63 | libavcodec.so.63, libavutil.so.61, libc.so |
| libswscale.so.10 | libavutil.so.61, libc.so |

llvm-nmで5本のstrong importを5本+own dynamic libcのexportsと照合し残り無し。libc自身の残りは既存rtld private hook `__rtld_exports` のみ。

## 全文規約の確認・限界

AGENTS.md/Guardrailの全適用規則で変更3 source filesを確認: userland/source registry単一、依存/保存の一致、device group条件の保持、外部codeのtree非取り込み、共有成果read-only、WIP、pushなし。C新規生成無し。Python/Makeの既存規約を保持し、実関数とparseで確認。
own libcは `-Wall -Wextra -Werror` で成功。外部FFmpeg sourceは未変更だがcompiler警告12件（unused-function 7、implicit-fallthrough 5）、configureには既存 `--pkg-config=false` によるWARNING 1件。外部sourceを修正/警告抑制してwarning 0とは報告しない。今回の変更したPython/Makeには規約違反残件なし。

全optional packageの各CPU buildは未実施。新方針で全項目が選べることと、各々のCPU移植が完了していることは別々に記録する。image/QEMU/実機再生/CI/pushは未実施。共有Master/Queue投影とGitHub公開はQ1へ保留。
