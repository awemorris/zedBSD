#!/bin/sh
# ws042: fetches the oils spec tests (Apache-2.0) at a fixed commit into
# build/ws042/oils.  They are test input only and are not kept in the tree.
set -e
commit=08310e96b182cf1d2dd65161e07b1743d17f8936
dir=${1:-build/ws042/oils}
if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]; then
	echo "$dir"; exit 0
fi
rm -rf "$dir"
git init -q "$dir"
git -C "$dir" fetch -q --depth 1 https://github.com/oils-for-unix/oils.git "$commit"
git -C "$dir" checkout -q FETCH_HEAD
echo "$dir"
