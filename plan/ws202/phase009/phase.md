<!-- awesome-plan project=zedbsd record=ws202-p009 -->

# ws202-p009: Vulkan Video の back end (1) device・session・decode・読み出し

Status: uncleared（software実装あり、全条件の確認は未完）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 8 LW
依存: p015（slot を持つ entry だけを積む計画の形）、H1、J6。D25 の判定の口は p016 と並べて作り、p010 で両方を合わせる（L3-06）、p017（標準NV12読み戻しのsource出力）

## 目的

H.264 の picture を Vulkan Video で decode し、出力を linear の NV12 の picture にする back end の中身を作る（design §5.1・§5.4・§5.5）。表示順・seek・作り直し・表は p010。

## 成果（`userland/desktop/libmedia/`）

1. `vkvideo-device.c`（D8・D9）:
   - **`dlopen("/lib/libvulkan.so")`**（image の path、名だけにしない: host で Mesa を読まないように。L2-07）、dlclose しない。`vkGetInstanceProcAddr`・`vkGetDeviceProcAddr` から `struct vkvideo_functions`。
   - 共有の device を **mutex と参照の数**で遅延に作る（`pthread_once` を使わない）。instance は **apiVersion `VK_API_VERSION_1_0`** と instance の拡張
     `VK_KHR_get_physical_device_properties2`（`vkvideo-probe/main.c` の `probe_instance`）。queue family は `vkGetPhysicalDeviceQueueFamilyProperties2KHR`（KHR の名で引く）と
     `VkQueueFamilyVideoPropertiesKHR`（`probe_video_family`）。device は 4 つの拡張（`probe_device`）。
   - 「video が無い」を覚える（instance を壊しても残す）。「壊れた」の印（p010 で使う）。queue の mutex。
   - **参照の数が 0 になったら、いつも device と instance を壊す**（M2-06・L2-20: kernel の video・render の context は kernel session の close でだけ外れる、design §15 E13）。
2. `vkvideo.c` の decoder の state と decode:
   - **open（D28、M2-02）**: parser の用意、avcC の SPS があれば `h264_check_sps`、共有の device の参照、続けて capability（`probe_capabilities`）と D20（大きさ・MB の数・
     参照の数・slot の数。level では断らず、Vulkan に渡す level_idc は design §5.4 の表で列挙へ写し `maxLevelIdc` に丸める）、D21（slot の image の合計 256 MiB）、session と
     memory（`probe_session`）、parameters（`probe_parameters`）、slot の image（`probe_picture`）、bitstream の buffer（`probe_bitstream`）、command・fence、status の query pool
     （`probe_status_pool`）まで作る。BUSY・PROFILE・ENOMEM は open の問題になり、libavcodec があれば表の次へ回る。open は window の thread で走る（L-01: 時間は p010 で測る）。
     avcC に SPS の無い track（D16）は session 以下を最初の in-band の SPS の時に作る（p010）。
   - 1 picture の decode（`probe_decode` を手本）: D25 の判定の口（p016、並べて作る間は「いつも decode」の stub）で「decode」の picture だけ。参照に積むのは slot を持つ entry だけ。slice の写し（32 byte 揃え）、記録、queue の
     mutex の中で submit、fence（5 秒）、status。
   - 読み出し（D10）: memory type が HOST_COHERENT でなければ先に `vkInvalidateMappedMemoryRanges`（D24、L2-18。zedBSD は COHERENT）。PLANE_0・PLANE_1 の subresource layout で Tile Y を de-tile し、crop の窓を pool の buffer へ。SAR（VUI → pasp）・色（VUI → colr → 既定）。
   - D6: parameters の作り直し。同じ track の中の in-band の SPS の変化（IDR での大きさ・profile・参照の数）で session・image の作り直し（L2-10）。
   - session の作成の失敗の分け方（U10 は閉じた、L3-01・L3-07、design §5.4）: **`vkCreateVideoSessionKHR` の `VK_ERROR_OUT_OF_DEVICE_MEMORY` だけ**を BUSY に写す
     （`render/video.c` 1077・1082〜1088。`vkAllocateMemory` の同じ結果は本物の memory 不足で ENOMEM）。`vkCreateVideoSessionKHR` の `VK_ERROR_INITIALIZATION_FAILED`
     （hang・quarantine、1044〜1049）は DEVICE にするが「video の無い機械」の覚えを立てない。libvulkan が renderer の結果を変えずに返すことを `objects.c` で確かめる（U21）。
     session は open で作るので、これらは open の問題。
   - 2 段目（degraded 1）では avcC に SPS のある track を試さず FORMAT を返す（1 段目の BUSY・PROFILE を繰り返さない。decoder.c は D27 で FORMAT でない方を返す）。
3. ops（`media_vkvideo_ops`、backend "vulkan-video"、codec "h264"）: open・send・receive（この Phase は decode の順）・picture・flush・close、picture 系は p003 の関数。
   表にはまだ入れない。
