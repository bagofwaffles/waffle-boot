#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The tests of docs/recover/core.js and install.js: node --test over tests/web/, which runs the Python tool's
# scenarios (gen_cases.py) and the simulated units (unit_bridge.py) itself.
# Exit status 0 when every test passed, or when node is not installed (said, and skipped).
set -eu
cd "$(dirname "$0")/../.."
if ! command -v node >/dev/null 2>&1; then
	echo "tests/web: SKIPPED, node is not installed (Node 20 or later runs them)"
	exit 0
fi
major=$(node -p 'process.versions.node.split(".")[0]')
if [ "$major" -lt 20 ]; then
	echo "tests/web: SKIPPED, node $major is older than 20"
	exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "tests/web: python3 is not installed: only the tests that need no Python tool run"
fi
# the page's module loads as a browser loads it: no syntax beyond ES modules
node --check docs/recover/core.js
node --check docs/recover/words.js
node --check docs/recover/install.js
node --check docs/recover/install_words.js
exec node --test tests/web/test_core.mjs tests/web/test_install.mjs
