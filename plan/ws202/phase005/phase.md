<!-- awesome-plan project=zedbsd record=ws202-p005 -->

# ws202-p005: AAC の構文と表

Status: uncleared（software実装あり、全条件の確認は未完）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 8 LW
依存: p002（stream）、p003（`bits.c`）、H3（HE-AAC）、H5（表の出典）

## 目的

AAC-LC の frame（ASC か ADTS の入力）を全部の field まで読み、channel ごとの量子化値・scalefactor・道具の data を持つ形にする（信号処理は p006）。

## 成果（`userland/desktop/libmedia/`）

1. `aac.h`: `struct aac_config`（AOT、core の rate、channel の配置、HE-AAC の印（design §6.1 の (1) 階層の明示・(2) 後方互換の明示）と拡張の rate、ADTS かどうか）、
   `struct aac_ics`、`struct aac_element`、`struct aac_frame`。
2. `aac.c`（構文）:
   - `aac_config_parse`（ASC: AOT の escape、rate の 24 bit の明示、GASpecificConfig、PCE、AOT 5・29、0x2b7 と sbrPresentFlag と 0x548）。用語は design §6.1 の通り
     （0x2b7 は**後方互換の明示**、暗黙は ASC に何も無く FIL だけ）。
   - `aac_adts_parse`（syncword、profile、rate の index、channel、protection_absent と CRC、number_of_raw_data_blocks_in_frame（最大 4 block、L2-08）、frame の長さ）。
     最初の packet で config を作り、以後は形の一致を検べる。profile が LC でなければ EINVAL（open の後なので libavcodec へは回らない、制限、L2-09）。
   - D26（仮、J7 (a)）の判定の関数: ADTS の track と、ASC が LC で core の rate が 24 kHz 以下の track は degraded 0 で FORMAT（p006 の open が使う）。
   - `aac_frame_parse`（raw_data_block、design §6.2）。ADTS の複数の block は block の前の CRC を読み飛ばす。
   - ICS・section・scalefactor（global_gain から DPCM、intensity と PNS の別の DPCM、PNS の最初の 9 bit）・pulse・tns・spectral（ESC）、short の group の並べ替え。
