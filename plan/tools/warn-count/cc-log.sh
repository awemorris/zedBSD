#!/bin/bash
# Compiler wrapper for the warning count: runs the compiler given as the
# arguments and keeps a copy of its diagnostics in WARN_LOG_DIR, one file per
# object, named after the object's path under build/<platform>/packages/.
# Diagnostics still go to stderr and the exit status is the compiler's.
obj=
prev=
for a in "$@"; do
	if [ "$prev" = "-o" ]; then obj=$a; fi
	prev=$a
done
if [ -z "$WARN_LOG_DIR" ] || [ -z "$obj" ]; then
	exec "$@"
fi
rel=${obj#*/packages/}
log="$WARN_LOG_DIR/$(printf '%s' "$rel" | tr '/' '%').log"
mkdir -p "$WARN_LOG_DIR"
"$@" 2> >(tee "$log" >&2)
rc=$?
wait
exit $rc
