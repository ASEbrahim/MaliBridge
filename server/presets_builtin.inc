/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Built-in per-game presets (server_presets.c), compiled into the server. One entry per Steam app:
 *
 *   {"<appid>", "<gpu>,<gpu>", "KEY=VALUE KEY=VALUE", "why, with the evidence (game, GPU, driver, date)"},
 *
 * <gpu>: the GPUs the entry was tested on, each a substring of the deviceName ("Mali-G720"). The
 * entry is applied only when a GPU the server drives has one of them in its name; on any other
 * GPU, or with "" (none listed), it is not, and the server says so once with the lines to copy
 * into <cache dir>/presets/<appid>.env to try them. KEY is one of the per-game keys
 * server_presets.c lists (the same names as in <cache dir>/env); values have no spaces. A user's
 * <cache dir>/presets/<appid>.env beats everything, then <cache dir>/env and the server's
 * environment beat an entry here key by key. Keep it to settings a test showed a game needs;
 * everything else stays the mode's default.
 *
 * Example (not active):
 *   {"413150", "Mali-G720,Mali-G925", "VKBRIDGE_BC_TRANSCODE_QUALITY=fast", "Stardew Valley: ..."},
 */

/* Untested here (no GPU listed: not applied anywhere yet, only said with the lines to try). From
 * GameNative's crowd configs for Mali (2026-10-18; their data, our entries): most good runs cap
 * the device memory at 4096 MB. Move a GPU into the list once a run on our tablets shows it. */
{"4000", "", "VKBRIDGE_HEAP_MAX_MB=4096",
 "Garry's Mod: 32 of 51 good Mali runs in GameNative's configs use a 4096 MB device memory cap (G925 5 stars); untested here"},
{"570940", "", "VKBRIDGE_HEAP_MAX_MB=4096 VKBRIDGE_BC_TRANSCODE_QUALITY=fast",
 "DARK SOULS: REMASTERED: GameNative's G720 runs at 4096 MB; stutter and slow area loads reported (fast BC transcode); untested here"},
{"1721470", "", "VKBRIDGE_HEAP_MAX_MB=4096 VKBRIDGE_BC_TRANSCODE_QUALITY=fast",
 "Poppy Playtime (UE4): 39 of 79 good Mali runs in GameNative's configs at 4096 MB; fast BC transcode for load times; untested here"},
{"242760", "", "VKBRIDGE_HEAP_MAX_MB=4096",
 "The Forest: missing textures and crashes in GameNative's Mali runs; a 4096 MB cap to try; untested here"},

/* RE Engine: compute shaders that write groupshared memory and read other threads' values with no
 * barrier, counting on 32-wide subgroups in lockstep (RE2's occlusion culling: Mesa MR 23408, the
 * flicker on Intel; vkd3d-proton carries subgroup-size quirks for re2.exe/re7.exe). Mali's
 * subgroups are 16 wide (G925: min = max = 16), so a 32-wide size cannot be required there;
 * shm_barrier puts the barriers in instead, level 2: also inside workgroup-uniform loops and ifs
 * and between a loop's iterations (DXVK's forceComputeLdsBarriers, top level and after writes
 * only, made RE2's flicker on the G925 steady but left the porch roof, gate pillars and
 * spindles culled). On the DX12 builds (vkd3d-proton) the shaders may also use wave operations
 * that count on 32 lanes, which no barrier fixes: wave32 emulates 32-lane subgroups in those
 * (D3D11 shaders have none, so it changes nothing on the DX11 builds). Untested here: G925 shows
 * flashing textures and vanishing geometry (DX11) and missing lighting (DX12 RT), G720 pink
 * objects (DX12 RT). */
/* RE2 DX11 on the G925 (r49p1, 2026-10-20, STORY screen): the flicker is cs.58af2d48 (app hash 2a0debecc286be40), a
 * 32-thread draw-list compaction whose ballot is done in shared memory with no barrier; cs.889d027a (e28ac7ddfa639dc4) is
 * the same kind. Their loops exit per thread, so the barriers need the lockstep rewrite first (spirv_lockstep.h): 231 blink
 * events / 10 s -> 0, geometry kept, fps the same (50.5). DXVK's forceComputeLdsBarriers also stops the flicker, but its
 * Subgroup-scope barriers inside cs.4605df31's `if (tid < 32)` (f9c93179c84d860f, a frustum/bounds reduction) cull the
 * porch roof, gate pillars and banner for good. The hashes are of DXVK 3.1.1's code for those shaders. */
{"883710", "Mali-G925", "VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_ONLY=e28ac7ddfa639dc4,2a0debecc286be40 VKBRIDGE_SHARED_BARRIER_LOCKSTEP=e28ac7ddfa639dc4,2a0debecc286be40",
 "Resident Evil 2 (DX11): draw-list compaction made for 32-wide waves races on 16-wide subgroups (flicker); barriers after the lockstep rewrite, on the two shaders"},
{"883710", "", "VKBRIDGE_WAVE32=1",
 "Resident Evil 2 (DX12 builds): wave operations that may count on 32 lanes; untested here"},
{"952060", "", "VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_WAVE32=1",
 "Resident Evil 3: RE Engine like RE2 (DXVK's RE2 profile covers it too); untested here"},
{"418370", "", "VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_WAVE32=1",
 "Resident Evil 7: RE Engine, subgroup-size quirk in vkd3d-proton like RE2's; untested here"},
{"601150", "", "VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_WAVE32=1",
 "Devil May Cry 5: RE Engine (DXVK groups it with RE2/RE3); untested here"},
