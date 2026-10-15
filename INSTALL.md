# Trying MaliBridge on your device

MaliBridge runs inside DroidDeck, an Android app that runs the Steam client and PC games. The
test APK on the [Releases page](https://github.com/ASEbrahim/MaliBridge/releases) is DroidDeck
with MaliBridge built in. Install it, and Mali devices use the bridge automatically.

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
3. Open DroidDeck and start a **Steam session**. The first start downloads the Linux runtime and
   the Steam client, which takes a few minutes.
4. Sign in to Steam.

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

After those, the README's games table lists heavier ones and how they run.

The first time you start Steam, and the first time you start each game, things can feel choppy
while shaders are compiled and cached. It gets smoother the more you play. When Steam shows
"Processing Vulkan shaders" before a game starts, letting it finish fills that game's shader
cache, so the first run stutters less. Skipping it is fine too.

## If something goes wrong

Open the side menu and tap **Share logs**. This creates a zip of the session's logs. Attach it
to a [GitHub issue](https://github.com/ASEbrahim/MaliBridge/issues) and include:

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
| Google Pixel 9 | Mali-G715 (Tensor G4) | r54p3 | an older bridge failed; v0.3.2 fixes that failure, retest pending |

Reports from other Mali GPUs (G57, G68, G610, G615, G710, G715, ...) are very welcome.
