#!/bin/sh
# ws034-p001: fetch every archive in distfiles.tsv, plus the verification
# material upstream publishes, into build/distfiles.  Each transfer is bounded
# at 600 seconds.  An archive that is already present is not fetched again.
# usage: fetch-distfiles.sh <repo-root> [key...]
set -u
root=$1; shift
manifest="$root/plan/ws034/phase001/distfiles.tsv"
dist="$root/build/distfiles"
mkdir -p "$dist"

get() {	# $1 url, $2 destination
	test -s "$2" && return 0
	tmp=$(mktemp "$dist/.p001.XXXXXX")
	if curl --fail --location --silent --show-error --max-time 600 \
		--output "$tmp" "$1"; then
		mv -- "$tmp" "$2"; echo "fetched $2"
	else
		rc=$?; rm -f -- "$tmp"; echo "FAILED($rc) $1"; return 1
	fi
}

grep -v '^#' "$manifest" | while IFS='	' read -r key version archive url kind v1 v2; do
	test -n "$key" || continue
	if test $# -gt 0; then
		case " $* " in *" $key "*) ;; *) continue ;; esac
	fi
	get "$url" "$dist/$archive"
	case $kind in
	gh-digest)
		repo=${v1% *} tag=${v1#* }
		gh api "repos/$repo/releases/tags/$tag" \
			--jq ".assets[] | select(.name==\"$archive\") | .digest" \
			> "$dist/$archive.gh-digest" && echo "digest $archive: $(cat "$dist/$archive.gh-digest")"
		;;
	none) ;;
	*)
		for v in $v1 $v2; do
			test -n "$v" && get "$v" "$dist/${archive}.verify.${v##*/}"
		done
		;;
	esac
done
