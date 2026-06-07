#!/usr/bin/env bash
set +e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

if command -v node >/dev/null 2>&1; then
  node "$SCRIPT_DIR/bootstrap.mjs" --repo "$REPO_ROOT" >/dev/null 2>&1
fi

exit 0
