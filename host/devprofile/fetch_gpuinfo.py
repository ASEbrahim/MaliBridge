#!/usr/bin/env python3
"""Fetch a vulkan.gpuinfo.org report as a Khronos profiles JSON, with the identity filled in.

    fetch_gpuinfo.py <report id> <out.json> [--name "Mali-G715"]

Also the report's memory heaps and types, as "x-malibridge" (devprofile_check.sh picks the
bridge's own imitation of that memory table when it has one).
gpuinfo's profile export leaves deviceName, deviceID, driverVersion and deviceType out of
VkPhysicalDeviceProperties; the bridge's per-GPU paths key on those (and on driverInfo, which the
export has). They are filled in from the report's listing (GPU name, driver "54.3.0" ->
VK_MAKE_VERSION(54, 3, 0), as Arm encodes it) unless the profile already has them.
"""
import html, json, re, sys, urllib.parse, urllib.request

MEMFLAGS = {"DEVICE_LOCAL_BIT": 0x1, "HOST_VISIBLE_BIT": 0x2, "HOST_COHERENT_BIT": 0x4, "HOST_CACHED_BIT": 0x8,
            "LAZILY_ALLOCATED_BIT": 0x10, "PROTECTED_BIT": 0x20}

def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "malibridge-devprofile"}), timeout=120) as r:
        return r.read()

def listing(rid):
    # the report's row in the listing (GPU name, driver); the listing has no id filter, so page down to it
    for start in range(0, 20000, 100):
        q = urllib.parse.urlencode({"start": start, "length": 100, "draw": 1, "order[0][column]": 0,
                                    "order[0][dir]": "desc", "columns[0][data]": "id", "filter[platform]": "android"})
        rows = json.loads(get("https://vulkan.gpuinfo.org/api/internal/reports.php?" + q))["data"]
        if not rows: return None
        for r in rows:
            if r["id"] == rid: return r
        if rows[-1]["id"] < rid: return None
    return None

def memory(rid):
    """The report's memory heaps and types (the profile export has none), from its page"""
    page = get(f"https://vulkan.gpuinfo.org/displayreport.php?id={rid}").decode("utf8", "replace")
    i = page.find("id='memorytypes'")
    if i < 0: return None
    seg = page[i:page.find("</tody>", i)]
    heaps, types = [], []
    for key, val, heap in re.findall(r"<td class='subkey'[^>]*>([^<]*)</td><td>(.*?)</td><td>Memory heap (\d+)</td>", seg):
        flags = sum(MEMFLAGS.get(f, 0) for f in re.findall(r"(\w+_BIT)", val))
        if key == "Flags": heaps.append({"flags": flags})
        elif key == "Size" and heaps: heaps[-1]["size"] = int(val.split()[0].replace(",", ""))
        elif key.startswith("Memory type"): types.append({"propertyFlags": flags, "heapIndex": int(heap)})
    return {"memoryHeaps": heaps, "memoryTypes": types}


def main():
    a = sys.argv[1:]
    if len(a) < 2: sys.exit(__doc__)
    rid, out = int(a[0]), a[1]
    name = a[a.index("--name") + 1] if "--name" in a else None
    d = json.loads(get(f"https://vulkan.gpuinfo.org/api/v3/getprofile.php?id={rid}"))
    caps = next(iter(d["capabilities"].values()))
    prof = next(iter(d["profiles"].values()))
    row = None if name else listing(rid)
    if row: name = row["gpuname"]
    m = re.search(r"driver (\d+)\.(\d+)\.(\d+)", prof.get("label", ""))
    props = caps["properties"].setdefault("VkPhysicalDeviceProperties", {})
    if name: props.setdefault("deviceName", name)
    if m:
        ma, mi, pa = map(int, m.groups())
        props.setdefault("driverVersion", (ma << 22) | (mi << 12) | pa)
    props.setdefault("deviceType", "VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU")
    props.setdefault("deviceID", 0)
    mem = memory(rid)
    # not a profiles field (the layer does not imitate memory): devprofile_check.sh reads it
    if mem: d["x-malibridge"] = {"gpuinfo_report": rid, "memory": mem}
    prof["description"] = (prof.get("description", "") + f" (gpuinfo report {rid})").strip()
    json.dump(d, open(out, "w"), indent=1)
    print(f"{out}: {prof.get('label')}, {name}, Vulkan {prof.get('api-version')}")

if __name__ == "__main__":
    main()
