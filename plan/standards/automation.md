# Standards and automation coverage

Full standard: [C coding style](../coding-style.md). Concise version not created;
load applicable full sections. Non-C areas use existing local conventions and
Phase-specific requirements; no new global policy is invented by adoption.

| Check | Command / version observed | Coverage and limit |
| --- | --- | --- |
| C formatting | `clang-format --dry-run --Werror <changed-files>`; 19.1.7; root `.clang-format` | Formatting only; cannot prove ownership, naming, layer/API semantics |
| Whitespace | `git diff --check -- <changed-paths>` | Diff whitespace only |
| Build | `make -j16` for selected supported configuration | Compile/link only; must record platform/configuration |
| Runtime | Phase's bounded `qemu-system-x86_64` invocation for amd64 | Guest observed behavior; not physical acceptance |
| Full standard | Human/agent review of changed source against full document | Record checked sections, exceptions, residuals and skipped checks |
| Plan sync | `python3 plan/tools/sync.py status` | Local state/outbox visibility; not remote semantic correctness |

No aggregate `make check`. No newly installed formatter/linter or mass formatting.
Versions are observations, not a new required toolchain pin. Record actual tools
and results on each relevant Phase. A WS's standards review needs its own agreed
scope and execution authority; do not fabricate conformance for prior user acceptance.

2026-09-12ユーザー確認: HAL配下の全変更は事前に具体差分への明示許可を確認する。既存宣言への実装補完も対象。自動formatter/buildの成功は適用許可を意味しない。Guardrailを参照。
