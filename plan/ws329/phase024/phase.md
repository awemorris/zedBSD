<!-- awesome-plan project=zedbsd record=ws329-p024 -->

# ws329-p024 libmedia の backend: h264enc-backend

- 親: [WS329](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-31 p001 で作成）。
- 依存: p022、WS318 p012a（encoder の層と ops が main にある）。
- 担当の想定: Opus 5.5（phase-runner）。

## 目的と範囲

- design §11: WS318 の encoder の backend の ops に `h264enc-backend.c` で載せる。表の位置（hardware の後、libavcodec の前）、query の行、config の写し（Q329-5 の答え）、RAW の出力と codec data は WS318 の `packet-format.c`。
- WS318 の encoder の host の試験に h264-software の行を足す（往復、CONFIG の断り、implementation の指定）。
- QEMU の確かめの道（image の中の短い encode の program、判定は console の log でなく SSH の出力）を決めて 1 回流す。

## 受け入れ

- WS318 の encoder の試験 PASS、run-unit と roundtrip basic が変わらず PASS、amd64 build warning 0、QEMU の短い encode（SSH で出力の MD5 と ffmpeg の decode を host で確かめる）。

## 変えてはいけない物

- WS318 の file は p024 に要る所だけ（表の 1 行、試験の行）。他は WS318 の担当と Q1 が調整。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。

## 確かめ

- WS318 の encoder の試験の command（p012a が決める）
- `timeout 3600 sh plan/ws329/tests/roundtrip.sh basic`
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、warning 0
- `git diff --check`。commit は `git commit -m WIP -- <path>`、push しない。結果の節（host の `date`、command と結果、未実施）を足し、ws.md の行を更新（cleared は Q1）。
