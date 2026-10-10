# WS202: Q1からT1へ渡す再生確認依頼

状態: **準備済み、未投入・未実施**。Q1のsource統合後に実行。実装担当はAGENTS.mdの「試験の担当T1」に従い、QEMU/実機を起動していない。mainへのmergeと共有Queue投入はQ1担当。WS202の完了を証明した記録ではない。

## 入力とbuild

- 構成: `plan/ws202/tests/config-media.mk`（CI構成からlibavcodec packageを除外、media-probeを追加）。i915 Vulkan Videoが既定ONのmainと統合する。
- source-owned入力: [tests/streams/README.md](tests/streams/README.md)。過去のbuild成果をimage入力にしない。
- 出力に必要なprogram: libmedia・libvulkan・videoplayer・music・audiod・openssh・media-probe。package一覧でlibavcodecがないことを確認する。
- 通常の試験image作成（Q1/T1専用、実装担当は未実行）:

```sh
sh plan/tools/guest/test-image.sh plan/ws202/tests/config-media.mk build/ws202-t1 \
  --file /usr/share/zedbsd-tests/ws202/h264-high-b-aac.mp4=plan/ws202/tests/streams/h264-high-b-aac.mp4 \
  --file /usr/share/zedbsd-tests/ws202/h264-high-b.video.sha256=plan/ws202/tests/streams/h264-high-b.video.sha256 \
  --file /usr/share/zedbsd-tests/ws202/h264-high-b-aac.rms=plan/ws202/tests/streams/h264-high-b-aac.rms \
  --file /usr/share/zedbsd-tests/ws202/h264-nocts.mp4=plan/ws202/tests/streams/h264-nocts.mp4 \
  --file /usr/share/zedbsd-tests/ws202/h264-nocts.video.sha256=plan/ws202/tests/streams/h264-nocts.video.sha256 \
  --file /usr/share/zedbsd-tests/ws202/aac-stereo.m4a=plan/ws202/tests/streams/aac-stereo.m4a \
  --file /usr/share/zedbsd-tests/ws202/h264-uat.mp4=plan/ws202/tests/streams/h264-uat.mp4 \
  --file /usr/share/zedbsd-tests/ws202/h264-uat.video.sha256=plan/ws202/tests/streams/h264-uat.video.sha256 \
  --file /usr/share/zedbsd-tests/ws202/h264-uat.rms=plan/ws202/tests/streams/h264-uat.rms
```

通常のtoolchain/image規則を使う。このsessionのビルドはnamed library/application/kernel objectのみで、disk-image・bootは未実施。

## 5330実機（ユーザーがUSBで起動、T1はSSH）

```sh
cd /usr/share/zedbsd-tests/ws202
media-probe --video-hash --time --expect=h264-high-b.video.sha256 h264-high-b-aac.mp4
media-probe --video-hash --twice --expect=h264-high-b.video.sha256 h264-high-b-aac.mp4
media-probe --video-hash --seek=1 --expect=h264-high-b.video.sha256 h264-high-b-aac.mp4
media-probe --video-hash --expect=h264-nocts.video.sha256 h264-nocts.mp4
media-probe --video-hash --expect=h264-uat.video.sha256 h264-uat.mp4
media-probe --video-hash --seek=2.5 --expect=h264-uat.video.sha256 h264-uat.mp4
media-probe --audio-rms h264-high-b-aac.mp4 > /tmp/ws202-audio.rms
media-probe --audio-rms h264-uat.mp4 > /tmp/ws202-uat-audio.rms
```

- videoはstderrに`codec=h264 backend=vulkan-video`、各commandがexit0、50/25/300など参照と同じ表示frameを出すこと。`--twice`は2sessionを同時に開き、各receive結果を比較する。2process同時も同じ参照で確認。
- audioは`codec=aac-lc backend=libmedia`、2秒が96000、12秒が576000 stereo frame。targetの`.rms`をhostへ取得し、`compare-rms.py`でsourceの参照と比較する。`--expect`はvideo用でaudioには使わない。
- windowをユーザーsessionで`videoplayer /usr/share/zedbsd-tests/ws202/h264-uat.mp4`から開く。`OPEN ... video=h264/vulkan-video audio=aac-lc/libmedia`、moving patternと音、Space pause/resume、右矢印seek、full screen、`ENDED`を確認。`FRAMES shown=... time_ms=... late=...`はpause/100frame/end/closeに出る。音のringを消費し終える前のENDや、seek後の前の音の混入がないことを確認。
- `music /usr/share/zedbsd-tests/ws202/aac-stereo.m4a`。`PLAY open codec=aac-lc backend=libmedia`、位置進行・seek・終端・曲切替を確認。短いfixtureのためpause/seek操作は手持ちの長いAAC-LC曲でもUATする。
- decode失敗ならcommandのstderr、target/commit、再現command、最初のPTS/hash差を保存してQ1へ返す。画素・音・目視の結果を別々に記録する。

## QEMU / optional software

`boot-test.sh`のPNGで起動確認、音deviceを付けてMusicのnative AAC-LCを確認。Venus等のvideo非対応GPUで、native media-probeはDEVICEを返し、libavcodec無しのVideo Playerは未対応のnoticeを出す。libavcodecありの通常回帰構成では、Video Playerがapp所有のfallbackを使う（video `h264/libavcodec`、audio `aac-lc/libmedia`）。QEMUの結果をi915実機のpixel証拠には使わない。

## 残るwhole WSの確認

この依頼は最初のnative再生・標準readback確認。WS第4版の全stream matrix/WS083の6試料、crop/SARのencoded fixture、欠落packet/conformance bitstream、multichannel/PCEのPCM精度、1080p30＋zgearsの90%性能基準、8context/BUSY、長いTS/AVIのmulti-AUとin-band変化、全AAT/UATを代用しない。これらはWS/p012/p013/p014の未達条件として保持する。既存の共有AAT helperと旧`run-host-codec.sh`のapp移管追従はQ1向けproposalで別に渡す。
