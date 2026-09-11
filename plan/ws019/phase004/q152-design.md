# q152 admission manifest and capacity integration

Date: 2026-09-09
Timebox: 90 active minutes

Implement the pure admission-to-transaction boundary: validate the supported
fixed source configuration against independently selected configuration-volume
identities, generate the six ordered transaction entries, generate explicit
destination PARTUUID configuration, and parse the complete successful df
record. Bind this to the existing command adapter without a new public helper.

Paths are controlled absolute ASCII components, without dot traversal, repeated
separators or shell/control characters. This is lexical validation, not a
symlink/mount ownership proof. Destination ESP and payload mount paths must be
distinct and not nested. Source artifact metadata remains separately verified
by the forthcoming source/format admission owner.

Capacity charges only missing files, rounded to the actual FAT cluster size.
Reserve two extra clusters per missing file for worst-case stage/final directory
entry growth, plus two per directory to be created (parent entry and initial
directory cluster). Fixed filenames need fewer than one cluster of entries at
the minimum supported cluster size. Existing files must already pass exact
verification before they may be excluded. This estimate is not a reservation;
actual allocation errors still use q149 cleanup. The caller supplies verified
BPB cluster sizes and exact missing-directory counts.

Test actual production Noct modules with independently calculated layouts,
all 64 presence subsets, cluster sizes, overflow/capacity refusal, malformed
command output, configuration source redirection and controlled-path rejection.
Exercise JIT and interpreter modes. Full source mounts, immutable binary-format
validation, GPT/BPB snapshot revalidation, confirmation and public packaging
remain necessary before p004 is complete. No partial launcher is installed.
