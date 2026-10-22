#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The layer's generated dispatch (layer/gen/layer_dispatch_gen.h) is built from
codegen/cached_server_hook_list.py, a cache of the hooks our scanner found in server/.

A hook added/removed in server/ without regenerating the cache leaves the layer's dispatch out of date.
"""

import difflib
import os
import sys


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, ".."))


def as_lines(hooks):
    return [f'    ("{vk_func}", "{hook_func}", "{file_name}"),' for vk_func, hook_func, file_name in hooks]


def main():
    root = repo_root()
    sys.path.insert(0, os.path.join(root, "codegen"))

    from gen_server_hook_list import get_server_hooks
    from cached_server_hook_list import CACHED_SERVER_HOOKS

    found = list(get_server_hooks())   # scans only, caches nothing
    cached = list(CACHED_SERVER_HOOKS)

    if found == cached:
        print(f"ok: {len(found)} hooks, the cached list matches the scanner")
        return 0

    print(f"FAILED: the cached list has {len(cached)} hooks, currently found {len(found)} hooks in server/")
    print()
    for line in list(
        difflib.unified_diff(as_lines(cached), as_lines(found),
        "codegen/cached_server_hook_list.py", "",
        lineterm="")):
        print(line)

    print()
    print("To regenerate the cache: python3 codegen/gen_vklayer_dispatch.py")
    return 1


if __name__ == "__main__":
    sys.exit(main())
