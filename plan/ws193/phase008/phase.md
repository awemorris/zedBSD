<!-- awesome-plan project=zedbsd record=ws193-p008 -->
# WS193 p008: RPi4の選択configでのrootfsビルド修正

Status: cleared
Disposition: normal
Parent: [WS193](../ws.md)
Queue: [Codex RPi4 build](../codex-rpi4-build-queue.md), rpi4-config-build-20261011-i01

## 承認・観測・有限範囲

2026-10-11 user「config.mkでmake -j16しましたが、ビルドエラーです。直せますか？」。
提示error: `make: *** No rule to make target 'build/arm64/dynamic/libbrowser.so', needed by 'build/arm64/rootfs/.stamp'. Stop.`
main base ad9d2f6d3、保存configはtests/rpi4-build-config-20261011.mk。全CPU共通registryでlibbrowserを選択できるが、従来のdesktop DSO/application link ruleはamd64/vmunix.mkだけにある。選択を消すのでなく、対応するarm64ビルドを補う。rootfsに選択したappも配置する。
有限範囲はこのconfigの同原因のルール不足・そのコンパイル/リンク修正、対象build、変更全文review、結果記録。kernel/driver機能移植・HAL API変更・toolchain変更・実機/QEMU・pushは含めない。未知の前提/別原因はその時点で照合。main統合は具体的commitの承認を確認。

## 設計・規則・受入

既存amd64のportable desktop/library link依存を共通化し、target CRT、compiler/link flags、ELF machineをplatformから渡す。CPU固有libc/rtld/assemblyは現行platform内へ残す。arm64が持つcompat library ruleは重複定義を避ける。amd64の展開recipeを変更前と照合する。
AGENTS/Guardrail、C変更時はcoding-style全文。専用worktree、自分のbuildだけを使用、共有LLVM/sourceはreadonly link、sysrootは現在のmain成果を複写。共有計画/cache/GitHub公開はQ1へ保留。
受入: libbrowserのarm64 build/依存が解決し、現在configでのrootfs依存に同原因のmissing ruleがない。対象buildで実AArch64 ELFを生成・検査、変更箇所の警告0、diff-check/manual full-rule review。image/実機は未実施なら明記。

## 追加のビルド調査 / 2026-10-11

libbrowserと全DSO依存の実arm64 build/ELF検査PASS、warning/error 0。共通化したamd64のsource/link/check recipeを逆置換で元の全文と比較し一致PASS（移動境界の末尾空行を除く）。
現在configの続くbuiltin buildではstatic keiland-previewのmath/strtod不足を検出し、amd64と同じく静的math/float parseを補完。OpenSSLのConfigure targetがamd64/i386しか無いためAArch64 LP64/no-asm targetを追加、package source patch levelを上げる。OpenSSHのlibutil copyをarm64へ補完。ELF machine名の対応はpackage個別でなく共通validatorでarm64→aarch64へ正規化する。同じconfigのルール/ビルド補完として進め、driver機能移植は含めない。
ユーザーは質問「専用worktree内でarm64版Noctをビルドしてよいですか？」へ「専用worktreeでビルドしてよい」と回答。Noctのsource/archiveは現在mainから自分のworktreeへ複写、host interpreterはreadonly linkで使う。共有LLVM/source/Noctは変更しない。Noctのtoolchain source変更はこの承認には含めていない。
最初のpackage取得はsandbox DNSで失敗。現在mainの検証archive・firmware cacheを自分のbuildへ複写して再利用。元source/licenseの検証は既存Makeが行う。共有buildは書き込まない。

## 最終結果 / 2026-10-11

rpi4-config-build-20261011-i01 cleared。現在configのライブラリ/選択app/static preview、kernel、承認済みNoct、外部package、rootfs、SD imageの通常make -j16が完了。イメージ終盤の固定256MiB容量不足もこのconfigのbuild修正として、FAT配置/root開始LBAを保って必要な大きさへ伸長。最終root199MiB、SD image328MiB。
`make check-disk-image`でMBR/FAT/収録payload/UFS byte一致PASS。最終変更全文の規則review、Python syntax、amd64移動recipeの逆置換全文一致、ELF arm64 alias/異CPU拒否、diff-check PASS。変更coreのcompiler警告0、外部packageの既存warningとtoolchain固定/未bootの制限は[証拠](../tests/rpi4-build-20261011.md)に記録。
main統合は具体的WIP commitの承認待ち。共有Master/Queue/Past Log投影とGitHub公開はQ1へ保留。WS全体p003/実機受入をこのbuildのみでclearにしない。

## Runtime follow-up / 2026-10-11

RPi4 UAT写真でssh-keygenの依存不足を確認。上記のlibutil copyはpackageリンク用で、arm64 rootへの収録が抜けていた。p008の当時のbuild-only結果/clearedは保持し、[p010](../phase010/phase.md)でruntime配置とCPU共通rootfs manifestを補完。p008のbuild成功をOpenSSH実行成功とはしない。構造の追加と結果はWS記録へ反映、shared投影/GitHubはQ1 pending。
