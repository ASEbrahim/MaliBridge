/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built-in per-game presets (server_presets.c), compiled into the server. One entry per Steam app:
 *
 *   {"<appid>", "KEY=VALUE KEY=VALUE", "why, with the evidence (game, GPU, driver, date)"},
 *
 * KEY is one of the per-game keys server_presets.c lists (the same names as in <cache dir>/env);
 * values have no spaces. A tester's <cache dir>/presets/<appid>.env beats an entry here key by key,
 * and <cache dir>/env or the server's environment beat both. Keep it to settings a test showed a
 * game needs; everything else stays the mode's default.
 *
 * Example (not active):
 *   {"413150", "VKBRIDGE_BC_TRANSCODE_QUALITY=fast", "Stardew Valley: ..."},
 */
