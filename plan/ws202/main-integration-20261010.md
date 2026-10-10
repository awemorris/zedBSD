# WS202: main統合（2026-10-10）

Event: `ws202-main-integration-20261010`。
承認者: current user、この会話の明示「mainへの統合はあなたがやってOKです。」。
範囲: 検証済みWS202のsource/tests/docs/buildをmainへ統合、main第4版との記録の意味の統合、移管に必要な既存host回帰/AATの追従、対象buildと回帰確認。
Status: finished。i01 cleared（main統合/build/対象回帰の部分scope）。WS/whole Phaseの未達は保持。

## 統合する成果と設計の保存

- main基点: `c301a01a80bc7d4b16ce5a4e49fa8ab50bcbb607`（clean）。独立branch: `codex/ws202-native-media`、source commit `b43d8c1421ab8a90a2efb28c165e0bdcfa0ca200`、最終records `214df66771912c99705526d0006e9a07ec80d79e`。
- 累積source-only binary patch: 4036810 bytes、SHA-256 `b3a214b1eca0eaaea48cd4c3543741ead8d9272be7ccff0a4ed338c063a3e6d2`。mainでapply-check PASS後に適用。i915/libvulkan標準NV12 readback、AAC-LC、H.264/parser/DPB/Vulkan runtime、native-only libmedia、app所有fallback、MP4 metadata、Video Player/Music、probe、固定入力、host/docs/buildを含む。
- mainのdesign第4版、review-001/002/003、全acceptance、Phase目的は保持。ユーザーの後の明示方針を現在の契約として追記し、独立worktreeのdated結果/承認/履歴を保存。旧degraded/ライブラリ内FFmpeg/Intel Tile Y読み出し/答え待ちを現在の手順として扱わない。
- offline ID衝突を修復: mainの`ws202-p016`はref-listのまま。独立branchの標準readbackの旧p016を、未使用確認した[ws202-p017](phase017/phase.md)へ由来付きでmapping。i03承認/結果/元commit/旧IDを保存。p009/p010はp017の実装済み出力へ依存、physical hash未確認でwholeをclearedにしない。
- H.264規格8.2.4.2.3のnon-existing frameのB-list除外の訂正をp008/p015/p016/p009/design/WSへ投影。欠けた必要参照はdrop、実GPU DPBへfake referenceを渡さない。
- p001〜p011/p015/p016/p017はsoftware出力を保存してwhole uncleared。p012/T1・p013/User UAT・whole p014はplanned。software部分i11 clearedはこれらの免除ではない。WSはincomplete。

## 共有回帰とAATの追従

`aat-native-proposal.patch`をmainへ適用。Music helperから起動時のFFmpeg load待ちを外し、AAC-LCの`backend=libmedia`を検証。Music/Video Playerのscenarioをnative-first/app fallbackと現行logへ更新。Python `compile()`の構文確認とshell `sh -n`はPASS。guest AATは未実施。

`plan/tools/media/host-layout.c`のlayout headerをmedia-appへ変更。`run-host-codec.sh`はapp adapterとnative AACをcompileし、元の8 codec/containerを維持。hostのnative GPU admissionだけを明示DEVICEにし、videoは本物のdlopen FFmpeg、AACは本物のnativeで確認。GPU fake pixelsは使わない。C89、負のreceive/全send/read/両drain/seek失敗の伝播とcleanup、失敗した素材生成/ABI確認のexit伝播を補った。新しいrunnerは独自の`build/tmp/ws202-main-codec`を使い、共有旧成果を削除しない。

実行: `sh plan/tools/media/run-host-codec.sh`（通常ホスト、ASan/UBSan/LeakSanitizer有効）、exit0。

- layout 61（host FFmpeg 7）/63（固定FFmpeg 9.0.2 staged headers）: 両方PASS。
- 元のMPEG-4/AAC sample、H.264/AAC MP4・Matroska、HEVC/AAC MP4、VP9/Opus WebM、Theora/Vorbis Ogg、MJPEG/PCM AVI、H.264/MP3 TS: **8件PASS**。
- 各caseの全packet/drain、video個数/表示時刻/実BGRA、audio length、middle seekを確認。途中エラーをoutputありと誤認しない。
- 最初の新runnerはops initializerの個数でcompile失敗、修正後はtest自身のEOF判定がENOENTだったため8件fail。mediafileのENODATA契約へ修正し再実行、全件PASS。production decoderの修正は不要だった。失敗を旧runnerの成功扱いとして消さない。
- final log: `build/ws202-main/codec-regression.log`。全fixture/logはlocal buildのみ、repositoryへコピーしない。

## mainのビルド

mainのsourceを別output `build/ws202-main/`へbuild。共有sysroot/LLVM/Noctは読み取りのみ。初回は共有sysrootのKeiland headerがmainの最新Phone APIより古く、既存libkeiland/systemのcontacts/record/KL_PHONE_*で失敗。共有sysrootを変更せず、mainの公開header `userland/desktop/include`をこのbuildで優先して再実行した。

