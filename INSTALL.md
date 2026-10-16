# Trying MaliBridge on your device

MaliBridge runs inside DroidDeck, an Android app that runs the Steam client and PC games. The
test APK on the [Releases page](https://github.com/ASEbrahim/MaliBridge/releases) is DroidDeck
with MaliBridge built in. The bridge is off by default, so turn it on once after installing (step 3).

This is a test build. Two devices have been tested so far (see the table at the end), so expect
rough edges, and please report what you find.

## What you need

- An Android phone or tablet with an **Arm Mali GPU** (MediaTek Dimensity, Google Tensor,
  Samsung Exynos). Snapdragon devices have Adreno GPUs: use the official DroidDeck instead.
- 64-bit Android 8.0 or newer.
- 8 GB of RAM or more. Heavier games need 12 GB or more.
- Storage for Steam (about 2 GB) plus your games.
- A Steam account with the games you want to play.

## Install

1. If the official DroidDeck is installed, uninstall it first. The test APK is signed with a
   different key, so Android won't install it over the official app. Uninstalling removes that
   app's data, including the Steam login and installed games.
2. Download the DroidDeck-MaliBridge `.apk` from the newest release and open it. Android asks
   you to allow installs from your browser or file manager. Allow it.
3. Open DroidDeck, go to **Settings**, and tap **Mali bridge: off** so it reads **Mali bridge: on**.
   The button is only shown on Mali devices, and only while no session is running.
4. Start a **Steam session**. The first start downloads the Linux runtime and the Steam client,
   which takes a few minutes.
5. Sign in to Steam.

## Settings to check

- **Compatibility tool.** In Steam, open a game's Properties → Compatibility and choose
  **Proton Experimental ARM64 (Compatible)**. That is DroidDeck's build, and it is the one that
  works. Valve's own "Proton (ARM64)" entries and "Steam Linux Runtime" also appear in the list,
  but they can't start inside DroidDeck: they need a sandbox that Android doesn't allow.
- **Games with a Linux version too** (Portal 2, Half-Life 2 and others): Steam may pick the Linux
  version, which doesn't start here yet. Set these to Proton Experimental ARM64 (Compatible) by
  hand, as above.
- **In-game graphics settings.** Most games in the tables run on their default settings. The ones
  we changed:

  | Game | Setting |
  |---|---|
  | Risk of Rain 2 | half resolution on the Mali-G720; 1280x720 on the Mali-G925 |
  | The Elder Scrolls V: Skyrim Special Edition | Medium preset |
  | Subnautica | Detail set to High |

- **Shader processing.** When Steam shows "Processing Vulkan shaders" before a game, letting it
  finish makes the first run smoother.

## First games to try

Start with something light. These run well:

| Game | Mali-G720 | Mali-G925 |
|---|---|---|
| Hollow Knight | 87-124 fps | runs well (player report) |
| Hollow Knight: Silksong | steady 62-66 fps | 72-96 fps |
| Celeste | 57-59 fps | not tested |
| Portal | 80 fps | not tested |
| Half-Life 2 | 59-74 fps | 95-103 fps |
| Inscryption | 68-83 fps | not tested |
| Stardew Valley | 60 fps | 57-62 fps |

## How demanding each game is

A rough guide from our testing, mostly on the Mali-G720. A newer GPU (the G925 runs these
1.4-2x faster) moves games up a group; a phone with less free memory can move them down.

| Group | Games | What to expect |
|---|---|---|
| Easy | Stardew Valley, Celeste, Hollow Knight, Hollow Knight: Silksong, Inscryption, Doki Doki Literature Club, Portal, Hades, Cuphead, Dead Cells | Smooth on default settings. Good first tests. |
| Medium | Half-Life 2, Portal 2, ULTRAKILL, ENDER LILIES, Lethal Company, Skyrim Special Edition | Playable, 40-75 fps on the G720. Portal 2 and Half-Life 2 need the compatibility tool set by hand (see above); Skyrim on Medium. |
| Hard | Subnautica, Risk of Rain 2 | Run, about 20-30 fps on the G720. Lower the resolution if needed; 12 GB of RAM or more helps. |
| Very hard | The Witcher 3 (DX12), Stray, Deep Rock Galactic | Start, but run slowly, and on the Mali-G925 Stray and The Witcher 3 stop early (a known issue). Expect problems. |

The README's games table has the measured frame rates for each game.

The first time you start Steam, and the first time you start each game, things can feel choppy
while shaders are compiled and cached. It gets smoother the more you play. When Steam shows
"Processing Vulkan shaders" before a game starts, letting it finish fills that game's shader
cache, so the first run stutters less. Skipping it is fine too.

## If something goes wrong

Let the game close, then tap **Stop session** in the side menu. Then, in DroidDeck's
**Settings**, tap **Share logs** next to **Latest session logs**. This creates a zip of the
session's logs. Stopping the session first matters: if Android closed the game because it ran
out of memory, that is only written to the logs when the session ends. Launch the game once per
session, so the logs show a single attempt. Attach the zip to a [GitHub issue](https://github.com/ASEbrahim/MaliBridge/issues) and include:

- your device model, and
- what you did and what happened, for example "Steam never appeared" or "the game closed
  right after starting".

The logs include the bridge's start-up self-test (`== vkbridge:` lines). On a working device it
ends with `OK: 13 of 13 checks passed`.

## Tested devices

| Device | GPU | Driver | Status |
|---|---|---|---|
| Samsung Galaxy Tab S10+ (SM-X826B) | Mali-G720 Immortalis MC12 (Dimensity 9300+) | r44p1 | tested in depth |
| Samsung Galaxy Tab S11 Ultra (SM-X936B) | Mali-G925 Immortalis MC12 (Dimensity 9400) | Android 16 (Vulkan 1.3.278) | works; games run 1.4-2x faster than on the S10+. Stray, and The Witcher 3 after its intro: known issue, being worked on |
| Google Pixel 9 | Mali-G715 (Tensor G4) | r54p3 | Steam runs and the self-test passes (v0.4, player report); Portal 2 closes soon after starting, being looked into |

Reports from other Mali GPUs (G57, G68, G610, G615, G710, G715, ...) are very welcome.
