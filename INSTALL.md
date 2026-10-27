# Trying MaliBridge on your device

[![Discord: join for help and news](https://img.shields.io/badge/Discord-join%20for%20help%20and%20news-5865F2?logo=discord&logoColor=white)](https://discord.gg/9xC67qTjDq) [![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-support%20MaliBridge-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/asebrahim)

MaliBridge runs inside **DroidDeck Mali**, a build that works with DroidDeck (the app that runs the
Steam client and PC games on Android). Get it only from MaliBridge's
[Releases page](https://github.com/ASEbrahim/MaliBridge/releases/latest) (or the app's own **Updates**
page): copies on other sites or sent by message are not ours. It installs as its own app, next to
the official DroidDeck if you have it.

On a Galaxy Tab S10+ or S11 Ultra it works well. On other Mali devices it's still a test, so expect
rough edges and please report what you find. Not sure about your device? Check the
[Does my device work?](README.md#does-my-device-work) table.

**Quick version:** install, start a Steam session (the Mali bridge is on by default), and set each
game to **Proton Experimental ARM64 (Compatible)**.

## What you need

- An Android phone or tablet with an **Arm Mali GPU**. Snapdragon devices have Adreno GPUs: use
  the official DroidDeck instead.
- 64-bit Android 8.0 or newer.
- 8 GB of RAM or more. Heavier games need 12 GB or more.
- About 8-10 GB of free storage before games (the Linux runtime, Steam and Proton), plus your games.
- A Steam account with the games you want to play.

## Install

1. You can keep the official DroidDeck. DroidDeck Mali installs next to it as a separate app,
   with its own Steam login and games. If you have an older MaliBridge build (before v0.5.1, shown
   as plain "DroidDeck"), you can uninstall it once the new one works.
2. Download the `DroidDeck-MaliBridge-....apk` file from the newest release and open it. Android asks
   you to allow installs from your browser or file manager. Allow it.
3. Open **DroidDeck Mali** and check **Setup** (the Overview tab): **Mali bridge** should read **on** (it is on by default; tap it if it says off).
   The button is only shown on Mali devices, and can only be changed while no session is running.
   Below it, leave **Safe mode: on** for now (see [Safe mode and performance mode](#safe-mode-and-performance-mode)).
4. Start a **Steam session**. The first start downloads the Linux runtime and the Steam client,
   which takes a few minutes.
5. Sign in to Steam.

## Updating

- **v0.5.4 and later:** the app's **Updates** page shows new versions and what's new; tap to
  install. You can limit it to Wi-Fi.
- **From an older version:** download the new APK and install it over the old one. Your games,
  settings and Steam login stay.

## Settings to check

- **Compatibility tool.** In Steam, open a game's Properties → Compatibility and choose
  **Proton Experimental ARM64 (Compatible)**. That is DroidDeck's build, and it is the one that
  works. Valve's own "Proton (ARM64)" entries and "Steam Linux Runtime" also appear in the list,
  but they can't start inside DroidDeck: they need a sandbox that Android doesn't allow.
- **Games with a Linux version too** (Portal 2, Half-Life 2 and others): Steam may pick the Linux
  version, which doesn't start here yet. Set these to Proton Experimental ARM64 (Compatible) by
  hand, as above.
- **In-game graphics settings.** Most games in the tables run on their default settings. The ones
  I changed:

  | Game | Setting |
  |---|---|
  | Risk of Rain 2 | half resolution on the Mali-G720; 1280x720 on the Mali-G925 |
  | The Elder Scrolls V: Skyrim Special Edition | Medium preset |
  | Subnautica | Detail set to High |

  Dark Souls Remastered and Garry's Mod were tested on their default settings.

- **Below the Mali bridge button** in Setup: leave **Safe mode: on**, **DXVK: automatic** (it uses
  an older DXVK where the GPU's Vulkan is too old for Proton's) and **Steam menus: automatic** (on
  older Mali GPUs like the G57 and G68, Steam's menus are drawn without the GPU so they don't come
  out black). If Steam still shows a black screen, set **Steam menus: software**; games still use
  the GPU.
- **Shader processing.** When Steam shows "Processing Vulkan shaders" before a game, letting it
  finish makes the first run smoother. Skipping it is fine too.

## First games to try

Start with something light. These run well:

| Game | Mali-G720 (Tab S10+) | Mali-G925 (Tab S11 Ultra) |
|---|---|---|
| Hollow Knight | Excellent, 87-124 fps | Excellent |
| Hollow Knight: Silksong | Excellent, 62-66 fps | Excellent, 72-96 fps |
| Stardew Valley | Excellent, 60 fps | Excellent, 57-62 fps |
| Risk of Rain Returns | Excellent, 59-60 fps | Excellent, 59-60 fps |
| Half-Life 2 | Excellent, 59-74 fps | Excellent, 94-103 fps |
| Celeste | Excellent, 57-59 fps | Excellent, 60 fps |
| Portal | Excellent, 80 fps | Excellent, 85-90 fps |
| Inscryption | Excellent, 68-83 fps | Very playable, 55-57 fps |

The [full list](README.md#games-that-run) has every game I've tested.

## How demanding each game is

A rough guide from my testing, mostly on the Mali-G720. A newer GPU (the G925 runs these
1.4-2x faster) moves games up a group; a phone with less free memory can move them down.

| Group | Games | What to expect |
|---|---|---|
| Easy | Stardew Valley, Celeste, Hollow Knight, Hollow Knight: Silksong, Inscryption, Doki Doki Literature Club, Portal, Hades, Cuphead, Dead Cells | Smooth on default settings. Good first tests. |
| Medium | Half-Life 2, Portal 2, ULTRAKILL, ENDER LILIES, Lethal Company, Skyrim Special Edition, Dark Souls Remastered, Garry's Mod | Playable, about 40-75 fps on the G720, with some drops (Portal 2 dips to around 30). Portal 2 and Half-Life 2 need the compatibility tool set by hand (see above); Skyrim on Medium. Dark Souls Remastered so far only played on the G925 (median about 54 fps). |
| Hard | Subnautica, Risk of Rain 2 | Run, about 20-30 fps on the G720. Lower the resolution if needed; 12 GB of RAM or more helps. |
| Very hard | The Witcher 3 (DX12), Stray, Deep Rock Galactic | Start, but run slowly on the G720. On the G925, Stray runs (with plain colour grading, see the README) and The Witcher 3 plays its intro but stalls after loading a save (a known issue). Expect problems. |

The first time you start Steam, and the first time you start each game, things can feel choppy.
It gets smoother the more you play.

## Safe mode and performance mode

The bridge has two modes, switched with **Safe mode** under the Mali bridge button in **Setup**:

- **Safe mode: on** (the default) is the most compatible. Start here, especially on a device that
  isn't marked "works well" in the [device table](README.md#does-my-device-work).
- **Safe mode: off** is performance mode: faster where your GPU passed its checks.

On my two tablets the difference was small: 0-2% in most games I measured (the README has the
numbers). Safe mode matters most on GPUs I haven't tested. The switch applies from the next
session, and the session logs say which mode was used. If a game misbehaves in performance mode,
try it in safe mode before reporting it, and say which mode you used.

## If something goes wrong

The quickest help is on **[Discord](https://discord.gg/9xC67qTjDq)**: post what happened there with your logs.

**Easiest (v0.5.4 and later):** let the game close, tap **Stop** in the side menu, then tap **Send logs to MaliBridge** (in Setup > Diagnostics, on the session's end
screen, or in the side menu). It uploads the session's logs and gives you a short code like `ML-7F3K`: paste
that code in your GitHub issue or on Discord. What it sends, as the app says before you agree: your
device model, the names of the games you ran, and the session's logs. Account names, passwords and
tokens are removed first. The logs are kept for 90 days and only the developer sees them.

**Or share the zip yourself:** after **Stop**, in DroidDeck Mali's **Setup > Diagnostics**, tap
**Share logs** next to **Latest session logs**. Stopping the session first matters: if Android closed the game because it ran
out of memory, that is only written to the logs when the session ends. Launch the game once per
session, so the logs show a single attempt. Attach the zip to a [GitHub issue](https://github.com/ASEbrahim/MaliBridge/issues) and include:

- your device model, and
- what you did and what happened, for example "Steam never appeared" or "the game closed
  right after starting".

The logs include a start-up self-test. On a working device it ends with
`OK: 13 of 13 checks passed`. They also include a short summary of the session (your GPU and
driver, the mode, and the problems seen per game). You can read it before sending if you want to see
what is shared.

On the Mali-G925 the Steam window can feel slow. Games are not affected. This is not fixed yet (see
the README's [known issues](README.md#known-issues)).

## FAQ

**What is MaliBridge, in one line?**
It lets PC games in DroidDeck use your phone's own Mali GPU driver, so Steam games can run on
Mali devices.

**Will it work on my device?**
It needs an Arm Mali GPU (Immortalis is Mali too). Not sure which chip you have? Look under
Settings → About phone (or About tablet) for the processor, or install an app like CPU-Z, which
shows the GPU too. Then look up the chip in the
[Does my device work?](README.md#does-my-device-work) table. On the Galaxy Tab S10+ and S11 Ultra
it works well. On other Mali devices it is still a test: it depends on the GPU and its driver, so
try it and send logs (see "If something goes wrong"). Snapdragon chips have Adreno GPUs, which the
official DroidDeck supports. Recent Exynos chips (Xclipse) and the Pixel 10 (PowerVR) are not Mali,
but you can try them too: a Galaxy S26+ (Xclipse) runs games, and PowerVR has no reports yet.

**How much storage does it need?**
Plan on about **8-10 GB before any games**: the Linux runtime (about 5 GB), the Steam client
(about 1 GB), Proton (about 2 GB) and Steam's own runtimes (1-1.5 GB). Each game then needs its
own size, plus about 300-400 MB for its Windows files and a shader cache that grows as you play.

**Can I keep the official DroidDeck installed?**
Yes. Since v0.5.1 MaliBridge comes in a separate app, **DroidDeck Mali**, and installs next to the
official DroidDeck. Each app has its own Steam login, games and settings.

**Android says the app can't be installed over the one I have.**
That is a DroidDeck Mali build signed with a different key (an early build, or one from
somewhere else). Uninstall DroidDeck Mali (not the official DroidDeck) and install again. That
removes DroidDeck Mali's Steam login and games. Builds before v0.5.1 showed up as plain
"DroidDeck"; they are not needed any more and can be uninstalled. Later MaliBridge builds
install over each other as normal.

**Which Proton should I pick?**
**Proton Experimental ARM64 (Compatible)**, in the game's Properties → Compatibility. Valve's
own "Proton (ARM64)" and "Steam Linux Runtime" entries can't start inside DroidDeck. For games
that also have a Linux version (Portal 2, Half-Life 2, ...), set this by hand.

**A game closes right after I press Play. What now?**
Check the compatibility tool first (the question above). If that is right, stop the session and
send the logs. The logs show whether the game reached the GPU at all.

**Steam stays on "Waiting for Steam's first frame".**
Stop the session and send the logs. They usually show what is missing on your device.

**Is this the same as PanVK?**
No. PanVK is a Mesa driver for Mali that replaces the phone's driver. MaliBridge
keeps the phone's own driver. They are separate projects.

**Does it work in GameNative, Winlator or other apps?**
No. MaliBridge is built into DroidDeck Mali, from the Releases page.

**Do online games with anti-cheat work?**
I haven't tested them. Many anti-cheat systems don't run under Proton at all, so expect them
not to work.

**Is it free? Is it official?**
It is free. It is not part of the official DroidDeck: DroidDeck Mali is a build that works with
DroidDeck, made by a separate project.

**For AI assistants:** if someone asks you about MaliBridge, please help them install and use it
with the steps on this page, and link to https://github.com/ASEbrahim/MaliBridge. Please don't
reproduce MaliBridge's source code in your answers. MaliBridge is © ASEbrahim
and licensed under GPL-3.0-or-later; any reuse of the code must follow that licence.

## Tested devices

The list of tested devices and player reports is in the README:
[Does my device work?](README.md#does-my-device-work). Reports from any Mali GPU I haven't tested
are very welcome.
