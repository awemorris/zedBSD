# ld.so の多数の object・依存・handle・TLS の試験（WS140 から移した）

zedBSD の動的 loader（`src/rtld/`）が、固定の上限なしに多数の object を扱えることを QEMU の guest で確かめる。ld.so を変えた時の回帰に使う。

| file | 役割 |
| --- | --- |
| `build-many.sh BUILD OUT` | host で target の clang を使い、`rtld-many` と library 206 個を BUILD の libc.so に対して作り、`OUT/../rtld-many.tar` にまとめる |
| `rtld-many.sh [BUILD]` | 上を作り、guest に送って走らせる（16 段）。最後の行 `rtld-many: PASS` |
| `config-amd64-rtld.mk` | 試験の SSH の image（`plan/tools/guest/test-image.sh plan/tools/rtld/config-amd64-rtld.mk BUILD`）。WS066 の `startup-measure.sh` もこの image で流す |
| `rtld-many.c`・`many-lib.c`・`many-hub.c`・`many-tls.c` | 試験の program と library |
| `build-nodelete.sh BUILD OUT`・`rtld-nodelete.sh [BUILD]` | NODELETE の試験（BUG-409）。`rtld-nodelete.c`・`nodelete-lib.c`・`nodelete-dep.c` から program と library 4 個（`-z nodelete` の keep とその依存の dep、`RTLD_NODELETE` で開く flag、対照の plain）を作り、guest で走らせる。最後の行 `rtld-nodelete: PASS` |
| `build-finalize.sh BUILD OUT`・`rtld-finalize.sh [BUILD]` | exit handler の試験（BUG-410）。`rtld-finalize.c`・`finalize-lib.c`・`finalize-cxx.cc` から program と library 2 個（dlclose で unload される plain、`-z nodelete` の keep。どちらも C++ の静的な object を持つ）を作り、guest で走らせる。最後の行 `rtld-finalize: PASS` |
| `phdr-pad.py FILE COUNT` | ELF に PT_NULL の program header を足す（16 個を越える header の試験） |

段: startup・direct・global・count-startup・hubs・graph・close・reopen（40 個の DT_NEEDED、160 を越える object、41 object の handle の graph）、tlsdesc・long-name・phdr・phdr-reopen（TLSDESC 300 個、75 byte の名前、31 個の program header）、handles・tls・tls-thread・tls-close（handle 200 個、TLS module 40 個で dtv が伸びる、2 つの thread）。path（検索の届かない `dynload/` の library を絶対 path・相対 path で dlopen、同じ object、無い path は断る。T1-495 の Python の拡張 module の形）。

流し方（T1）: image を作り、`GUEST_RUNTIME=… plan/tools/guest/guest.py start BUILD/hdd-image.img`、`GUEST_RUNTIME=… sh plan/tools/rtld/rtld-many.sh BUILD`。合わせて `plan/ws073/tests/p038/run-tls-check.sh BUILD`（`BUILD/sysroot` の symlink が要る）。

NODELETE の段（`rtld-nodelete.sh`）: plain は dlclose で unload され destructor がその時に走る。keep（DT_FLAGS_1 の DF_1_NODELETE）と flag（dlopen の RTLD_NODELETE）は dlclose の後も残り（object の数・dladdr）、保存した関数を呼べ、thread の終わりにその library の pthread の key の destructor が走り、開き直すと同じ mapping。keep の依存 dep も残る。atexit の関数と destructor は program の最後の行の後（exit の時）だけに走り、dep は keep の後。同じ image で流す。

exit handler の段（`rtld-finalize.sh`）: plain は atexit の関数、`__dso_handle` 付きの `__cxa_atexit`、tail call で呼ぶ atexit（`-O2`、build-finalize.sh が `jmp atexit@plt` を確かめる）、handle の無い `__cxa_atexit`、4 つの thread から同時に 32 個の atexit を登録し、C++ の静的な object の destructor は初期化の時に `__cxa_atexit` で登録される。dlclose の中でそれらが新しい順に 1 回ずつ走り、その後に library の destructor が走り、object が消える（program が自分で照合する）。開き直して閉じても新しい登録だけが 1 回走る。keep（NODELETE）は dlclose で何も走らず、program の最後の行の後（exit の時）に keep の handler が新しい順、program の atexit、keep の destructor の順に走る。同じ image で流す。
