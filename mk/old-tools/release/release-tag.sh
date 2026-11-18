#!/bin/sh
# The tag checks of the release workflow (.github/workflows/release.yml, ws129-p004; plan/ws129/release.md
# section 5), kept here so that they run the same way on a developer's machine.  Run from the top of the tree,
# checked out at the tag (VERSION is read from it).
#
#   tools/release/release-tag.sh classify TAG
#       An rc tag, zedbsd-<VERSION>-rc<N>, is built: prints kind=build, rc=N, version=VERSION.
#       A final tag, zedbsd-<VERSION>, promotes an rc: prints kind=promote, rc=, version=VERSION.
#       Anything else, or a tag whose version is not VERSION's, fails.
#   tools/release/release-tag.sh rc FINAL_TAG [N]
#       Prints the rc tag a final tag promotes: zedbsd-<VERSION>-rc<N>, or without N the highest-numbered
#       rc tag of the version whose commit is in the final tag's history.  Fails when there is none.
#   tools/release/release-tag.sh promotable RC_TAG FINAL_TAG
#       Fails unless the rc's commit is in the final tag's history and the two differ only under
#       docs/release/ (the notes may be finished after the rc; anything else must be built again).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

# Stops with a message.
fail() {
	echo "release-tag: $*" >&2
	exit 1
}

# The version in the tree: one line of digits, dots, letters and dashes.
version() {
	test -f VERSION || fail "no VERSION here"
	value=$(cat VERSION)
	printf '%s\n' "$value" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+(-[a-z0-9]+(\.[a-z0-9]+)*)?' ||
	    fail "VERSION is not a version: $value"
	printf '%s\n' "$value"
}

# The commit a tag names.
commit_of() {
	git rev-parse -q --verify "refs/tags/$1^{commit}" || fail "no tag $1"
}

case "${1:-}" in
classify)
	test $# -eq 2 || fail "usage: classify TAG"
	tag=$2
	wanted=$(version)
	case "$tag" in
	"zedbsd-$wanted")
		printf 'kind=promote\nrc=\nversion=%s\n' "$wanted"
		;;
	"zedbsd-$wanted"-rc*)
		number=${tag#"zedbsd-$wanted-rc"}
		printf '%s\n' "$number" | grep -Eqx '[1-9][0-9]*' || fail "the rc number of $tag is not a number"
		printf 'kind=build\nrc=%s\nversion=%s\n' "$number" "$wanted"
		;;
	*)
		fail "$tag is neither zedbsd-$wanted nor zedbsd-$wanted-rc<N> (VERSION is $wanted)"
		;;
	esac
	;;
rc)
	test $# -eq 2 || test $# -eq 3 || fail "usage: rc FINAL_TAG [N]"
	final=$2
	final_commit=$(commit_of "$final")

	# The one named.
	if test -n "${3:-}"; then
		printf '%s\n' "$3" | grep -Eqx '[1-9][0-9]*' || fail "the rc number $3 is not a number"
		commit_of "$final-rc$3" >/dev/null
		printf '%s\n' "$final-rc$3"
		exit 0
	fi

	# The highest-numbered rc in the final tag's history.
	best=
	best_number=0
	for tag in $(git tag --list "$final-rc*"); do
		number=${tag#"$final-rc"}
		printf '%s\n' "$number" | grep -Eqx '[1-9][0-9]*' || continue
		test "$number" -gt "$best_number" || continue
		git merge-base --is-ancestor "$(commit_of "$tag")" "$final_commit" || continue
		best=$tag
		best_number=$number
	done
	test -n "$best" || fail "no rc tag of $final in its history"
	printf '%s\n' "$best"
	;;
promotable)
	test $# -eq 3 || fail "usage: promotable RC_TAG FINAL_TAG"
	rc_commit=$(commit_of "$2")
	final_commit=$(commit_of "$3")
	git merge-base --is-ancestor "$rc_commit" "$final_commit" || fail "$2 is not in the history of $3"
	others=$(git diff --name-only "$rc_commit" "$final_commit" | grep -v '^docs/release/' || true)
	test -z "$others" || fail "$3 changes more than docs/release/ since $2: $(echo $others)"
	echo "release-tag: $3 promotes $2"
	;;
*)
	fail "usage: classify TAG | rc FINAL_TAG [N] | promotable RC_TAG FINAL_TAG"
	;;
esac
