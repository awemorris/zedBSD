#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
ZEDBSD_USER_TEST_MODE=fault exec "$repo/scripts/test-user-init.sh"
