/* SPDX-License-Identifier: GPL-3.0-or-later
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
