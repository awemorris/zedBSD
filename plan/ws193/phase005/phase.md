<!-- awesome-plan project=zedbsd record=ws193-p005 -->
# WS193 p005: CPU共通のユーザーランド選択とFFmpeg arm64

Status: cleared
Disposition: normal
Parent: [WS193](../ws.md)
Queue: [Codex Queue](../codex-userland-queue.md), menuconfig-userland-20261010-i01

## 承認と有限の範囲

2026-10-10 current user: 「arm64のmake menuconfigで、packages->multimediaにffmpegがない。ユーザランドはまったく同じ情報を元にメニューを表示してほしい…レビューして直せる？」。
追加回答: 「全項目を全CPUで選択可能にしたい」。未対応項目のdisabled表示案は採用しない。
Base・Desktop・Packagesの登録をCPU共通にする。makeの一覧、選択・保存・依存展開・実効選択が同じregistryを使い、arm64へ切り替えても選択を落とさない。FFmpegのarm64 configure/build設定を補完する。
Firmwareと直接記述専用Tests/X11の既存platform条件は保持。全optional packageの全CPU移植・全件build・実機再生・image生成は今回の確認範囲ではない。
有限枠: この1項目の修正、対象host確認、FFmpeg arm64 package build、変更全文レビューと記録。main統合・pushは個別承認対象。

## 原因と設計

main 93914124cでamd64/rpi4のmake list-user-programsは同一。menuconfigは一つのregistryを取得するがplatformで絞り、libavcodec登録はamd64限定。saveとmake側もplatform条件で選択を落とす。
libavcodecのconfigureはamd64以外をx86_32としていた。CPU共通の選択方針をroot Makefileのregistryに一箇所で適用し、FFmpeg archはaarch64を明示する。external source/archive/licenseは変更しない。

## 依存・標準・完了条件

前提: mainのp004実装93914124c、既存FFmpeg外部source、既存read-only LLVM/sysroot。
AGENTS.md、Guardrail、外部source境界・共有tree read-onlyを適用。Python/Makeは既存規約を保持、C source変更なし（C全文規約のgeneration対象なし）。対象差分の全文レビュー、構文、git diff --checkを行う。
対象host確認: amd64/rpi4の一覧共通、Base/Desktop/Packagesの全項目が選択・保存可能、依存が保存後makeの実効選択に残る。arm64 FFmpeg buildとELFのmachine/soname/neededを確認。
受入はこの限定範囲。WS全体や全optional packageのarm64実行成功は主張しない。GitHub公開・shared Master/Queue投影はQ1へ保留。

## 2026-10-10 結果 / menuconfig-userland-20261010-i01

cleared（今回の限定範囲）。全6platformの255共通項目で表示・依存選択・保存/load・Make実効選択PASS。RPi4 real handler/PTYのFFmpeg表示・選択PASS。arm64 configure/build/stage exit 0、全5共有libraryのELF/needed検証とstrong import照合PASS。変更3 source filesの適用規則全文review・Python構文・diff-check PASS。
[コマンド・環境・失敗と修正・警告/未実施範囲](../tests/userland-selection-20261010.md)。外部FFmpegの12 compiler警告は未変更upstreamのものとして記録し、warning 0とは報告しない。全optional packageの全CPU移植は今回の受入に含めない。

main統合は具体的成果commitについて確認する。WS全体のp003/受入と共有Master/Queue/GitHub公開は未完のまま。push無し。次Queueを開始しない。
