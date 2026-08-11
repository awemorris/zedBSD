#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
BOOTS_USER_TEST_MODE=malformed exec "$repo/scripts/test-user-init.sh"
