# ld.so の多数の object・依存・handle・TLS の試験（WS140 から移した）

zedBSD の動的 loader（`src/rtld/`）が、固定の上限なしに多数の object を扱えることを QEMU の guest で確かめる。ld.so を変えた時の回帰に使う。

| file | 役割 |
| --- | --- |
| `build-many.sh BUILD OUT` | host で target の clang を使い、`rtld-many` と library 206 個を BUILD の libc.so に対して作り、`OUT/../rtld-many.tar` にまとめる |
| `rtld-many.sh [BUILD]` | 上を作り、guest に送って走らせる（16 段）。最後の行 `rtld-many: PASS` |
| `config-amd64-rtld.mk` | 試験の SSH の image（`plan/tools/guest/test-image.sh plan/tools/rtld/config-amd64-rtld.mk BUILD`）。WS066 の `startup-measure.sh` もこの image で流す |
| `rtld-many.c`・`many-lib.c`・`many-hub.c`・`many-tls.c` | 試験の program と library |
| `build-nodelete.sh BUILD OUT`・`rtld-nodelete.sh [BUILD]` | NODELETE の試験（BUG-409）。`rtld-nodelete.c`・`nodelete-lib.c`・`nodelete-dep.c` から program と library 4 個（`-z nodelete` の keep とその依存の dep、`RTLD_NODELETE` で開く flag、対照の plain）を作り、guest で走らせる。最後の行 `rtld-nodelete: PASS` |
| `phdr-pad.py FILE COUNT` | ELF に PT_NULL の program header を足す（16 個を越える header の試験） |

段: startup・direct・global・count-startup・hubs・graph・close・reopen（40 個の DT_NEEDED、160 を越える object、41 object の handle の graph）、tlsdesc・long-name・phdr・phdr-reopen（TLSDESC 300 個、75 byte の名前、31 個の program header）、handles・tls・tls-thread・tls-close（handle 200 個、TLS module 40 個で dtv が伸びる、2 つの thread）。path（検索の届かない `dynload/` の library を絶対 path・相対 path で dlopen、同じ object、無い path は断る。T1-495 の Python の拡張 module の形）。

流し方（T1）: image を作り、`GUEST_RUNTIME=… plan/tools/guest/guest.py start BUILD/hdd-image.img`、`GUEST_RUNTIME=… sh plan/tools/rtld/rtld-many.sh BUILD`。合わせて `plan/ws073/tests/p038/run-tls-check.sh BUILD`（`BUILD/sysroot` の symlink が要る）。

NODELETE の段（`rtld-nodelete.sh`）: plain は dlclose で unload され destructor がその時に走る。keep（DT_FLAGS_1 の DF_1_NODELETE）と flag（dlopen の RTLD_NODELETE）は dlclose の後も残り（object の数・dladdr）、保存した関数を呼べ、thread の終わりにその library の pthread の key の destructor が走り、開き直すと同じ mapping。keep の依存 dep も残る。atexit の関数と destructor は program の最後の行の後（exit の時）だけに走り、dep は keep の後。同じ image で流す。
