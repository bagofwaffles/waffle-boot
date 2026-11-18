#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The tests of docs/web/core.js, install.js, mtp.js and usb.js: node --test over tests/web/, which runs the
# Python tools' scenarios (gen_cases.py), the simulated units (unit_bridge.py) and the MTP responders
# (mtp_bridge.py) itself.
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
node --check docs/web/core.js
node --check docs/web/words.js
node --check docs/web/install.js
node --check docs/web/install_words.js
node --check docs/web/mtp.js
node --check docs/web/usb.js
exec node --test tests/web/test_core.mjs tests/web/test_install.mjs tests/web/test_mtp.mjs
