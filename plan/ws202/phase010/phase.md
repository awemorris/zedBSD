<!-- awesome-plan project=zedbsd record=ws202-p010 -->

# ws202-p010: Vulkan Video の back end (2) 表示順・seek・失敗・表、media-probe の video、5330 の小さい確認

Status: in-progress
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 8 LW
依存: p009

## 現在の適用方針

[最新ユーザー決定](../policy-20261010.md)が以下の旧第2版手順に優先する。具体的手順の改訂/reviewは未了。

## 目的

p009 の back end を再生に使える形に仕上げて表の先頭に入れ、本物の GPU での最初の確かめを早く行う（review-001 M-09）。

## 成果

1. `vkvideo.c`:
   - 表示順と時刻（D22）: p008 の bumping で出る picture に、送った packet の pts を整列した列の最小を当てる（add-in と同じ）。待ち行列が満ちたら send は EAGAIN。drain。
   - in-band だけの track（D16）: avcC に SPS が無い track は degraded 0 で FORMAT、degraded 1 で受け、最初の in-band の SPS・PPS と I まで session を作らず AU を捨てる。
     範囲の外の SPS は send が EINVAL（log 1 回）。
   - flush（design §5.7）: 待ち行列と pts を捨て、`h264_dpb_restart`、RESET、最初の I まで捨てる。
   - 失敗（design §5.9）: status ERROR（log の数え、連続 30 で EIO）。DEVICE_LOST・ETIMEDOUT は decoder の失敗と「壊れた」の印。ETIMEDOUT は `vkDeviceWaitIdle` が
     失敗したら decoder の object を壊さずに持つ。参照の数 0 で device と **instance** を壊し、次の open は新しい instance（J6 (a): libavcodec へは切り替えない）。
2. `decoder.c` の表を `{ &media_vkvideo_ops, &media_aac_ops, &media_avcodec_ops }` に（ws.md の merge の順。p006 が未 merge なら差分を Q1 へ）。
3. `userland/tests/media-probe/`（新、video の部分、design §10.3）: `main.c`・`Makefile`（vkvideo-probe と同じ形の package、`userland/base/common/sha256.c` を compile）。
   `--video-hash`・`--expect`・`--seek=S`・`--time`（open・decode・de-tile の時間）。1 行目 `media-probe: video=<codec>/<backend>`。
4. `plan/ws202/tests/config-media.mk`（design §10.4）: `include config/ci/config-amd64.mk` と `ZEDBSD_USER_PROGRAMS := $(filter-out libavcodec,$(ZEDBSD_USER_PROGRAMS)) media-probe`。
   stream は config に書かない（`test-image.sh` が `ZEDBSD_TEST_EXTRA_FILES` を command line で渡し config の `+=` が消える、design §15 E7）。
5. host 試験 `run-host-vkvideo.sh` に足す: 表示順と時刻（`h264-high-b-aac.mp4`・`h264-nocts.mp4` で時刻が単調、数が AU の数と同じ）、EAGAIN、drain、flush の後の捨て方、
   in-band だけの track（`h264.ts` の packet で degraded 0 は FORMAT、1 は受けて最初の SPS まで捨てる）、status ERROR の数え、DEVICE_LOST の後の open が新しい instance を作る、
   close の後も picture が使える。
6. `plan/tools/media/run-host-codec.sh`（WS の外: 差分を Q1 へ）: `h264.{mkv,ts,avi}`・`h264-nocts.mp4` を足す。host では H.264 は vkvideo が DEVICE → add-in で、既存の
   H.264 の試料（MPEG-TS を含む）が回帰しない。
7. T1 への小さい依頼（Q1 経由、M-09）: image `ZEDBSD_CONFIG=plan/ws202/tests/config-media.mk`、`test-image.sh` の
   `--file /usr/share/zedbsd-tests/ws202/<名>=plan/ws202/tests/streams/<名>`（WS083 と同じ素材の 6 本の mp4、`h264-high-b-aac.mp4` と `.sha256`）。5330 を USB で起動し SSH で:
   `ls -l /usr/share/zedbsd-tests/ws202/`（kei で読める、U12）、`media-probe --video-hash --expect=… …` の 7 本、`media-probe --seek=2.5 …`、`--time` の行（U2、open の時間）。
   結果を待たずに次の仕事へ移ってよい。FAIL は p009・p010 に戻る。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-vkvideo.sh` | p009 と p010 の項目が全部 PASS |
| `sh plan/tools/media/run-host-codec.sh` | PASS |
| `make … ZEDBSD_CONFIG=plan/ws202/tests/config-media.mk ZEDBSD_USER_PROGRAMS="libmedia videoplayer music libbrowser media-probe" …` | warning 0 |
| 5330（T1） | 7 本の全 frame が一致、seek の後の frame が一致、1 行目 `video=h264/vulkan-video` |


## 構造改訂と部分結果（2026-10-10）

POC/seekを維持し、表はnative backendだけ。app fallbackはここへ入れない。device不在はエラー。media-probeはapp fallbackなしで自前経路の証拠にし、標準readback未実装ならclearしない。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。