3. `aac-tables.c`（H5 の回答に従う）:
   - (a) の時: `plan/ws202/tests/gen-aac-tables.py`（`python3 -I`）が `build/distfiles/ffmpeg-9.0.2.tar.xz`（main の checkout の物を読むだけ。無ければ package の
     Makefile の URL から取得して SHA-256 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e` を確かめる）の `libavcodec/aactab.c` から、
     `codes1`〜`codes11`・`bits1`〜`bits11`・`ff_aac_scalefactor_code`・`_bits`・`swb_offset_1024_*`・`swb_offset_128_*`・`ff_aac_num_swb_1024`・`_128`・
     `ff_tns_max_bands_1024`・`_128`（design §15 E6）の**値だけ**を読み、zedBSD の名前・並び・型で `aac-tables.c` を書く。頭に出典（file、tarball の SHA-256、
     「values only, no code copied」）。`--check` で tree の物と一致を確かめる。
   - (b)・(c) の時: その出典から同じ形で。
   - Huffman の復号の 2 段の表は init で `pthread_once` で作る（design §6.9）。
4. host 試験 `plan/ws202/tests/run-host-aac-parse.sh`:
   - 全 AAC の stream（`aac-adts.ts` を含む）の全 frame で失敗 0、読み終わりの bit が packet と一致、複数の block の ADTS（手で 2〜4 block を連ねた試料）、D26 の判定
     （ADTS・22.05 kHz の LC は degraded 0 で FORMAT、44.1・48 kHz の LC は受ける）、要素と道具の数え（`aac-short` に EIGHT_SHORT、`aac-is-pns` に
     PNS・intensity、`aac-ms-tns` に ms_used・TNS）。
   - Huffman の表の自己の検査（prefix 符号、Kraft の和）。
   - HE-AAC の signalling（M-11）: `gen-asc.py`（`python3 -I`）が `aac-lc-stereo-44k.m4a` の esds の ASC を (1) AOT 5 の階層の明示、(2) AOT 2＋0x2b7＋sbrPresentFlag 1 に
     書き換えた file を `build/` に作る。`aac_config_parse` が HE-AAC と読み、core の rate・AOT が元と同じ。degraded 0 の open は断り、1 は受ける（H3 (a) の時）。
   - 2 thread の同時の初めての open（TSan、表の `pthread_once`）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `python3 -I plan/ws202/tests/gen-aac-tables.py --check`（H5 (a)） | 一致 |
| `sh plan/ws202/tests/run-host-aac-parse.sh` | 全部 PASS（ASan/UBSan、TSan の回） |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |

## 注意

- H5 の回答の前に始めない。この Phase では back end を表に入れない。
- ISO/IEC 14496-3 の表の番号は未確認（design U6）。生成の file の comment は「ISO/IEC 14496-3 の Huffman の codebook と scalefactor band の表」とし、番号を書くのは
  規格で確かめてから。


## 構造改訂と部分結果（2026-10-10）

12 Huffman数値の生成器/aac-huffman.[ch]を実装・構造/生成一致/C89確認PASS。p005全体は未完としてuncleared。H5はユーザー承認済み。AOT5/29/SBR/PSは拒否し、degraded=1でcoreを受ける旧手順は失効。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。

## 2026-10-10 i05のpartial scope

[有限実行AAC入力](../policy-20261010.md#有限実行-codex-ws202-20261010-aac-input)でASC/ADTS/PCEと既存12bookのruntime復号を続ける。HE-AAC拒否とPCM未実装を明示し、p002の全fixture/p003のpicture/soundに依存するwhole raw_data_blockは未着手として保存。

## 2026-10-10 i05部分結果

i05の具体partial scopeをclearedとして終了。[source/設計の具体化・host/build・C全文review・制限](../aac-input-result-20261010.md)。ASC/ADTS/PCE metadataと独自Huffman runtimeは通常libmedia buildへ登録。探索は2段lookupからonce初期化のbounded prefix treeへ具体化した（Phase内部、API/依存の追加無し）。raw_data_block/ICS/tool構文、FILのSBR検出、CRC検算、残る規格表/PCMは未実装。 p005全体をclearedとしてcloseしない。Q1への統合・共有projectionはpending。

## 2026-10-10 自走実装の進捗

ユーザーの動画プレイヤで再生可能になるまで自走する指示により、[codex-ws202-playback](../policy-20261010.md#自走の実行承認-codex-ws202-playback)を継続中。旧degraded/LC-core-only/FFmpeg-library-backendの手順は適用しない。[AACの実PCM・共通音声の途中証拠](../aac-native-progress-20261010.md)を保存。whole Phaseのclearanceではなく、app/H.264/end_us/seek等の未完criteriaを保持する。独立sourceのみ変更、Master/共有Queue/他担当投影はQ1へpending。

## Native再生software結果（2026-10-10）

Event: `ws202-native-playback-software-20261010-p005`。Queue: [codex-ws202-playback](../policy-20261010.md#自走の実行承認-codex-ws202-playback)。

raw_data_block/ICS/tool構文、band/TNS数値表、atomic FIL SBR拒否を実装。旧i05未実装残件のうちraw parser/暗黙SBRは解消。CCE/gain controlは明示拒否。PCE全metadataは既存入力試験、全multichannel PCM/TSan/CRC検算matrixは未達。

[最終source/command/結果・限界](../playback-result-20261010.md)、[Q1統合](../handoff-20261010.md)、[T1の準備済み依頼](../t1-playback-request-20261010.md)。旧第2版の手順・昔のpartial outcomeを保存し、最新記録が未実装記述の現在状態を置換する。whole criteriaを満たしたとは扱わず、Q1の意味の統合と未実施matrix/実機結果が再開条件。main/共有投影/GitHubの更新はQ1 pending。


## main統合の追記（2026-10-10）

Event: `ws202-main-integration-20261010-p005`。ユーザー「mainへの統合はあなたがやってOKです。」によりsourceと記録をmainへ統合。最新の承認済み方針・手順・確認・残件は[統合記録](../main-integration-20261010.md)と[policy](../policy-20261010.md)。上の設計時点の推奨、旧未実装/統合pendingは履歴として保存する。software出力の有無とwhole clearanceを区別する。標準readbackの依存はp017、ref-listは既存p016。p012/T1→p013/User UAT→whole p014の確認は未実施、Master/共有Board/GitHubへの投影はQ1に保持。
