<!-- awesome-plan project=zedbsd record=ws034p043 -->

# ws034-p043: fork した子の blocking write が親の copy-on-write fault を止める

Phase ID: `ws034-p043`
Parent: [WS034](../ws.md)
Status: **cleared**（q337-i01、2026-09-24）
Phase disposition: normal
Queue: q337（q337-i01）
実行: メインセッション

## きっかけ

ws034-p042 の試験で見つけた。`sys_write`（`src/kern/syscall.c`）は user buffer を `uaccess_pin` したまま、
socket の送信で眠る（window を待つ間など）。その buffer の page を fork で共有している親が同じ page に書くと、
copy-on-write の fault が、子の write が終わるまで待たされた。子の送信が親の読み出しを待っていると、互いに待ち合う。

## 範囲

1. 再現する最小の試験（fork の後、子が共有 page から大きな write をして眠り、親がその page に書く）。
2. pin された page の copy-on-write を、pin の解除を待たずに済ませる（写すだけなので、元の page は読めればよい）か、
   write が眠る前に pin を外す（kernel 側へ写してから眠る）。どちらが VM の設計に合うかを決める。
3. host の VM の試験とゲストで確かめる。

## 原因

`vmspace_fault()` の、既にある private page への fault は、まず `vm_private_page_io_acquire()` で page の
排他的な I/O 所有権を取る。これは **pin が全部外れるまで待つ**。copy-on-write の fault も同じ道を通るので、
相手の process が同じ page を pin したまま眠っていると（socket や pipe への blocking write）、その write が
終わるまで fault が進まない。相手の write が、fault した側が読むのを待っていれば、互いに待ち続ける。

`fork()` 自身は同じ状況を扱っていた（pin された resident な page は、待たずにその場で写す）が、
fork の後に pin された page の copy-on-write にはその扱いが無かった。

## 修正（`src/kern/vmspace.c`、`src/kern/vm.c`）

- copy-on-write の書き込み fault で、page が他者に pin されていて resident なら、**所有権を取らず**、
  この fault 自身の pin（`vm_private_page_pin()`）の下で写す。pin は page を memory に留め、reclaim・swap・
  所有者を遠ざける。共有された copy-on-write の page に付く pin は読むためのものだけなので、写す間に中身は変わらない。
- revalidation で copy-on-write でなくなっていたら、所有権の道からやり直す（EAGAIN）。
- 写しの install で見る状態は、所有者なら BUSY と RESIDENT、pin で写すなら RESIDENT。
- `vm_page_replace_private()` は、古い backing を「所有している」ことを確かめていた。pin で持っていることも
  認めるようにした（reverse mapping の一覧は、どちらでも `reclaim_lock` の下で変える）。

最初の版は `vm_page_replace_private()` の確認で kernel が止まった（`replacing an unowned VM private backing`）。
それを受けて上の最後の点を足した。

## 検証

| 検証 | 結果 |
| --- | --- |
| `tcp-bulk` の `TCPBULK_SHARED=1`（書き手が fork 前に埋めた共有 buffer から送り、読み手が同じ buffer に書く） | **4 KiB〜256 KiB で全部届き、中身が一致**。修正前は受け側が `accept` の後の書き込みで止まり、0 byte で timeout |
| `cow-stress.c`（40回: 子が、親が blocking な pipe write で pin した共有 page へ書く。親子とも中身を確かめる） | `COWSTRESS PASS bad=0` |
| 通常の `tcp-bulk`、pipe を 100 回 | PASS |
| amd64 `disk-image`、pcat `vmunix` | PASS |

VM の host 試験（`plan/ws025/tests/run-user-lease-host.py`、`plan/ws025/tests/run-file-cache-host.sh` など）は、
**この変更の前から build できない**（`tid_t` が無い等）。修正前の `vmspace.c` でも同じく失敗することを確かめた。
VM の host 試験を直すのは別の作業である。
