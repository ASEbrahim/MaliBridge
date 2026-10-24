#!/usr/bin/env python3
"""Adds to a profile the device extensions a tester's vkbridge-server.log lists ("all:" lines) and
the profile lacks, and checks the log's core features against the profile's.
    merge_server_log.py <profile.json> <vkbridge-server.log> <out.json>
gpuinfo's export leaves out extensions its reporting app did not know (a 2026 Mali r54/r56 driver
lists 15-45 more than the export has); the bridge's log lists every one. Their feature structs stay
as the profile has them (absent: the desktop driver's values)."""
import json, re, sys

prof, log, out = sys.argv[1:4]
d = json.load(open(prof))
caps = next(iter(d["capabilities"].values()))
ext = caps["extensions"]
text = open(log, errors="replace").read()
added = []
for line in text.splitlines():
    if "  all: " not in line: continue
    for tok in line.split("  all: ", 1)[1].split():
        name, _, ver = tok.rpartition(":")
        name = "VK_" + name
        if name not in ext:
            ext[name] = int(ver) if ver.isdigit() else 1
            added.append(name)
# the log's core features against the profile's
core = caps["features"].get("VkPhysicalDeviceFeatures", {})
diff = []
for m in re.finditer(r"(\w+)=([01])", " ".join(l for l in text.splitlines() if " features: " in l)):
    k, v = m.group(1), m.group(2) == "1"
    if k in core and bool(core[k]) != v: diff.append(f"{k}: profile {core[k]}, log {v}")
gpu = re.search(r"GPU: (.*?) \(vendor 0x13b5 device (0x[0-9a-f]+), Vulkan ([\d.]+), driver (0x[0-9a-f]+)\)", text)
props = caps["properties"].setdefault("VkPhysicalDeviceProperties", {})
if gpu and props.get("deviceID", 0) == 0: props["deviceID"] = int(gpu.group(2), 16)
d.setdefault("x-malibridge", {})["merged_server_log"] = {"extensions_added": added, "feature_differences": diff}
json.dump(d, open(out, "w"), indent=1)
print(f"{out}: {len(added)} extension(s) added from the log; core features differing: {diff or 'none'}"
      + (f"; log GPU {gpu.group(1)}, Vulkan {gpu.group(3)}, driver {gpu.group(4)}" if gpu else ""))
