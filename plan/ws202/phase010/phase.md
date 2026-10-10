<!-- awesome-plan project=zedbsd record=ws202-p010 -->

# ws202-p010: Vulkan Video の back end (2) 表示順・seek・失敗・表、media-probe の video、5330 の小さい確認

Status: uncleared（software実装あり、全条件の確認は未完）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 9 LW
依存: p009、p016、J10（5330 の道、仮: scp）、p017（標準NV12読み戻しのsource出力）

## 目的

p009 の back end を再生に使える形に仕上げて表の先頭に入れ、本物の GPU での最初の確かめを早く行う（review-001 M-09）。

## 成果

1. `vkvideo.c`:
   - 表示順と時刻（D22）: p008 の bumping で出る picture に、送った packet の pts を整列した列の最小を当てる（add-in と同じ）。待ち行列が満ちたら send は EAGAIN。drain。
   - p009 の D25 の stub を p016 の判定に置き換える。
   - **D29（M2-01、第 4 版で直した M3-02・L3-04、design §5.6）**: POC の分かる出ない picture（leading の picture、D25 で捨てた picture、result status ERROR の picture）は
     POC を持つ**空の entry** として表示順の待ち行列に入れ、bumping で出る時に整列の列の最小の pts を 1 つ消費して捨てる（receive は返さない）。POC の分からない AU（最初の
     I の前、parse の誤り、冗長 slice だけの AU）だけ pts を値で外す。pts は send が packet を受けた時だけ列に足す（EAGAIN の送り直しで二重に足さない）。
   - in-band だけの track（D16）: avcC に SPS が無い track は degraded 0 で FORMAT、degraded 1 で受け、最初の in-band の SPS・PPS と I まで session を作らず AU を捨てる。
     範囲の外の SPS は send が EINVAL（log 1 回）。session・image はその時に作る（D28 の例外）。
   - flush（design §5.7）: 待ち行列と pts を捨て、`h264_dpb_restart`、RESET、最初の I まで捨てる。
   - 失敗（design §5.9）: status ERROR（log の数え、連続 30 で EIO、参照なら欠けた参照として DPB に）。DEVICE_LOST・ETIMEDOUT は decoder の失敗と「壊れた」の印。ETIMEDOUT は
     `vkDeviceWaitIdle` が失敗したら decoder の object を壊さずに持つ。参照の数 0 で（壊れた時も通常の close でも）device と instance を壊し、次の open は新しい instance
     （J6 (a): libavcodec へは切り替えない）。
2. `decoder.c` の表を `{ &media_vkvideo_ops, &media_aac_ops, &media_avcodec_ops }` に（ws.md の merge の順。p006 が未 merge なら差分を Q1 へ）。
3. `userland/tests/media-probe/`（新、video の部分、design §10.3）: `main.c`・`Makefile`（vkvideo-probe と同じ形の package、`userland/base/common/sha256.c` を compile、
   **package の依存は `desktop/libmedia`**、L2-19）。`--video-hash`（1 行に pts（µs）と hash）・`--expect`（pts で行を合わせて pts と hash の両方を比べる。seek の後は最初に出た
   pts から、L2-01）・`--seek=S`・`--time`（open（instance・device・session・image）・decode・de-tile の時間）・`--twice`（同じ process で 2 つの decoder、L2-03）。1 行目
   `media-probe: video=<codec>/<backend>`。
4. `plan/ws202/tests/config-media.mk`（design §10.4）: `include config/ci/config-amd64.mk` と `ZEDBSD_USER_PROGRAMS := $(filter-out libavcodec,$(ZEDBSD_USER_PROGRAMS)) media-probe`。
   stream は config に書かない（`test-image.sh` が `ZEDBSD_TEST_EXTRA_FILES` を command line で渡し config の `+=` が消える、design §15 E7）。
5. host 試験 `run-host-vkvideo.sh` に足す: 表示順と時刻（`h264-high-b-aac.mp4`・`h264-nocts.mp4` で時刻が単調、数が AU の数と同じ）、**D29: 各 sync sample から seek した後の
   各 picture の時刻が参照（`.sha256` の pts）と一致、`h264-gap.mp4` で捨てた picture の時刻が消費され出た picture の時刻が元の stream と一致、`h264-nocts.mp4` の各 sync
   sample から seek した後の時刻が、通しの decode で同じ picture（hash で合わせる）が受けた時刻と一致（ffmpeg の参照を使わない、M3-02）、EAGAIN の送り直しで pts が二重に
   ならない**、EAGAIN、drain、flush の後の捨て方、
   in-band だけの track（`h264.ts` の packet で degraded 0 は FORMAT、1 は受けて最初の SPS まで捨てる）、status ERROR の数え、DEVICE_LOST の後の open が新しい instance を作る、
   close の後も picture が使える。
