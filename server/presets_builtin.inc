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
