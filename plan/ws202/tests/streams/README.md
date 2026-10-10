# WS202の実機確認入力

すべてこのWSがFFmpeg/libx264の合成入力から生成した素材（Zlib）。外部の映像・音声・実装コードを取り込んでいない。encoderの版とbuild設定は`encoder-version.txt`、入力と参照のSHA-256は`SHA256SUMS`。固定素材の再生成は`sh plan/ws202/tests/make-uat-streams.sh`。encoderの版が変わるとbytesも変わるため、今回の実機確認にはcommitされた素材と対応する参照を一組で使う。

| File | 用途 |
| --- | --- |
| h264-high-b-aac.mp4 | 320×180、25fps、2秒、H.264 High/B + mono AAC-LC 48kHz |
| h264-high-b.video.sha256 | 独立FFmpeg decoderの表示PTS（microseconds）とvisible NV12 hash、50行 |
| h264-high-b-aac.rms | 独立FFmpeg decoderのmonoを左右同値にした48kHz PCM、MP4終端2秒を適用、1024 stereo frameごと94行 |
| h264-nocts.mp4 / h264-nocts.video.sha256 | 同じ映像bitstreamをcttsなしに格納。hashは同じ、時刻は昇順packet clockを消費する仕様の参照 |
| aac-stereo.m4a | Musicのnative AAC-LC確認、44.1kHz stereo、1.2秒 |
| h264-uat.mp4 / h264-uat.h264 | 640×360、25fps、12秒、High/open GOP/B pyramid/4 references + mono AAC-LC 48kHz。pause・seek・full screen用 |
| h264-uat.video.sha256 / h264-uat.rms | 独立参照、300 video frame、576000 stereo audio frame |

raw NV12/PCMとhost stand-inのhashはimageへ入れない。ここに置いた`.video.sha256`は実decoderの参照であり、hostのGPU stand-inが生成したhashとは異なる。

確認前にhostで`cd plan/ws202/tests/streams && sha256sum -c SHA256SUMS`。確認ツールはvideoのPTS/hash両方と参照末尾までの消費を検査する。audio RMSはtarget出力をhostへ取得し、`python3 plan/ws202/tests/compare-rms.py REFERENCE ACTUAL`で比較する。
