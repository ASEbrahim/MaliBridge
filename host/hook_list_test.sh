#!/bin/bash
# The cached server hook list tests/check_cached_server_hooks.py must match what the scanner finds in server/
cd "$(dirname "$0")/.."
python3 tests/check_cached_server_hooks.py
