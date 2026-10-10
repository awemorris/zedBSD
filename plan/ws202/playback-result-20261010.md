# WS202: native再生のsoftware実装・検証結果（2026-10-10）

> main統合追記: [最新の統合記録](main-integration-20261010.md)。以下の独立worktree時点の未merge/未実装/pendingは履歴。標準readbackの旧offline p016は[ws202-p017](phase017/phase.md)へmapping修復し、mainのp016（ref-list）を維持した。

Event: `ws202-native-playback-software-20261010`。Owner: 独立Codex session、worktree `.claude/worktrees/ws202-codex`、branch `codex/ws202-native-media`。承認は[policyのcodex-ws202-playback](policy-20261010.md#自走の実行承認-codex-ws202-playback)。main/共有計画/他WS/同期/toolchainを変更していない。実装commitと統合patchは[Q1 handoff](handoff-20261010.md)の最新追記。

## 到達点

動画・音楽appがnative H.264/AAC-LCを使う経路、app所有の任意libavcodec fallback、標準Vulkan NV12読戻し、native-only media-probeを実装して対象build/host確認を終了した。**5330/i915の実decode pixels、動画プレイヤの表示・音・同期・性能は未確認。WSはincomplete、whole Phaseはclearedにしない。** Q1のmain統合後に[T1依頼](t1-playback-request-20261010.md)を実行し、最初の実機不一致を実装へ戻す。

既存の部分結果i01〜i05と失敗した確認の履歴は保存する。この記録は後続の結果であり、昔の「未実装」を過去時点の記録から消していない。

## 実装の事実

### Library / application boundary

- libmediaのbackend表は`media_vkvideo_ops`と`media_aac_ops`のみ。FFmpeg adapter/layoutは`userland/desktop/media-app/`へ移し、Video Player/Musicへ直接compileする。外部backendをlibmediaへ注入する口はない。
- native open/最初の未受理packetのcodec/profile/device/busy拒否だけをfallback理由にする。OOM/破損/受理後のGPU障害はfallbackで隠さず失敗として公開。任意codec無しの非対応GPUはDEVICEのまま返す。
- browserのengineはnative-only。decoder/read/seek/drain失敗をMEDIA_FAILEDとして公開し、seekによるresetまで失敗を保持する。browserの新しいsoftware fallbackやUATは範囲外。
- backend identity・trim・frame duration・SARは本番利用のAPI。probeだけの`media_decoder_*` APIを足していない。

### AAC-LC / common sound

- ASC/PCE/ADTS、SCE/CPE/LFE/DSE/FIL、long/short ICS、grouping、spectral/scalefactor Huffman、pulse、TNS、M/S・PNS・intensity、逆量子化、FFTによるDCT-IV/IMDCT、sine/KBD window/START/STOP/8SHORT overlapを独自実装。
- PCE/tagに合わせたchannel identityごとのoverlap、1〜8channelのstereo downmix、LFE除外。AAC couplingとgain controlは明示ENOTSUP。AOT5/29/有効なSBR/PSを拒否し、FIL SBRが有効blockの後にあってもsend時にpacket全体を拒否してPCMを出さない。
- 規格rate 7350〜96000Hzの入力。共通PCMは連続source clock、実following blockのlookahead、Kaiser β8.6・64tap/1025phase、rate変更でsegment reset、同rateはfilter bypass、出力8000〜192000Hz stereo16bit。source queueは16384frameでbounded、allocator失敗は次receiveまでsticky errnoを保持。
- MP4 edit-listのstart/end、seek後の1block overlap prerollとsource-grid trim、receive時に未変換部分を捨てる契約、最後の短いblockを扱う。
- Huffman/band/TNSの数値だけを固定FFmpeg9.0.2から抽出。元のarchive/member hashと再現生成・prefix/Kraft・roundtripの証拠は[policy](policy-20261010.md)と[AAC入力結果](aac-input-result-20261010.md)。外部のdecoder/探索/filterbank実装を取り込んでいない。

### H.264 / Vulkan Video

- WS083のZlib probeを元に、shared bits・incremental SPS/PPS・AU identity・全slice metadata・POC0/1/2・gap inferred entry・MMCO1〜6・logical/physical DPB・per-slice initial/modified P/B参照listとactive prefixのadmissionを実装。
- ITU-T H.264:2021の8.2.1/8.2.4/8.2.5とTable A-1を照合。type0 gapのnon-existing frameをB初期listから除外する規格補正、MMCO5のdecode時POCとmark後のnormalized historyの分離、High constraint_set3のreorder0推定を適用。詳細は[h264-progress](h264-progress-20261010.md)。欠落参照を架空のGPU imageにしない。
- leading non-intra seek pictureは前のepochのmarkingを適用せずdrop。全sliceのactive refsが揃うまでGPUへsubmitしない。MMCO5/IDRのoutput epochをdrainしてから次へ。cttsなしMP4ではPOCの表示順でpacket clockのmultisetを消費し、既知POCのdropは空entryで時刻を消費する。
- 標準のinstance/device/queue/profile/level/ref capacity/combined NV12 usageをquery。COINCIDEを優先、必要ならDISTINCT。GPU名・PCI ID・Intel Tile Y・private optimal layoutをlibraryに置いていない。
- HOST_VISIBLE buffers（coherent優先、必要なflush/invalidate）、queue family別poolとCONCURRENT image sharing、SYNC2 video/transfer/host barriers、binary decode→transfer→next decode、transfer fence後のplane sample単位コピー、CPU側cropを実装。
- timeoutではinflight ownerを保持。closeはdevice retirement後に解放し、retirement失敗かつDEVICE_LOSTでない状態ではGPU資源ownerを隔離保持。allocation/dispatch/query失敗をfalse completionやunsupported fallbackへ変換しない。
- i915のNV12 TRANSFER_SRC admissionとCopyImageToBuffer plane bounds/de-tileはdriver所有。HAL API/UAPIは不変。libvulkanの標準wireでplane情報を保持する。VCS/RCS retirement・cache・failed DMA lifetimeは既存契約を維持（p016のsoftware証拠）。

### Pictures / containers / applications / probe

- reference-counted CPU NV12 pool（free cacheだけ上限、checked-outはallocatorの限り取得）、decoder close後に保持するframe/scaler、odd NV12 size、6色行列/範囲、horizontal mapping/2converted-row cacheのbilinear scaler、SARを実装。
- MP4のpasp、colr nclx/nclc、edit-list end_us、cttsなしflagをtrack末尾に追加。既存track prefixは保持、内部library/appを一緒にrebuild。
- Video Playerはcodec/backend別OPEN、SAR fit、seek trim、runtime failure notice、pause/end/closeのFRAMES/time_ms/late。ENDは音ringの消費を待つ。Musicもnative backend log・seek preroll/trim・失敗通知を実装。
- `userland/tests/media-probe/`は実際のlibmedia.soだけをlink。visible CPU NV12のPTS/SHA-256、48kHz stereo PCMの1024frame RMS、seek、同時2decoder、open/総処理時間を提供。参照の未消費末尾と空outputはfailure。GPU実行時間とtransfer時間を別々に測ったとは称さない。
- 新probeのpackage/amd64のlibmedia link recipeとimage選択を追加。英語利用者文書は`docs/reference/media-playback.md`。source-ownedの2秒fixtureと12秒open GOP UAT/参照を[tests/streams](tests/streams/README.md)へ保存。

## 最終確認（2026-10-10 JST）

Host: Debian13/GCC14.2.0、FFmpeg7.1.5-0+deb13u1、libavcodec61。target compiler: readonly共有LLVM23.1.0。formatter: clang-format19.1.7。全対象host compileはC89/Wall/Wextra/Werror/pedantic（target APIの64bit値に必要なWno-long-long）、ASan/UBSan/LSanは通常環境で実行。sandboxのLSan ptrace制約は同じrunnerの通常環境実行で解決し、LSanを無効にしていない。

| Command | 結果 / 実際に証明する範囲 |
| --- | --- |
| `sh plan/ws202/tests/run-host-aac-native.sh` | plain/ASan/UBSan/LSan PASS。5独立AAC素材、ASC raw/ADTS出力一致、明示/暗黙HE-AAC拒否、continuous resampling |
| `sh plan/ws202/tests/run-host-native-playback.sh` | plain/ASan/UBSan/LSan PASS。production container/decoder/app + **GPU execution stand-in**。50pictures/96000PCM、seek1.25/0/1.999、no-FF/device拒否、no-ctts、frame lifetime、runtime standard-command契約、POC/gap/MMCO5/list、4slice独立stream、probe音RMS/2session/seek |
| `sh plan/ws202/tests/run-host-app-fallback.sh` | plain/ASan/UBSan/LSan PASS。native videoのadmissionだけをDEVICE stand-inで拒否。**実libavcodecのdlopen**による50H.264 pictures＋native AAC96000frame、seek、software pictureのclose後scale |
| `build/tmp/ws202-native-playback/host-h264-plain plan/ws202/tests/streams/h264-uat.h264` | 300pictures/211B、drop0。12秒open GOP/B pyramid/4referencesの**parser/DPB**確認、hardware pixelsではない |
| native host probeへ50picture参照の後に余計な期待行を追加 | exit1/EIO。合うprefixだけをPASSにしない |
| `cd plan/ws202/tests/streams && sha256sum -c SHA256SUMS` | 固定source入力と参照が一致 |
| `git diff --check` | PASS |

AAC float参照比較: stereo73.24dB、mono75.17dB、low85.64dB（PNSの乱数差を含む）；PNS無しtransient136.56dB/intensity135.67dB、max≦2^-14/RMS≦2^-17の基準PASS。resamplingの997Hzは8k/44.1k/96k入力で期待RMSの相対差0.1%以内、96kの30kHz alias RMS<3e-5。native probeの独立16bit RMS参照は94blockで最大絶対差9.41e-7、閾値を緩めていない。

RMS参照の整合: monoをFFmpegの既定-3dB stereo変換でなくnativeと同じ左右同値にし、FFmpegが残すencoder paddingにMP4の2秒終端を適用した。最終native出力は96000frame、終端partial blockは768frame。この参照条件の修正をnative decoderの障害修正として扱わない。

### Named build

```sh
make ZEDBSD_CONFIG=plan/ws202/tests/config-media.mk \
  ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot \
  -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete \
  -j4 build/amd64/dynamic/libmedia.so build/amd64/dynamic/libvulkan.so \
  build/amd64/dynamic/libbrowser.so build/amd64/bin/videoplayer \
  build/amd64/bin/music build/amd64/bin/media-probe \
  build/amd64/kern64/src/drivers/gpu/i915/render/command.o \
  build/amd64/kern64/src/drivers/gpu/i915/render/image.o \
  build/amd64/kern64/src/drivers/gpu/i915/render/instance.o \
  build/amd64/kern64/src/drivers/gpu/i915/render/video.o
```

**exit0、warning0/error0**。通常check-dynamic-elfもPASS。libmedia.soのNEEDEDはlibc.soだけ、SONAME libmedia.so；Video Player/Music/probeに直接のlibavcodec NEEDED無し。任意FFmpegはappのdlopen。共有sysrootの完了stampはread-onlyの既存完了を明示しtoolchain再buildを抑止した。image/bootの結果ではない。

### 規約確認

正本AGENTS.md、Guardrail、`plan/coding-style.md`全文890行/§14を適用。新規・変更scope65 C/headerを確認し、file/function順、static宣言、C89先頭宣言、argument tabs、public/static header comments、ownership/immutable table、guard/call/error分離、immediate allocation check、semantic paragraph、return、同期/retirement、native/app/license境界をreview。formatter19.1.7のInheritParentConfig/ColumnLimit0を用い、規約に優先するargument tab/コメント区切りを復元。特殊なparser/array処理はcompiler/hostに加えてmanual review、formatterだけで意味適合を主張していない。

`python3 plan/tools/style-check.py <build/ws202-final-c-scope.txtの各path> --summary`は**49候補でexit1**。全49は基点`0bc3d4d0f`にあるi915既存codeの候補と同じ（command4、gfx.h1、instance2、video42）。新native/app/probe/host scopeは0。i915旧codeの無関係な一括整形はしていない。現scopeのbuild/host PASSと既存baselineを分けて記録し、whole p014規約/登録はQ1/UAT後のベータ3作業として未完に保持。

## 証拠 / 残件

実行logはown `build/ws202-aac-native-final.log`、`build/ws202-native-playback-final.log`、`build/ws202-app-fallback-final.log`、`build/ws202-final-build.log`、`build/ws202-uat-parser.log`、`build/ws202-final-style.log`、`build/ws202-style-base.log`。runner/source fixtureをcommitし再実行可能。ローカル未公開の証拠でありGitHub同期済みとは称さない。

- 実機pixels/retirement/cache、A/V同期・音・window操作、2physical session、GPU復帰/BUSY、QEMU image/notice、性能/全UATは未実施。T1/Q1/Userへ残す。
- 全Phaseのfixture matrixは未達: WS083の6MP4比較、encoded crop/SAR、missing-packet本物のdecode、ITU conformance bitstream、AAC multichannel/PCEのPCM精度・band別PNS、TSan、container全回帰、1080p30/zgears。既存計画の義務を削除せず、wholeをunclearedにする。
- ASO/redundant coded pictures、coupling/gain controlは明示拒否。TSの1PES内複数AUの各PTS、BT.2020 constant-luminance固有の変換、入手したconformance bitstreamの復号は未確認/未完。通常の1packet1AU・非constant-luminance係数に対するsoftware証拠だけ。
- `plan/tools/media/run-host-codec.sh`/host-layoutは旧libmedia/avcodec pathを指す。共有toolの移管追従/新runner登録はQ1担当、ここでは旧runnerをPASSと報告しない。対象の実app fallbackは新host runnerで確認した。
- AAT helper/scenarioへの追従は[aat-native-proposal.patch](aat-native-proposal.patch)（共有fileには未適用）。Q1が依存登録とともに適用する。
- p016 offline ID衝突（main=ref-list、own=standard readback）を両方保存。Q1がmappingを解決し、最新第4版へpolicy/resultを意味で統合。Master/Queue/Guardrail/GitHub projectionはpending。

## カーネルの最終link確認（2026-10-10）

同じconfig/read-only sysrootのnamed target `build/amd64/vmunix`もexit0、warning0/error0。通常の`check-kernel-includes.noct`と`check-amd64-vmunix.noct`がPASS、kernel全体のlinkを確認した。初回はworktreeに既存Noct toolの参照が無くpost-linkでexit2（`build/NoctLang/build-static/noct: not found`）。許可されたread-only共有利用としてown `build/NoctLang`をmainの既存locked NoctLangへsymlinkし、同じtargetを再実行した。toolchainをbuild/install/patchしていない。image/起動は未実施。

vmunix SHA-256: `e3111503e4fa59e43184a966e8de538c89ce642a66ae8e427537bc2d3c4ebe95`。最終kernel build log SHA-256: `679b1134196fe460fc998ef03e01a2ef47127125a81f14dc3ef5489bc547cb39`。
