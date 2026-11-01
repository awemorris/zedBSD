#!/bin/sh
# Builds and runs plan/ws334/tests/storage-test.c: libbrowser's instances over the files backend and a
# fake backend (the cookie jar and Web Storage kept across instances, two instances apart, the backend's
# close and errors).  The engine's objects come from the WS074 host build (plan/ws074/tests/host-build.sh
# VARIANT, run first); the test's pages are served by the host's python3 -m http.server on 127.0.0.1.
#   sh plan/ws334/tests/storage-test.sh [plain|asan]
# PORT=<n> chooses the server's port (default 18335); the persistent cache part uses the server of
# cache-server.py on the next port (the test stops it at the end).  The output goes to a new directory of
# build/tmp/ws334-storage (fresh_out).
# Last line: storage-test: PASS or storage-test: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
variant=${1:-plain}
case $variant in
plain|asan) ;;
*) echo "storage-test: unknown variant $variant" >&2; exit 2 ;;
esac
PORT=${PORT:-18335}
CACHE_PORT=$((PORT + 1))
out=build/tmp/ws334-storage
fresh_out "$out"
run=$(cd "$out" && pwd -P)

# The engine, built by the WS074 host build.
if ! timeout 3600 sh plan/ws074/tests/host-build.sh "$variant" > "$run/host-build.log" 2>&1; then
	tail -20 "$run/host-build.log"
	echo "storage-test: FAIL (the host build failed, log $run/host-build.log)"
	exit 1
fi
host=build/ws074-host/$variant
flags=$(cat "$host/flags" | cut -d' ' -f2-)
engine=$(ls "$host"/obj/*.o | grep -v -e '/browser_main\.o$' -e '/browser_main-posix\.o$' -e '/host-shell\.o$' -e '/browser-os-error\.o$')
if ! timeout 600 ${CC:-cc} $flags -Iplan/ws334/tests/unit -o "$run/storage-test" plan/ws334/tests/storage-test.c \
    $engine -lvulkan -lm -ldl -lpthread > "$run/compile.log" 2>&1 || [ -s "$run/compile.log" ]; then
	cat "$run/compile.log"
	echo "storage-test: FAIL (the test did not compile cleanly)"
	exit 1
fi

# The pages: set.html and get.html over http, old.html as a file: page.
mkdir -p "$run/pages" "$run/a" "$run/b"
cat > "$run/pages/set.html" <<'PAGE'
<!DOCTYPE html><script>
localStorage.setItem("item", "kept");
document.cookie = "page=10; max-age=3600";
console.log("set");
</script>
PAGE
cat > "$run/pages/get.html" <<'PAGE'
<!DOCTYPE html><script>
console.log("item=" + localStorage.getItem("item") + " cookie=" + document.cookie);
</script>
PAGE
cat > "$run/pages/old.html" <<'PAGE'
<!DOCTYPE html><script>
console.log("old=" + localStorage.getItem("old"));
if (localStorage.getItem("new") === null)
	localStorage.setItem("new", "written");
else
	console.log("new=" + localStorage.getItem("new"));
</script>
PAGE

# The server, until the test ends.
/usr/bin/python3 -I -m http.server --bind 127.0.0.1 --directory "$run/pages" "$PORT" > "$run/server.log" 2>&1 &
server=$!
tries=0
until /usr/bin/python3 -I -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), 1).close()" "$PORT" 2> /dev/null; do
	tries=$((tries + 1))
	if [ "$tries" -gt 50 ]; then
		kill "$server" 2> /dev/null
		echo "storage-test: FAIL (the server did not start on port $PORT)"
		exit 1
	fi
	sleep 0.1
done

# The cache part's server, which the test stops itself.
/usr/bin/python3 -I plan/ws334/tests/cache-server.py "$CACHE_PORT" > "$run/cache-server.log" 2>&1 &
cache_server=$!
tries=0
until /usr/bin/python3 -I -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), 1).close()" "$CACHE_PORT" 2> /dev/null; do
	tries=$((tries + 1))
	if [ "$tries" -gt 50 ]; then
		kill "$server" "$cache_server" 2> /dev/null
		echo "storage-test: FAIL (the cache server did not start on port $CACHE_PORT)"
		exit 1
	fi
	sleep 0.1
done

# The test.
ASAN_OPTIONS=detect_stack_use_after_return=0 timeout 300 "$run/storage-test" "$run" "$PORT" "$CACHE_PORT" > "$run/test.log" 2>&1
code=$?
kill "$server" "$cache_server" 2> /dev/null
wait "$server" "$cache_server" 2> /dev/null
sync
cat "$run/test.log"
if [ "$code" = 0 ] && tail -1 "$run/test.log" | grep -q '^test-storage: PASS'; then
	echo "storage-test: PASS"
else
	echo "storage-test: FAIL (exit $code, log $run/test.log)"
	exit 1
fi