4. U11: level_idc の丸めが Vulkan の仕様の VUID に触れないかを仕様で確かめ、phase.md に記録（触れるなら丸めずに断るか、SPS の level を変えずに渡すかを Q1 に報告）。
5. host 試験 `plan/ws202/tests/run-host-vkvideo.sh`: 偽の Vulkan の関数の表。instance の apiVersion が 1.0 で properties2 の拡張が有効、properties2 の KHR の関数で family を問う、
   **open で** session・image まで作る、session の作成の失敗が open の BUSY・D20・D21 が open の PROFILE、作る object の順と引数、setup・参照の slot（slot を持たない参照を
   積まない、D25 で捨てる picture は submit しない）、32 byte 揃え、de-tile・crop が模様と一致、parameters の作り直し、video の無い device で DEVICE、通常の close で参照の数 0 なら
   device と instance を壊す、本番の `dlopen("/lib/libvulkan.so")` が host で失敗して DEVICE（偽の表を入れない回）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-vkvideo.sh` | PASS（ASan/UBSan） |
| `sh plan/ws202/tests/run-host-h264.sh` | PASS |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |

## 注意

- 偽の Vulkan は本番の dlopen の道を通らない。本物は p010 の終わりの 5330 の小さい確認で。実装の担当は QEMU・実機を起動しない。
- libvulkan・i915 を変えない。疑いは Q1 に報告。


## 構造改訂と部分結果（2026-10-10）

Vulkan queryで選び、機種名/i915の固定判定をしない。Tile Yをlibmediaで読まない。標準NV12 readbackのp016出力を追加依存とする。COINCIDE/DISTINCT・format usageも能力を問い合わせる。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。

## p016の出力を受ける具体的変更（2026-10-10）

標準Vulkan Video queryでNV12のDECODE_DST/必要なDPBとTRANSFER_SRCを要求する。decode queueとは別にgraphics/transfer queueを選ぶ場合、共有する出力imageは標準CONCURRENT sharing（両familyを列挙）または正しいownership transferとする。decode fence/status成功後、PLANE_0のextentはwidth/height、PLANE_1はceil(width/2)/ceil(height/2)、format compatible texelは1/2 byte。両planeのVkBufferImageCopyをHOST_VISIBLE staging bufferへ記録し、transfer fenceと必要なinvalidate後にlinear NV12だけをpictureへ渡す。offset/rowLengthをsample単位で計算し、容量/overflowを確認。libmedia内Tile Yとprivate subresourceLayoutの読取りは廃止。対応がないdeviceは明示DEVICE/PROFILE、app側adapterがfallbackする。

ユーザー承認のdriver/libvulkan補完はp016でsoftware確認済み、実機画素は未確認。p009自体は未実装/未実行。H.264のreorder/seek/timeout結果、DPB再利用前copy退役はp010を含めて確認する。

## H.264規格照合の設計補正（2026-10-10）

Event: h264-reference-admission-20261010。i08を実行開始。[規格照合と影響](../h264-progress-20261010.md)（Phaseからは [../h264-progress-20261010.md](../h264-progress-20261010.md)）。POC type0のgap推定non-existing frameはB slice初期参照listから除外する。p008はgap/POC metadata、p015は全sliceのlogical/real参照list照合、p009はその判定に基づくdecode admissionを補正する。第4版referenceは保存。software/実機clearanceはまだない、Q1共有projection pending。

## Native再生software結果（2026-10-10）

Event: `ws202-native-playback-software-20261010-p009`。Queue: [codex-ws202-playback](../policy-20261010.md#自走の実行承認-codex-ws202-playback)。

標準能力/level/complete usage照会、COINCIDE/DISTINCT resources、2queue semaphore/barrier/fence、HOST_VISIBLE readback、retirement-aware cleanupを実装。標準command stand-inの契約を確認しallocation/query errno伝播を改善。実機GPU pixels/loader経路/退役は未確認。

[最終source/command/結果・限界](../playback-result-20261010.md)、[Q1統合](../handoff-20261010.md)、[T1の準備済み依頼](../t1-playback-request-20261010.md)。旧第2版の手順・昔のpartial outcomeを保存し、最新記録が未実装記述の現在状態を置換する。whole criteriaを満たしたとは扱わず、Q1の意味の統合と未実施matrix/実機結果が再開条件。main/共有投影/GitHubの更新はQ1 pending。


## main統合の追記（2026-10-10）

Event: `ws202-main-integration-20261010-p009`。ユーザー「mainへの統合はあなたがやってOKです。」によりsourceと記録をmainへ統合。最新の承認済み方針・手順・確認・残件は[統合記録](../main-integration-20261010.md)と[policy](../policy-20261010.md)。上の設計時点の推奨、旧未実装/統合pendingは履歴として保存する。software出力の有無とwhole clearanceを区別する。標準readbackの依存はp017、ref-listは既存p016。p012/T1→p013/User UAT→whole p014の確認は未実施、Master/共有Board/GitHubへの投影はQ1に保持。