6. `plan/tools/media/run-host-codec.sh`（WS の外: 差分を Q1 へ）: `h264.{mkv,ts,avi}`・`h264-nocts.mp4` を足す。host では H.264 は vkvideo が DEVICE（`/lib/libvulkan.so` が
   無い）→ add-in で、既存の H.264 の試料（MPEG-TS を含む）が回帰しない。`h264.avi` は p002 で Annex B にした物（M2-08）。
7. T1 への小さい依頼（Q1 経由、M-09。道は J10 の仮の推し (b)、design §10.4）: 今の 5330 の image（libavcodec 有り）に scp で入れて流す。ユーザーの起動は要らない
   （2026-10-08 ユーザー「アップデートや再起動は自由に」）。T1 は ESP に書かない。
   - 送る物: 担当の build の `libmedia.so`（kei の home の `ws202/lib/` へ。system の `/lib/libmedia.so` は置き換えない。zedBSD の rtld は `LD_LIBRARY_PATH` を先に探す:
     `src/rtld/rtld.c` 1662〜1668）、`media-probe`、
     `plan/ws202/tests/streams/` の WS083 と同じ素材の 6 本の mp4・`h264-high-b-aac.mp4`・`h264-gap.mp4` と各 `.sha256`（kei の home の `ws202/` へ）。
   - SSH（kei、master.md の 5330 の運用の行の通り）で `LD_LIBRARY_PATH=$HOME/ws202/lib` を付けて: `media-probe --video-hash --expect=… …` の 8 本（1 行目が `video=h264/vulkan-video`。libavcodec が入っていても自前が表の
     先頭）、`media-probe --seek=2.5 h264-high-b-aac.mp4`、`--time` の行（U2、open の時間 L-01）、`--twice`。
   - (b) で確かめられない物（2 段目の試しの `h264.ts`・`h264.mkv`、DEVICE の notice、`--file` と `--mode` の置き場所 U12）は p012 の (a) に残す。
   - 結果を待たずに次の仕事へ移ってよい。FAIL は p009・p010（D25 なら p016）に戻る。open の時間が 200 ms を越えたら Q1 に報告（Video Player の open を media の thread へ移す
     仕事、design §9.3）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-vkvideo.sh` | p009 と p010 の項目が全部 PASS |
| `sh plan/tools/media/run-host-codec.sh` | PASS |
| design §10.6 の build（`ZEDBSD_CONFIG=plan/ws202/tests/config-media.mk`、media-probe を含む） | exit 0、`grep -c 'warning:'` が 0 |
| 5330（T1、scp） | 8 本の全 frame が pts と hash で一致（`h264-gap` は出た frame が元の stream と一致）、seek の後の frame が一致、`--twice` も一致、1 行目 `video=h264/vulkan-video` |


## 構造改訂と部分結果（2026-10-10）

POC/seekを維持し、表はnative backendだけ。app fallbackはここへ入れない。device不在はエラー。media-probeはapp fallbackなしで自前経路の証拠にし、標準readback未実装ならclearしない。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。

## Native再生software結果（2026-10-10）

Event: `ws202-native-playback-software-20261010-p010`。Queue: [codex-ws202-playback](../policy-20261010.md#自走の実行承認-codex-ws202-playback)。

native H.264 backend/reorder/cttsなしclock消費・空entry、in-band configuration/seek reset、native-only media-probeを実装。2session/seek/probeのEOF参照消費はhost stand-in確認。image config/build/実機入力は準備済み。actual hardware decodeはT1待ち。

[最終source/command/結果・限界](../playback-result-20261010.md)、[Q1統合](../handoff-20261010.md)、[T1の準備済み依頼](../t1-playback-request-20261010.md)。旧第2版の手順・昔のpartial outcomeを保存し、最新記録が未実装記述の現在状態を置換する。whole criteriaを満たしたとは扱わず、Q1の意味の統合と未実施matrix/実機結果が再開条件。main/共有投影/GitHubの更新はQ1 pending。


## main統合の追記（2026-10-10）

Event: `ws202-main-integration-20261010-p010`。ユーザー「mainへの統合はあなたがやってOKです。」によりsourceと記録をmainへ統合。最新の承認済み方針・手順・確認・残件は[統合記録](../main-integration-20261010.md)と[policy](../policy-20261010.md)。上の設計時点の推奨、旧未実装/統合pendingは履歴として保存する。software出力の有無とwhole clearanceを区別する。標準readbackの依存はp017、ref-listは既存p016。p012/T1→p013/User UAT→whole p014の確認は未実施、Master/共有Board/GitHubへの投影はQ1に保持。
