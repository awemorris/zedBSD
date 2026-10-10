# WS202 i04/i05: bitsとAAC入力の部分結果（2026-10-10）

> main統合追記: [最新の統合記録](main-integration-20261010.md)。以下の独立worktree時点の未merge/未実装/pendingは履歴。標準readbackの旧offline p016は[ws202-p017](phase017/phase.md)へmapping修復し、mainのp016（ref-list）を維持した。

Parent: [WS202](ws.md)。承認/境界: [有限実行AAC入力](policy-20261010.md#有限実行-codex-ws202-20261010-aac-input)。独立worktree `.claude/worktrees/ws202-codex`、branch `codex/ws202-native-media`。mainのsource/共有計画/同期を変更していない。

## 結果と範囲

- i04（p003のbits部分）: **cleared**。MSB reader、read1、skip、align、left、ue/se、RBSP unescapeを自前実装。入力範囲外はsticky error、失敗したscalar fieldはcursorを進めない。ueのUINT32_MAXとseのINT32_MIN/INT32_MAXを含めて確認。
- i05（p005の入力metadata/Huffman部分）: **cleared**。ASC、PCE、ADTS header/block boundary、固定12bookのruntime、scalefactor差分、spectral tuple/sign/escapeを自前実装。非LC/明示HE-AAC/有効なSBR signallingはENOTSUP、今回読んだ構文の破損/不足はEINVAL。出力は成功時だけ公開する。
- p003/p005全体とWSは**未完**。AAC raw_data_block/ICS、FILの暗黙SBRの拒否、CRCの検算、scalefactor band/TNS表、PCM/filterbank、picture/sound、native backendへの接続、app fallback移管は未実装。現在の既存FFmpeg backendはまだ移管前であり、最終のnative-only境界を達成したとは扱わない。

実装source: `userland/desktop/libmedia/bits.[ch]`、`aac-input.[ch]`、`aac-codec.[ch]`。既存の`aac-huffman.[ch]`と共に`LIBMEDIA_SOURCES`へ通常登録。解析APIはprivateで、exports.mapを変更していない。新しい自前sourceはZlibで、外部の実装コード/探索構造は写していない。

## 設計の具体化

Huffman runtimeは旧手順の2段lookupに代えて、`pthread_once`で作る独自のprefix treeを採用した。最大289 leafのfull treeに577 node/book、12bookを静的に確保し、初回以後は読取りのみ。symbol範囲、prefix/重複、node上限を構築時に確認。読取りはleaf到達で止まり、paddingの先読みをしない。allocation/failure ownershipや公開API/他Phaseの依存は追加していない。これはp005内部の探索方式の具体化であり、再生性能の受け入れを代用しない。

PCEはfront/side/back/LFE、SCE/CPE/LFEのclass別tag、matrix index/pseudo-surroundを保持し、重複class/tagを拒否。標準channel configuration 7は8channel。protected ADTS multi-blockのpositionは最初のraw_data_blockからの相対値を絶対offsetへ変換する。unprotectedの後続blockはraw parserがID_ENDを読むまで位置未確定（offset 0）とする。

仕様の一次資料: [ISO/IEC 14496-3:2001のASC/GA/PCE](https://www.ossrs.net/lts/zh-cn/assets/files/ISO_IEC_14496-3-AAC-2001-7f4d0b3622b322cb72c78f85d91c449f.pdf)、[ISO/IEC 13818-7:2004のADTS/符号とescape/量子化値](https://ossrs.net/lts/zh-cn/assets/files/ISO_IEC_13818-7-AAC-2004-67b015c6ddfc9a4af83665738477124a.pdf)。前者GA table4.1/PCE table4.2、後者6.2/6.3、9、10.3。normative spectrumの最大絶対値8191にescapeを制限する。全文や組版を取り込んでいない。後方互換のSBR-absent LC拡張は受け入れ、有効なSBRは拒否する。

## host確認

Command: `sh plan/ws202/tests/run-host-aac-input.sh`（通常環境）。**plainとASan/UBSanがPASS**。CCはGCC14.2.0。compile flagsは`-std=c89 -Wall -Wextra -Werror -pedantic -O1 -g -pthread`、sanitizerは`-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`。`ASAN_OPTIONS=detect_leaks=1`、`UBSAN_OPTIONS=halt_on_error=1`。

試験は`host-aac-input.c`から実production sourceをlinkする。テスト専用のproductionスイッチ/代替decoderはない。output: own `build/tmp/ws202-aac-input.run.TQCTwN`、log `build/aac-input-host.log`。

確認内容:

1. unaligned32bit、境界/overrun/sticky、empty/null/bit count overflow、Golomb端点と不足suffix、RBSP chained prevention/in-place/不正byte/短いdestination。
2. 12book全1362symbolと最後のbitを欠いた符号語。4threadで初回lookupを同時実行し、once publication後の正確なsymbol/bit endpointを確認（TSanは未実施）。
3. 全11 spectral bookの独立した期待tuple（符号付きbook、非zeroだけのsign、係数順）、zero scalefactor/quad、signが両escapeに先行するpair、8191端点/過大escape拒否。
4. literal LC ASC、SBR-absent拡張、explicit frequency、config7、AOT5/29/後方互換SBR拒否、reserved frequency/short ASC。6channel PCEのclass/tag/group/matrix、重複tag、short comment、失敗時の出力保持。
5. literal ADTS、protected1〜4blockと相対position、short frame、nonmonotone position、非LC profile/bad sync。
6. host FFmpegのencoder（parser/decoderコードは参照しない）が独立に作る0.2秒のAAC-LC ADTS、44.1kHz stereo/48kHz mono。全frame headerを読み、宣言rate/channelとpacket終端の一致を確認。**payload decode/PCM/音質の確認ではない**。

## build/規約/exports

Named target:

```sh
make ZEDBSD_CONFIG=config/ci/config-amd64.mk \
  ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot \
  -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete \
  -j4 build/amd64/dynamic/libmedia.so
```

**exit0、warning/error0**。readonly共有sysroot/toolchainを利用し、build/install/patchしていない。LLVM23.1.0、own `build/libmedia-aac-input-build.log`。check-dynamic-elfのamd64/shared-library/NEEDED libc.so/SONAME libmedia.soがPASS。SHA-256: `41c6ebaa29ffd20eef21399a10bb25daf36e3433a9aefe4989909282ac803686`。

`llvm-readelf --dyn-syms`に新private prefix `media_bits_`/`media_rbsp_`/`media_aac_`のexport無し。通常symbol表ではLOCALとしてlink済み。libmediaにFFmpegが残っている現状の既存dlopen importを削除済みとは主張しない。

新6source/headerとhost Cを`clang-format-19`19.1.7（InheritParentConfig/ColumnLimit0）で整形し、規約が優先するdefinition argumentのtabを復元。`style-check.py ... --summary`: **total0**。全新Cの`cc -std=c89 -Wall -Wextra -Werror -pedantic -pthread -fsyntax-only ... -I.`: **PASS**。`git diff --check`: **PASS**。C全文の手動reviewでfile順、宣言位置、purpose paragraph、独立guard、return/error、once共有寿命、API/ライセンス境界も確認。WS最終p013 conformanceはこれでclearedにしない。

## 再開・統合

独立成果はbits/AAC構文を必要とする次のsourceから使えるが、他WSの依存はQ1がmainへ統合してから満たす。Q1のmainは`ee7df65dc`へ進み、WS202の第3版wsがcommit済み、designは別sessionの未commit。独立branchの旧設計を上書きしないよう、source/test-only patchと意味の統合が必要なWS/Phase記録を分けて提供する。merge/push/GitHub公開は未実施。

次のAAC実装はraw_data_block/ICS/tool構文と規格のband/TNS数値を追加し、FILのSBR検出を含めてから信号処理へ進む。H.264/app移管はまだ未完。実機・QEMU・音質・動画再生は未実施。
