# WS202 native AAC・共通音声: 継続中の証拠（2026-10-10）

> main統合追記: [最新の統合記録](main-integration-20261010.md)。以下の独立worktree時点の未merge/未実装/pendingは履歴。標準readbackの旧offline p016は[ws202-p017](phase017/phase.md)へmapping修復し、mainのp016（ref-list）を維持した。

Parent: [WS202](ws.md)、承認/active Queue: [codex-ws202-playback](policy-20261010.md#自走の実行承認-codex-ws202-playback)。i06/i07を継続。動画プレイヤでのnative H.264+AAC-LC再生が目標であり、以下の部分成果で自走を終了しない。

## 実装と設計の具体化

- `aac-frame.[ch]`: 原著のLC raw_data_block、SCE/CPE/LFE/PCE/DSE/FIL/END、ICS/長短window grouping、sections/scales/spectral/pulse/TNS。common_windowなしのCPEを独立に扱い、intensityはcommon_window必須。ms_mode=2のintensity極性をms_mode=1と混同しない。FIL拡張境界を読んでSBR 13/14をENOTSUPにする。CCE・予測・gain controlは再現せずENOTSUP（音を黙示的に省略しない）。これらのnative非対応はapp-level fallback対象になる。CRCフィールド/複数raw blockの境界を読むがchecksumは未検算。
- `aac-synth.[ch]`: 原著のinverse quant/PNS/M/S/intensity/TNS、radix-2 FFTによるDCT-IV/IMDCT、sine/KBD、long start/stop/eight short/overlap-add。数式だけから不変表をpthread_onceで構成。最初のTNS係数の符号誤りを実過渡音で再現し、ISOの式の正のsinへ修正した。外部decoder実装を読んだり写していない。
- `aac-bands.[ch]` / `gen-aac-bands.py`: 承認されたnumeric-only方針で規格band offset/count/TNS limitのliteral整数だけ抽出。入力はHuffmanと同じ固定FFmpeg9.0.2 archive/member/hash。関数/float近似/探索構造/外部commentを抽出しない。13周波数の始終点、増加、alignment、件数を検証。
- `sound.[ch]`: decoder所有の連続stereo source queue、rational clock、sample trim/end、D23のreceive後寿命。実次frameをlookaheadとして保持しpacket境界でzero paddingしない。Kaiser beta8.6、64tap（downsamplingは比で拡大）、1024phase+線形補間、rate-dependent cutoff。rate tableをdecoderごとに一度作ってsegment間再利用。same-rateはsincを通さず飽和/16bit量子化だけ。tableをglobal cacheするのは共有の可変状態を増やすため採らず、普通の技術選択としてprivate lifetimeを保持。
- `aac.c`: native `media_aac_ops`。全packetを構文検査してから公開（packet末尾SBRでも先のLCを音にしない）。ADTS複数block・ASC raw packet、class/tag/sideでoverlap保持、普通のconfigurationとPCE speaker groupから3〜8ch stereo downmix/LFE除外、native receive/drain/flush/pre-roll/trim。packet当たり64block上限。AAC sourceはlibmedia build登録したが**decoderのtable/appへの移管はまだ途中**で、既存FFmpegは後続でlibraryから外す。

## 一次資料

[AAC ISO/IEC 13818-7:2004](https://ossrs.net/lts/zh-cn/assets/files/ISO_IEC_13818-7-AAC-2004-67b015c6ddfc9a4af83665738477124a.pdf)の12.2 intensity、14.3 TNS、15.3 filterbankと、[ISO/IEC 14496-3:2001](https://www.ossrs.net/lts/zh-cn/assets/files/ISO_IEC_14496-3-AAC-2001-7f4d0b3622b322cb72c78f85d91c449f.pdf)のLC syntax/PNSを確認。外部decoderのコードの移植ではない。

## 対象host・build

`sh plan/ws202/tests/run-host-aac-native.sh`（ASan/UBSanが通常動作するhost環境）: PASS。

独立合成信号をhost FFmpegでAAC-LCへencodeし、ADTSを同じhostでreference decode。nativeはproduction sourceの直接compileを呼び、外部decoder内部に依存しない。

| Fixture | raw frames / tools | native/reference PCM |
| --- | --- | --- |
| stereo44.1k | 53、short/PNS/MS | length一致、SNR73.24dB（PNS乱数差を含む） |
| mono48k | 58、short/PNS | length一致、SNR75.17dB |
| stereo22.05k | 27、short/PNS/MS | length一致、SNR85.64dB |
| transient48k | 58、short36channel-frames、TNS3、PNSなし | SNR136.56dB、max差1.49e-7 |
| intensity48k | 58、intensity/MS/short、TNS4、PNSなし | SNR135.67dB、max差2.39e-7 |

plain/ASan/UBSanで5サンプルの実raw payloadとPCMを確認。explicit HE-AAC ASCと「先頭は完全なLC、同一packet末尾FILでimplicit SBR」のatomic拒否を確認。ASC raw/ADTS backendは同一出力（58*1024 sample stereo48k）、drainで取り残しなし。

resampling: 8k/44.1k/96k→48k、出力sample数=ceil(source_count*output_rate/input_rate)。997Hzの通過域は±0.001比率以内。96kの30kHz toneのaliasは16bit量子化後RMS0（物理的な無限attenuationとは主張しない）。全帯域sweep/TNSの全order/全profile/multi-channel/Music-seek/PNS bandごとの比較は未実施。

標準: 全文`plan/coding-style.md`を適用。clang-format19（ColumnLimit0）後にargument TABと意味のparagraphを保持、対象style-check candidates0。C89 -Wall/-Wextra/-Werror/-pedantic（platform型のlong longを明示許容）でhost compile。新generatorは`--check`一致。変更後named libmedia build PASS・warning0、`git diff --check` PASS。WS全体の最終conformanceを済んだ扱いにはしない。

build command: `make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j4 build/amd64/dynamic/libmedia.so`。shared sysroot/toolchainはreadonly、own成果だけ作成。

## 残件・再開

H.264 parser/DPB/ref lists/Vulkan Video/native table、picture pool/scaler、container end_us/SAR/colour、app-only FFmpeg移管、Video/Music seek/notice/failure伝播、最終source規約/build/対象host、Q1 patch、実機受入。H.264 physical readback/hashは未確認、WSはincomplete。own WSへの投影は更新、main/共有Board/GitHubはQ1へpending。
