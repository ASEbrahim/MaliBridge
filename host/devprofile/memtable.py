#!/usr/bin/env python3
"""The bridge's imitation (VKBRIDGE_FAKE_MISSING token) of a profile's memory table, or nothing.
server_memory.c: g720mem is the G720's/G925's 0x7 0xb 0x11 [0x21/heap 1], which most Mali drivers
report; xringmem the XRing O3's 22 types. The table is the profile's "x-malibridge" memory types."""
import json, sys
t = [m["propertyFlags"] for m in (json.load(open(sys.argv[1])).get("x-malibridge", {}).get("memory", {}).get("memoryTypes") or [])]
x = [7, 0xb, 0x11] + [7] * 6 + [0xb] * 6 + [0x11] * 6 + [0x21]
print("xringmem" if t == x else "g720mem" if t in ([7, 0xb, 0x11], [7, 0xb, 0x11, 0x21]) else "")
