#!/usr/bin/env python3
"""The host server's switches for a Mali profile (host/profiles/mali-*.txt), as shell assignments.
    mali_profile_env.py <mali-NAME.txt> <vk.xml>
Uses only switches the bridge already has (server_emu.c):
  VKB_PROFILE_FAKE_MISSING  VKBRIDGE_FAKE_MISSING tokens: a driver extension the profile lacks that the
                            bridge emulates or decides on (divisor, push, dynrend, ...), and the core
                            features it has a token for (bc, clip, cull, dualsrc, fillmode, multiviewport,
                            multidraw): the server then takes the emulation path a Mali takes
  VKB_PROFILE_HIDE_EXTS     VKBRIDGE_SERVER_HIDE_EXTS: every other device extension of the registry the
                            profile lacks (not shown to clients)
  VKB_PROFILE_FAKE_API      VKBRIDGE_FAKE_API: the driver taken as the profile's Vulkan version (what clients are
                            shown, promotion, vidyn), as a 1.1 Mali's server does
  VKB_PROFILE_NOT_IMITATED  the core features the profile lacks that no switch can take away (for the log)"""
import sys, re
import xml.etree.ElementTree as ET

# server_emu.c fake_exts[]: token -> the driver extensions it stands for
EXT_TOKENS = [
    ("divisor", ["VK_EXT_vertex_attribute_divisor", "VK_KHR_vertex_attribute_divisor"]),
    ("depthclip", ["VK_EXT_depth_clip_enable"]),
    ("maint5", ["VK_KHR_maintenance5"]),
    ("maint6", ["VK_KHR_maintenance6"]),
    ("push", ["VK_KHR_push_descriptor"]),
    ("robustness2", ["VK_EXT_robustness2", "VK_KHR_robustness2"]),
    ("loadstorenone", ["VK_KHR_load_store_op_none"]),
    ("dynrend", ["VK_KHR_dynamic_rendering"]),
    ("vidyn", ["VK_EXT_vertex_input_dynamic_state"]),
    ("eds", ["VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2"]),
    ("timeline", ["VK_KHR_timeline_semaphore"]),
]
# server_emu.c: core features with a VKBRIDGE_FAKE_MISSING token
FEATURE_TOKENS = {"textureCompressionBC": "bc", "shaderClipDistance": "clip", "shaderCullDistance": "cull",
                  "dualSrcBlend": "dualsrc", "fillModeNonSolid": "fillmode", "multiViewport": "multiviewport",
                  "multiDrawIndirect": "multidraw"}

prof, xml = sys.argv[1], sys.argv[2]
api, exts, feats = None, set(), {}
for line in open(prof):
    w = line.split()
    if not w or w[0].startswith('#'):
        continue
    if w[0] == 'api':
        api = '.'.join(w[1].split('.')[:2])
    elif w[0] == 'ext':
        exts.add(w[1])
    elif w[0] == 'feature':
        feats[w[1]] = int(w[2])
tokens, covered = [], set()
for tok, names in EXT_TOKENS:
    missing = [n for n in names if n not in exts]
    if len(missing) == len(names):
        tokens.append(tok)
        covered.update(names)
not_imitated = []
for f, v in sorted(feats.items()):
    if v:
        continue
    if f in FEATURE_TOKENS:
        tokens.append(FEATURE_TOKENS[f])
    else:
        not_imitated.append(f)
reg = ET.parse(xml).getroot()
hide = []
for e in reg.find('extensions').findall('extension'):
    if e.get('type') != 'device' or 'vulkan' not in (e.get('supported') or '').split(','):
        continue
    n = e.get('name')
    if n not in exts and n not in covered:
        hide.append(n)
print(f"VKB_PROFILE_FAKE_MISSING={','.join(tokens)}")
print(f"VKB_PROFILE_HIDE_EXTS={','.join(sorted(hide))}")
print(f"VKB_PROFILE_FAKE_API={api}")
print(f"VKB_PROFILE_NOT_IMITATED={','.join(not_imitated)}")
