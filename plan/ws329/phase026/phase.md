<!-- awesome-plan project=zedbsd record=ws329-p026 -->

# ws329-p026 規約の全文の見直し

- 親: [WS329](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-31 p001 で作成）。
- 依存: p023〜p025。
- 担当の想定: conformance-reviewer。

## 目的と範囲

- WS329 が変えた C（`h264enc-*`・`vcodec-*`・p002 の共通化）を plan/coding-style.md の全文と照らし、書き方だけを直す（動作は変えない）。実装の Phase で持ち越した header の comment の誤りを直す。

## 受け入れ

- coding-style の checklist、run-unit all・roundtrip basic・WS326 の run-unit all が PASS、amd64 build warning 0。

## 変えてはいけない物

- 動作。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。

## 確かめ

- 上の試験と zedBSD の libmedia（x86_64・arm64・riscv64、自分の worktree の build/ で。初回は `make CONFIGMK=config/config.<機種>.mk llvm-toolchain-cached` と `sysroot`）: `timeout 2400 make CONFIGMK=config/config.<機種>.mk -j4 build/<機種>/packages/libmedia/.stamp`（<機種> は x86_64・rpi5・vf2）→ 各 exit 0、warning 0
- `git diff --check`。commit は `git commit -m WIP -- <path>`、push しない。結果の節（host の `date`、command と結果、未実施）を足し、ws.md の行を更新（cleared は Q1）。
