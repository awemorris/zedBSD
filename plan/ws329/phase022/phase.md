<!-- awesome-plan project=zedbsd record=ws329-p022 -->

# ws329-p022 encoder の全体と最初の全体の試験: h264enc-encoder

- 親: [WS329](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-31 p001 で作成）。
- 依存: p004〜p021。
- 担当の想定: Opus 5.5（phase-runner）。module の間の誤りがここで出る。

## 目的と範囲

- `h264enc-encoder.c`（`h264enc.h` の公開の関数と `h264enc-encoder.h` の流れ）。他の encoder の `.c` の不具合をここで直してよい（直した点を結果に、header の誤りは p026 へ持ち越すか Q1 と相談して直す）。
- host の道具 `plan/ws329/tests/tool/h264enc-tool.c`（raw の I420 を stdin、`.264` と再構成の framemd5 と PSNR を出す）と build の script、`plan/ws329/tests/roundtrip.sh`（design §10.2: ffmpeg の lavfi を pipe、ffmpeg の厳しい decode の framemd5、自前の decoder（WS326 p022 の host の道具）の framemd5、encoder の再構成の framemd5（`h264enc_set_recon_hook` で crop した可視の範囲を表示の順に）の 3 者の一致。最後の行 `roundtrip: PASS`）。
- 制約の確かめ（decoder が調べない物）: 道具が `h264enc_get_stats` で max_mb_bits ≤ 3200、au_over 0、MV の範囲（level の MaxVmvR）、PCM の 0 の標本無し（profile 66・77）を確かめ、AU の大きさを `h264enc_level_max_au_bytes` と比べる。CABAC の bin の数は zero_words の計算で満たす（9.3.4.6）。

## 受け入れ

- roundtrip の基本の組: 3 profile × CAVLC/CABAC（CB は CAVLC だけ）× B 0・2 × slice 1・3 × 176x144 と 360x240 × 30 frame で 3 者の MD5 が全 frame 一致、ffmpeg の誤り 0。
- CQP・CBR・VBR の各 1 本で packet の数・時刻の単調・key の flag。
- `run-unit.sh all`・WS326 の `run-unit.sh all` PASS、amd64 の libmedia.so warning 0。

## 変えてはいけない物

- decoder の `.c`・header（不具合を見つけたら Q1 へ。WS326 の範囲）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。

## 確かめ

- `timeout 3600 sh plan/ws329/tests/roundtrip.sh basic`
- `sh plan/ws329/tests/run-unit.sh all`、`sh plan/ws326/tests/run-unit.sh all`
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、warning 0
- `git diff --check`。commit は `git commit -m WIP -- <path>`、push しない。結果の節（host の `date`、command と結果、未実施）を足し、ws.md の行を更新（cleared は Q1）。