```sh
make BUILD=build/ws202-main \
  ZEDBSD_CONFIG=plan/ws202/tests/config-media.mk \
  ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot \
  -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete \
  'DYNAMIC_CPPFLAGS=-nostdinc -I. -Iinclude -Iuserland/desktop/include -isystem /home/awe/zedBSD-claude1/build/amd64/sysroot/usr/include -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC' \
  -j4 build/ws202-main/dynamic/libmedia.so \
  build/ws202-main/dynamic/libvulkan.so build/ws202-main/dynamic/libbrowser.so \
  build/ws202-main/bin/videoplayer build/ws202-main/bin/music \
  build/ws202-main/bin/media-probe build/ws202-main/vmunix
```

最終exit0、warning0/error0。kernel include check PASS（367 objects/9618 dependencies）、amd64 vmunix/ELF/依存検査PASS。libmediaのNEEDEDはlibcのみ、appはFFmpegへ直接linkしない。7対象のSHA-256は[独立worktreeの最終artifact](handoff-20261010.md)と全て一致した。final log: `build/ws202-main/integration-build-current-headers.log`、初回失敗は`integration-build.log`に保持。通常image作成のsysroot更新はQ1/T1の既存手順で行う。

## 規約・対象確認と限界

- 累積sourceの全文規約/manual review/独立host PASSは[software結果](playback-result-20261010.md)を参照。新host-codec.cは全文§1〜14を見直し、clang-format 19.1.7、関数引数のTABを復元、`python3 plan/tools/style-check.py plan/tools/media/host-codec.c --summary` total0。layoutの移動はinclude/commentのpathだけ、既存sourceを無関係に一括整形しない。
- `git diff --check` PASS。AAT helper Python syntax/shell syntax PASS。
- `sh plan/ws202/tests/run-host-nv12-readback.sh`（通常ホスト、ASan/UBSan/LeakSanitizer有効）: exit0、plain/sanitizerともPASS。mainのNV12 plane範囲/stride/offset/reject、既存i915 command、WS083 libvulkan video、標準format queryを確認。実decode画素は生成しない。logは`build/ws202-main/nv12-readback.log`。
- compiler GCC14.2.0/Clang23.1.0、clang-format19.1.7、host FFmpeg7.1.5。

[T1準備](t1-playback-request-20261010.md)の入力/config/実probe/window手順をmainへ保存。実機のnative decode/readback hash、音・同期・seek・pause/fullscreen/end、QEMU非対応/no-FFmpeg、全fixture matrix（missing-packet/ITU/crop/SAR/WS083等）、性能/同時session/UAT/whole p014/共有Tools登録は未完。詳細はsoftware結果の残件を保持する。main統合と実機で再生できたことは別の結果であり、WS completedとはしない。

Master/共有Queue/Guardrail/履歴/Tools/GitHubのprojectionは既存Q1所有のためpendingとしてこの記録へ残す。これらの共有fileを上書きしていない。push/remote公開、image作成/起動、他者への外部メッセージは実行していない。


## 共有編集の保存

統合中に`plan/beta2.md`へ別session/人の未commit編集が現れ、必須列にWS202が追加された。編集は読み取り確認のみ、変更せず、このsessionのcommitへ含めない。WSのTargetへ観測した現状を投影するが、release公開やwhole p014の免除の承認として扱わない。mainのWS202差分だけを確定し、共有編集を残す。

### main確認logのSHA-256

- `integration-build-current-headers.log`: `65422412f0e83428ad0e27a0d8be07bb5313073742dcfa7c347ce3644efddcd4`
- `codec-regression.log`: `864992973356bebea0b37ec169e265de4643e54cba1564420b6a3b891fbe48fc`
- `nv12-readback.log`: `fc9e3f2190161f03ea8116a5c8d08133d541f40c72801b765befd99f206c662f`


## Git統合の確定

source/記録/共有回帰の統合commit: `22f1d6480`（WIP）。source patchの全pathを独立branchと比較し、検証済みsourceがmainへ取り込まれたことを確認した。続いてbranch `codex/ws202-native-media`（`214df6677`）をmerge親として記録する。ソースを先に反映・検証して記録の競合を意味で解消済みのため、このmergeのtreeはmain側を採る（`-s ours`）。取り込み済みbranchの古い計画で第4版を上書きせず、後から同じbranchを再投入しないための履歴統合であり、sourceの代わりに空のmergeで済ませたものではない。

承認された統合はfinished、named build/8 codec/container/61・63 ABI/NV12対象host/構文・diff確認PASS。残る共有未commit編集は`plan/beta2.md`だけで、このsessionの成果に含めない。master.mdは更新していない。未実施の実機と全acceptanceはT1/ユーザー/Q1の再開条件として保持。
