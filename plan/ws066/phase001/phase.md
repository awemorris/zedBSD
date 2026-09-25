<!-- awesome-plan project=zedbsd record=ws066p001 -->

# ws066-p001: 動的 link の起動の費用の内訳と設計

Phase ID: `ws066-p001`
Parent: [WS066](../ws.md)
Status: planning
Queue: none
Disposition: normal

## 範囲

guest で `/bin/true`・`sh -c :`・`cc t.c -o t` の起動を段階ごとに測る（kernel の exec、`ld.so` の mapping、再配置、symbol の探索、TLS、`libc` の初期化）。再配置と探索の数を object ごとに数える。WS066 の候補から効果の大きいものを選び、実装の Phase に分ける。

## 受け入れ

内訳の表（QEMU の証拠）と、選んだ候補・見込み・影響（toolchain の rebuild の要否、ABI）を書いた設計。実装は含まない。
