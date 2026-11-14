#!/bin/sh
# The host test of the scheduler's hint decisions (src/kern/sched-hint.c, hint-test.c beside this file), then the
# same test against copies of the source with one rule broken each, every one of which it must catch.
#
#   plan/ws302/tests/balancer-host/run.sh OUT
#
# OUT receives the binaries and the copies.  The last line is "balancer-host: PASS" when the test passes on the
# source and fails on every broken copy.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
[ $# -ge 1 ] || { echo "usage: run.sh OUT" >&2; exit 2; }
out=$1
mkdir -p "$out"
status=PASS
flags="-std=c99 -O1 -Wall -Wextra -Werror -Iinclude -DCONFIG_KERN_DESKTOP_BALANCER=1"
mutant_flags="-std=c99 -O1 -w -Iinclude -DCONFIG_KERN_DESKTOP_BALANCER=1"
test=plan/ws302/tests/balancer-host/hint-test.c

# The test on the source as it is, with the sanitizers.
cc $flags -fsanitize=address,undefined src/kern/sched-hint.c "$test" -o "$out/hint-test" || status=FAIL
if [ -x "$out/hint-test" ]; then
	"$out/hint-test" > "$out/hint-test.txt" 2>&1 || status=FAIL
	cat "$out/hint-test.txt"
fi

# Each broken rule: a sed expression on the source.  The test must fail on every one.
n=0
while IFS='|' read -r name expression; do
	[ -n "$name" ] || continue
	n=$((n + 1))
	sed "$expression" src/kern/sched-hint.c > "$out/mutant-$n.c"
	if cmp -s src/kern/sched-hint.c "$out/mutant-$n.c"; then
		echo "mutant $name: the expression changed nothing"
		status=FAIL
		continue
	fi
	cc $mutant_flags "$out/mutant-$n.c" "$test" -o "$out/mutant-$n" 2> "$out/mutant-$n.log" || {
		echo "mutant $name: does not build"
		status=FAIL
		continue
	}
	if "$out/mutant-$n" > "$out/mutant-$n.txt" 2>&1; then
		echo "mutant $name: NOT caught"
		status=FAIL
	else
		echo "mutant $name: caught"
	fi
done <<'EOF'
ties to the highest number|s/speed > best_speed/speed >= best_speed/
unusable CPUs chosen|s/if (!classes\[index\].usable)/if (0)/
busy CPUs chosen|s/set = bit_test(backlog, index);/set = 0;/
whole core not required|s/if (whole_core \&\& !whole)/if (0)/
sibling speed ignored|s/return classes\[index\].sibling_rank;/return classes[index].rank;/
efficient placed elsewhere|s/cpu = choose_fastest(classes, count, idle, backlog, SCHED_HINT_CORE_EFFICIENT, 0);/cpu = choose_fastest(classes, count, idle, backlog, SCHED_HINT_CORE_ANY, 0);/
kernel hold off|s/if (in_kernel \&\& base > SCHED_HINT_USER_DEFAULT)/if (0)/
raise above the default|s/if (level < SCHED_HINT_LEVEL_HIGHEST)/if (0)/
raise before the limit|s/if (waited < limit)/if (waited + 1 < limit)/
raise that lifts nothing|s/if (level >= waiting)/if (level > waiting)/
EOF
echo "balancer-host: $status"
