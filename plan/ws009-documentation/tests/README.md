# WS009 shared test cases

Parent: [WS009](../ws.md)

| Case ID | Required observation |
| --- | --- |
| DOC-T00 | Every relative Markdown file link resolves and navigation reaches all active WS/Phase documents |
| DOC-T10 | Public UAPI statements cite headers, implementations, and executable tests |
| DOC-T20 | A clean supported environment follows the build guide through image creation |
| DOC-T30 | Documented QEMU boot commands reach the stated loader/root/login outcome |
| DOC-T31 | Kernel parameter reference matches parser names, defaults, and unknown-key behavior |
| DOC-T40 | init/service/network examples pass against installed commands and current configuration grammar |
| DOC-T50 | Console, graphics, system, input, and GPU references distinguish current, deprecated, and planned interfaces |
| DOC-T60 | POSIX/SUS and `_XOPEN_SOURCE` claims agree with WS001, public headers, and compile/runtime tests |

Link validation is shared across all WSs; producer WSs own behavioral evidence
while WS009 owns document accuracy and navigation.

## DOC-T00 command

Run the native Noct validator from the repository root:

```sh
build/NoctLang/build-static/noct \
  plan/ws009-documentation/tests/check-relative-links.noct docs plan
```

It recursively checks every Markdown link with a relative file target. Web,
mail, absolute-path, and same-document fragment links are intentionally outside
this file-existence check.
