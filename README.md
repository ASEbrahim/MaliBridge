# MaliBridge

**Play Steam and PC games on Android phones and tablets with a Mali GPU**: MediaTek Dimensity,
Google Tensor, Xiaomi XRing, older Samsung Exynos and other Arm Mali / Immortalis chips.
MaliBridge runs inside DroidDeck and uses your device's own Mali driver, so no custom GPU driver
is needed.

**[Download](https://github.com/ASEbrahim/MaliBridge/releases/latest)** ·
**[Install guide](INSTALL.md)** · **[Does my device work?](#does-my-device-work)** ·
**[Games that run](#games-that-run)** · **[FAQ](INSTALL.md#faq)** ·
**[Report a problem](https://github.com/ASEbrahim/MaliBridge/issues)**

On a Samsung Galaxy Tab S10+ (Mali-G720) and Galaxy Tab S11 Ultra (Mali-G925), 31 games I've
tested are playable or better, for example Half-Life 2 at about 94 fps, Hollow Knight: Silksong,
Stardew Valley, Risk of Rain Returns, Dark Souls Remastered and Garry's Mod. It is free and open
source.

## Does my device work?

You need an Arm Mali GPU. Find your device below, or look up its chip in the second table. Not sure
which chip you have? It's under Settings → About phone (or About tablet), or in an app like CPU-Z.

**Tested**

| Device | Chip / GPU | Driver | Result |
|---|---|---|---|
| Samsung Galaxy Tab S10+ | Dimensity 9300+ / Immortalis-G720 MC12 | r44p1 | **Works well** (tested in depth) |
| Samsung Galaxy Tab S11 Ultra | Dimensity 9400 / Immortalis-G925 MC12 | r49p1 | **Works well** (tested in depth) |
| Xiaomi 18 Fold | XRing O3 / Mali-G2-Ultra-NX MC16 | r56p0 | Player reports: works since v0.5.2 (self-test 13/13, Steam, Brotato about 77 fps) |
| Google Pixel 9 | Tensor G4 / Mali-G715 | r54p3 | Player report: Steam starts; games not confirmed yet |
| Google Pixel 8 / 8 Pro | Tensor G3 / Mali-G715 | r54 | Player reports: games run (Cuphead, Hades, Balatro, Dishonored) |
| vivo X200 | Dimensity 9400 / Immortalis-G925 MC12 | | Player report: games run (Euro Truck Simulator 2, CarX Drift Racing Online) |
| OPPO Find X9 Pro and others | Dimensity 9500 / Mali-G1-Ultra MC12 | r54p1 | Player reports: games run since v0.5.2 (Balatro, Slay the Spire) |
| OnePlus Ace 2V (Dimensity 9000) | Mali-G710 | r32p1 (Vulkan 1.1) | Player report: Half-Life 2 starts and RimWorld plays for 1-2 hours with DXVK 1.10.3 |
| POCO X6 Pro | Dimensity 8300-Ultra / Mali-G615 MC6 | | Player report: lighter games run (GTA San Andreas, Burnout Paradise) |
| realme Neo7 | Dimensity 9300+ / Immortalis-G720 MC12 | r44p1 | Player report: Stardew Valley runs; Balatro runs with its text missing |
| Dimensity 8350 phone | Mali-G615 | | Player report: Rivals of Aether about 55 fps |
| Xiaomi Pad 9 Pro Max | XRing O3 | | Player report: Human Fall Flat works, Portal 2 40-50 fps at 1080p |
| Xiaomi 17T Pro | Dimensity 9500 / Mali-G1-Ultra MC12 | r54p1 | Player report: Hollow Knight, Silksong, Hades, Celeste and Portal run |
| Xiaomi 14T Pro | Dimensity 9300+ / Immortalis-G720 MC12 | r44p1 | Player report: Steam, controller and downloads work; Alan Wake 20-50 fps on Low |
| POCO X8 Pro | Dimensity 8500 / Mali-G720 MC8 | r49p1 | Player reports: Indiana Jones and the Emperor's Tomb about 30 fps; Hob plays with some freezes |
| Redmi Note 12S | Helio G96 / Mali-G57 MC2 | | Player report: first G57 report; Steam works first try, lighter games run |

**Help wanted: not tested yet.** Reports from these would help the most:

| Chip | GPU | Example devices |
|---|---|---|
| Dimensity 9300 / 9300+ (other devices) | Immortalis-G720 MC12 | vivo X100 Pro |
| Dimensity 9400 / 9400+ (other devices) | Immortalis-G925 MC12 | OPPO Find X8 |
| Dimensity 8400 | Mali-G720 MC7 | POCO X7 Pro |
| Dimensity 9000 / 9200 | Mali-G710 / Immortalis-G715 | OnePlus Ace 2V |
| Dimensity 7300, 8100, 8200, Helio G99 and similar | Mali-G615 / G610 / G57 | many mid-range phones (smaller GPUs: expect light games only) |
| Exynos 1280 / 1380 / 2100 | Mali-G68 / G78 | Galaxy A53, A54, S21 |

Devices with the same chip as a tested one will likely behave similarly, but phone makers ship
different driver versions, so it's still worth a report.

**Not Mali:** Snapdragon chips have Adreno GPUs (use the official DroidDeck). Recent Samsung Exynos
chips (2200, 2400, 2500) have Xclipse GPUs, and the Pixel 10's Tensor G5 has a PowerVR GPU; MaliBridge
doesn't support these.

On a device that isn't in the first table, it's still a test: it depends on the GPU and its driver.
Logs from your device help a lot (see [If something goes wrong](INSTALL.md#if-something-goes-wrong)).

## Get started

1. Check your chip in the table above.
2. Download `DroidDeck-MaliBridge-...-test.apk` from the [latest release](https://github.com/ASEbrahim/MaliBridge/releases/latest) and install it. It shows up as **DroidDeck Mali**.
3. Open DroidDeck Mali (the **Mali bridge** is on by default; check Settings if unsure), start a **Steam** session and sign in.
4. For each game, pick **Proton Experimental ARM64 (Compatible)** in its Properties → Compatibility.

The [install guide](INSTALL.md) has every step, the settings to check, and which games to try first.

<a id="status"></a>
## Games that run

These are the games I've played on my two tablets, a Galaxy Tab S10+ (Mali-G720) and a Galaxy
Tab S11 Ultra (Mali-G925), with the settings from the [install guide](INSTALL.md#settings-to-check).
The S11 Ultra runs the same game about 1.4-2x faster. On other devices, expect results in line with
how strong the GPU is.

Ratings, from worst to best: barely runs, kinda playable, playable, very playable, excellent. They come from
playing the games myself where I did, and from the frame rate otherwise.

| Game | Mali-G720 (Tab S10+) | Mali-G925 (Tab S11 Ultra) |
|---|---|---|
| Hades | **Excellent** | not tested |
| Hollow Knight | **Excellent**, 87-124 fps | **Excellent** (player report) |
| ULTRAKILL | **Very playable**, 55-57 fps | not tested |
| Celeste | **Excellent**, 57-59 fps | not tested |
| Portal | **Excellent**, 80 fps | not tested |
| Cuphead | **Excellent** | not tested |
| Dead Cells | **Excellent** | not tested |
| ENDER LILIES | **Playable**, 42-44 fps | not tested |
| Lethal Company | **Playable**, 43-45 fps | not tested |
| Subnautica | **Playable**, 20-24 fps with Detail set to High | **Very playable**, 28-40 fps in play (while thermally throttled), 81 fps on the menu |
| Inscryption | **Excellent**, 68-83 fps | not tested |
| Doki Doki Literature Club | **Excellent**, title screen 91-92 fps | not tested |
| Half-Life 2 | **Excellent**, 59-74 fps in the opening chapter | **Excellent**, 94-103 fps in the opening chapter |
| Portal 2 | **Playable**, about 45-50 fps in play with drops to around 30 | **Not rated yet** (menu only): 115 fps in the menu |
| Hollow Knight: Silksong | **Excellent**, a steady 62-66 fps in play | **Excellent**, 72-96 fps |
| The Elder Scrolls V: Skyrim Special Edition | **Excellent**, about 60 fps (its cap) on Medium; smooth in third person, some stutter on first load | not tested |
| Stardew Valley | **Excellent**, 60 fps (its cap) on the title screen | **Excellent**, 57-62 fps |
| Risk of Rain 2 | **Kinda playable**, at half resolution: 18-30 fps through a boss fight with crowds and effects (15-23 fps at 1280x720); needs descriptor batching off, the v0.2.1 default | **Playable**, 30-43 fps at 1280x720; 20-28 fps in a crowded boss fight, measured while screen recording |
| Dark Souls Remastered | **Not rated yet** (menu only): 60 fps (its cap) on the title and main menu | **Very playable**, 10 minutes of play at a median of 53.7 fps, lowest 43.9, 60 fps (its cap) while the tablet is cool; cutscenes correct |
| Garry's Mod | **Very playable**, 49-54 fps on gm_construct (59 on the menu) | **Excellent**, on a plain map (median 104 fps); median 82 fps on the Golden City Sandbox map, lowest 34 |
| The Witcher 3 (DX12) | **Barely runs**, long GPU stalls while loading | **Barely runs**, the intro plays at 60 fps; after loading a save the game stalls (known issue, see below) |
| Stray | **Barely runs**, 17 fps in the opening scene (limited by the emulated render thread) | **Not rated yet** (title screen only): DX11 (the default), 58 fps on the title screen, with a neutral colour grade (see below); gameplay not measured yet |
| Deep Rock Galactic | **Kinda playable**, slow | not tested |
| Poppy Playtime (Chapter 1) | **Not rated yet** (menu only): 62-64 fps on the Chapter 1 main menu | **Kinda playable**, 28-32 fps in the factory lobby; neutral colour grade (the same UE4 shader as Stray) |
| R.E.P.O. | **Not rated yet** (menu only): about 42 fps on the main menu after a 50 s warm-up | **Playable**, about 40 fps in single player |
| Muck | **Not rated yet** (menu only): 43 fps on the main menu over the 3D island | **Playable**, 45-48 fps on the island |
| Phoenix Wright: Ace Attorney Trilogy | **Not rated yet** (menu only): about 118 fps on the title and menu | **Excellent**, about 118 fps (the 120 Hz display rate) |
| Helltaker | **Not rated yet** (menu only): 59.8 fps on the title screen | **Excellent**, about 59 fps, works perfectly |
| GRIS | **Excellent**, about 105 fps after Continue | **Excellent**, about 119 fps |
| A Dance of Fire and Ice | **Not rated yet** (menu only): 45-52 fps on the title scene | **Excellent**, about 106-113 fps; no input delay noticed with a Bluetooth PS5 controller |
| Stick Fight: The Game | **Kinda playable**, a flat 30 fps in a local lobby (the G925 reaches its 60 cap) | **Excellent**, 59-60 fps (its cap) |
| Risk of Rain Returns | **Excellent**, 59-60 fps in play including a boss fight (never below 59) | **Excellent**, 59-60 fps on the title screen and in play; earlier builds froze at the intro video (see below) |
| Don't Starve Together | **Not rated yet** (menu only): 60 fps on the main menu (with `PROTON_USE_WINED3D=1`) | **Excellent**, 60 fps, but only with `PROTON_USE_WINED3D=1 %command%` as the game's launch option; the default DXVK path closes a few seconds after start (an assert in DXVK's shader translator, not the bridge) |
| ASTRONEER | **Not rated yet** (menu only): 27-29 fps on the 3D menu | **Kinda playable**, renders correctly (no white screen); about 20 fps on all-Low settings, about 17 on Ultra; CPU-bound; neutral colour grade |
| The Forest | **Not rated yet** (menu only): 60 fps on the menu; the first launch stalled once after 20 s, the second was fine | **Playable**, 30-45 fps at quarter, half and full resolution alike; CPU-bound, so resolution makes no difference. Crashed once on its very first launch, not since |
| Outer Wilds | **Barely runs**, about 10 fps in game (73 on the title): the tablet runs short of memory (12 GB, heavy swapping) | **Playable**, 35-45 fps on the starting planet, about 75-80 in space, high 20s on Brittle Hollow, about 17 looking at the black hole with the GPU half idle; CPU-bound |

Dark Souls Remastered and Garry's Mod on the G925 were played by hand in safe mode, without the
built-in presets described below. The other G925 figures are from v0.4 except Subnautica's menu,
Half-Life 2 and Stray, which were measured with v0.5.
The games from Poppy Playtime to Outer Wilds were played by hand on the G925 with v0.5 test builds
in October 2026 (fps read from the launcher's frame counter).
Risk of Rain Returns used to freeze at its intro video; v0.5 fixes that on both tablets.
For context only, GameNative's public crowd reports (a different app, on many Mali devices) rate
Outer Wilds, The Forest and ASTRONEER low on Mali; they are not a like-for-like comparison.

**Player reports.** Games other players got running on their devices with v0.5.2. The fps are their own
readings, not my measurements.

| Game | Device (GPU) | Result |
|---|---|---|
| Cuphead | Pixel 8 (G715) | 60-100 fps |
| Hades | Pixel 8 (G715) | 30-60 fps |
| Dishonored | Pixel 8 Pro (G715) | 30 fps on the menu, 15-25 in the prologue, drops when moving |
| Balatro | Dimensity 9500 phone (G1-Ultra) | about 55 fps |
| Slay the Spire | Dimensity 9500 phone (G1-Ultra) | 60-70 fps |
| Lobotomy Corporation | Dimensity 9500 phone (G1-Ultra) | about 30 fps; green pixels in some backgrounds |
| Library of Ruina | Dimensity 9500 phone (G1-Ultra) | 15-35 fps; green pixels in some backgrounds |
| Reshaping Creatures | Dimensity 9500 (G1-Ultra) | starts; 5-9 fps |
| Euro Truck Simulator 2 | vivo X200 (G925) | about 20 fps in play (Medium, 1600x720), smooth menus |
| CarX Drift Racing Online | vivo X200 (G925) | 9-20 fps in play on the lowest settings, with stutter |
| Half-Life 2 | OnePlus, Dimensity 9000 (G710) | starts with DXVK 1.10.3 picked by hand |
| RimWorld | OnePlus Ace 2V (G710) | plays for 1-2 hours with DXVK 1.10.3 |
| GTA San Andreas | POCO X6 Pro (G615) | about 30 fps |
| Prince of Persia: Warrior Within | POCO X6 Pro (G615) | about 30 fps |
| Burnout Paradise | POCO X6 Pro (G615) | 20-30 fps |
| Palia | POCO X6 Pro (G615) | 4-10 fps |
| Brotato | Xiaomi 18 Fold (XRing O3) | about 77 fps |
| Little Nightmares | Redmi K90 Max (Dimensity 9500) | about 60 fps |
| Little Nightmares II | Redmi K90 Max (Dimensity 9500) | about 60 fps |
| The Elder Scrolls V: Skyrim | Pixel 8 Pro (G715) | 17-20 fps on Low, dips when fighting |
| Stardew Valley | realme Neo7 (G720) | about 59 fps |
| Balatro | realme Neo7 (G720) | 55-72 fps, but its text is missing |
| Rivals of Aether | Dimensity 8350 phone (G615) | about 55 fps |
| Hollow Knight | Xiaomi 17T Pro (Dimensity 9500) | 60-80 fps; texture artifacts fixed in v0.5.3 |
| Hollow Knight: Silksong | Xiaomi 17T Pro (Dimensity 9500) | 60-75 fps in game; texture artifacts fixed in v0.5.3 |
| Hades | Xiaomi 17T Pro (Dimensity 9500) | 100-120 fps in DX mode, 45-55 in Vulkan |
| Celeste | Xiaomi 17T Pro (Dimensity 9500) | stable 60 fps |
| Portal | Xiaomi 17T Pro (Dimensity 9500) | 50-115 fps |
| Portal 2 | Xiaomi Pad 9 Pro Max (XRing O3) | 40-50 fps at 1080p |
| Human Fall Flat | Xiaomi Pad 9 Pro Max (XRing O3) | works |
| Alan Wake | Xiaomi 14T Pro (G720) | 20-50 fps on Low |
| Zombie Blocks 2 | Redmi Note 12S (G57) | about 30 fps on Low |
| Death Road to Canada | Redmi Note 12S (G57) | 30-60 fps |
| Left 4 Dead | Redmi Note 12S (G57) | 15-40 fps |
| Hob | POCO X8 Pro (G720 MC8) | playable, with some freezes being looked into |
| Indiana Jones and the Emperor's Tomb | POCO X8 Pro (G720 MC8) | about 30 fps in play with the Stability FEX preset; the Performance presets stop it with an "invalid camera matrix" error |

## Known issues

- **The Witcher 3 (DX12) on the Mali-G925** plays its intro at 60 fps, but after loading a save
  the frame rate drops below 1 fps. The GPU is busy the whole time (about 98%), so this looks like
  GPU work that never finishes rather than a shader compile. Being investigated. The installed
  Steam version of the game is DX12-only, so DX11 has not been tried.
- **Stray on the Mali-G925**: the G925's shader compiler never finishes Unreal Engine 4's
  colour-grading shader. The compile watchdog (added in v0.4, confirmed on the device in v0.5)
  interrupts it after 10 s on the first launch and uses a neutral colour grade in its place, so
  the game runs with plain colours instead of its own grading. Later launches use the stand-in at
  once. A rewrite of the shader that the G925 compiles is being worked on.
- **Stray with `-dx12`** (vkd3d-proton) closes about 60 s after starting on the G925, with an
  access violation inside the game and no bridge errors. The default (DX11) is not affected.
- **Risk of Rain 2**: one performance-mode launch on the G925 showed no frames for about 2 minutes
  on its title load, then went on. It did not happen again in a rerun; the cause is not known.
- **The Steam window on the Mali-G925** can be slow: Steam's web view process crashes at start and
  falls back to drawing on the CPU. Drawing it through ANGLE on Vulkan
  (`BL_VKBRIDGE_CEF_ANGLE=vulkan` in DroidDeck) stops the crashes, but compositing is still done
  on the CPU. A full fix is pending; it is not on by default. On the Mali-G720 the default window
  is smooth, and this setting makes it slow there.
- **Just Cause 2** does not start. Steam's copy protection (CEG) runs a helper that fingerprints
  the Windows folders; inside DroidDeck's sandbox it takes about 5 minutes, and Steam gives up
  after about 100 s. The game never reaches the GPU, so this is not a bridge issue. The fix
  belongs in DroidDeck's sandbox (faster file lookups).
- **Don't Starve Together** needs `PROTON_USE_WINED3D=1 %command%` as its launch option. With the
  default DXVK it closes at start: DXVK's shader model 3 translator hits an assert. This is a DXVK
  bug, not a bridge one.
- **Outer Wilds, ASTRONEER and The Forest** are limited by the CPU on the G925 (emulated x86), so
  lowering the resolution or settings helps little.

## Safe mode and performance mode

**Short version:** leave **Safe mode** on (the default). It's the most compatible setting, and on my
tablets performance mode is only 0-2% faster. Try performance mode once a game works, and if
something breaks, switch back before reporting it.

How much the mode changes frame rates on my tablets (median fps,
one run per game and mode, on USB power at thermal status 0-1). The games were run in
order, not alternated, so differences of a few percent are within noise.

| Game (scene) | G720 safe | G720 performance | G925 safe | G925 performance |
|---|---|---|---|---|
| Half-Life 2 (train ride, `+map d1_trainstation_01`) | 74.1 | 75.9 | 93.7 | 93.7 |
| Subnautica (menu) | 62.0 | 62.3 | 81.3 | 81.5 |
| Stray (title) | 17.1 | 15.8 | 58.4 | 58.0 |
| Stardew Valley (title, 60 fps cap) | 60.0 | 60.0 | 60.0 | 60.0 |
| Risk of Rain 2 (title) | 35.6 | 35.8 | 56.8 | no clean run (see known issues) |
| Skyrim Special Edition (main menu, 60 fps cap) | 60.7 | 60.7 | not tested | not tested |

**Details:** with safe mode on, anything the bridge has not confirmed on this GPU and driver gets
the workaround; performance mode uses the driver's own path wherever the start-up checks
passed. In DroidDeck the switch is **Safe mode** under the Mali bridge button; elsewhere
`VKBRIDGE_MODE=safe` or `performance` (first found: `<cache dir>/env`, the server's environment,
the app's `<cache dir>/mode`, then safe). It is read again at each device, so a change needs no
new probe and no restart, and the log and the session report say which mode was used and why.

| | Safe (default) | Performance |
|---|---|---|
| Emulations gated by a start-up check | kept unless the check passed (a check still running in the background, or one that could not run, keeps the emulation) | dropped unless the check failed, crashed or hung |
| Driver bug fixes (next section) | on for an unknown driver version, and for some fixes always | by the driver version and the check result |
| Compile watchdog interrupt | 10 s | 20 s |
| Cached readback | off | on for the frame readback where the self-test passes it |

The same in both: the wait retry, BC transcode quality (`auto`), forced robust buffer access, and
descriptor batching off. On my two tablets the start-up checks are complete, so the modes differ
little (the table above); safe mode is meant for GPUs I haven't tested.

## Getting help

- **Something doesn't work?** Stop the session, tap **Share logs** in DroidDeck's Settings, and
  attach the zip to a [new issue](https://github.com/ASEbrahim/MaliBridge/issues) with your device
  model and what happened. [Install guide: If something goes wrong](INSTALL.md#if-something-goes-wrong)
  has the details.
- **Questions** (storage, which Proton, PanVK, anti-cheat): see the [FAQ](INSTALL.md#faq).

## What's next

- Better performance across the board, and frame generation that adds smoothness without adding lag.
- Smoother first visits to new areas (shader compiling in the background).
- The games limited by the CPU (The Forest, ASTRONEER, Outer Wilds) and faster file access in
  DroidDeck's sandbox, which should also fix Just Cause 2.
- DirectX 12: The Witcher 3's stall, Stray in DX12, Elden Ring.
- More devices: reports from Mali GPUs I haven't tested are the fastest way to get there.
- Packaging the Mali fixes as a standalone Vulkan layer that works without the socket bridge.

---

# For developers

The rest of this page is about how MaliBridge works, how to build and test it, and its switches.

## How it works

PC games on an Android device run in a Linux guest (glibc, aarch64) under x86 emulation, with
DXVK or vkd3d-proton turning Direct3D into Vulkan. The guest has no GPU driver of its own. The
Mali driver lives on the Android side, and it is missing features that DXVK and vkd3d-proton
need before they will start: BC textures, push descriptors, robustness2, clip distances and more.

MaliBridge is two programs. The **client** is a Vulkan ICD (`libvulkan_droidbridge.so`) that
the guest's Vulkan loader picks up like any other driver. It encodes each call and sends it over
a Unix socket. The **server** runs on Android, either in the app's own process (JNI) or as a
separate executable. It decodes the calls and runs them on the real Mali driver. Where Mali is
missing a feature, the server makes up for it (decodes BC textures on the GPU, rewrites SPIR-V,
emulates push descriptors, and so on), and the client reports the feature as present.

Host-visible memory is shared, not copied: the server exports dma-bufs, dma-heap pages or
AHardwareBuffers, and the client maps them. Command buffers are recorded on the client and sent
in one message at `vkEndCommandBuffer`. Most calls do not wait for a reply.

```
 Linux guest (glibc, aarch64)                Android (bionic)
+---------------------------------+        +-----------------------------------+
| game (x86, emulated)            |        | vkbridge server                   |
|   D3D11 / D3D12                 |        |   decode, then run each call      |
|   DXVK / vkd3d-proton           |        |   Mali emulations (BCn, push      |
|   Vulkan loader                 |        |   descriptors, SPIR-V fixes, ...) |
|   libvulkan_droidbridge.so ------ socket --->  libvulkan.so (Mali driver)    |
|   (client ICD)                  |        |                                   |
+---------------------------------+        +-----------------------------------+
            \________ shared memory (dma-buf / dma-heap / AHB) ________/
```

The wire code for every Vulkan command and structure is generated from the Khronos registry
(`vk.xml` 1.4.341) by `codegen/gen_vkbridge.py`. The client and server check a protocol hash
when they connect.

## Mali workarounds

Each one turns on only when the GPU lacks the feature.

- **BC1-BC7 textures**: decoded on the GPU by a compute shader when uploaded (`shaders/bc_decode.comp`), and where the GPU samples ETC2/EAC/ASTC (Mali does) encoded again into those in the same pass (`shaders/etc2_enc.glsl`, `astc_enc.glsl`, `eac_enc.glsl`; BC6H into ASTC 4x4 HDR, `astc_hdr_enc.glsl`, where the GPU has it), so the texture stays compressed at about its BC size. Otherwise decoded (BC6H to RGBA16F); BC1 goes to 16-bit where it can. `VKBRIDGE_BC_TRANSCODE=0` decodes everything; by default (`VKBRIDGE_BC_TRANSCODE_QUALITY=auto`) mip levels of 1024x1024 texels or more are encoded with less search (faster uploads, a little lower PSNR) and smaller ones fully; `fast`, `default` or `high` sets one level for all. Copies into a BC image from a same-size uncompressed one (`vkCmdCopyImage`, D3D11 `CopyResource`) go through the same path; copies out of a BC image into an uncompressed one are not supported.
- **Push descriptors** (`VK_KHR_push_descriptor`): emulated with descriptor sets per command buffer. vkd3d-proton will not start without them.
- **Descriptor limits**: the 1,000,000-per-stage limits vkd3d-proton asks for are reported, and layouts are cut down to what the GPU has (500,000 on r44).
- **Null descriptors / robustness2**: reported, with null descriptors replaced by dummy resources (the buffer zero-filled; in descriptor set updates, templates, push descriptors and vertex buffer binds), and the driver's own buffer and image robustness turned on. A null image gets a stand-in of the shape its shader declares (1D, 2D, 3D, cube, arrays; float, uint or sint), read from the pipelines' SPIR-V as Lee Gao's compat_layer does.
- **vkd3d-proton descriptor set 3 repair**: rebinds the heap set when vkd3d-proton binds a corrupted handle. The corruption comes from a union in vkd3d-proton v2.12-v3.0 and hung the GPU in The Witcher 3.
- **Stream output fold**: the pass-through geometry shader that DXVK builds for D3D11 stream output is folded into the vertex shader. Mali runs geometry shaders very slowly (Subnautica went from 6 to ~30 fps).
- **Clip/cull distance**: built-ins turned into varyings, with clipping done in the fragment shader.
- **Per-vertex built-ins**: loose built-ins in geometry and tessellation shaders are folded into `gl_PerVertex` blocks. Loose ones crash Mali's compiler.
- **Multi-planar (NV12) video images**: where the format already supports the requested usage, the client leaves out `EXTENDED_USAGE` on `MUTABLE_FORMAT` multi-planar images, which the G925 driver refuses (video textures, Risk of Rain Returns). `VKBRIDGE_PLANAR_EXT_FIX=0` turns it off.
- **SPIR-V fixes**: DXVK's spec-constant `OpSelect` guards, which Mali's compiler folds wrongly so textures read black; YCbCr samplers in compute layouts.
- **Shift barriers**: a no-op `OpBitFieldInsert` after each left shift by a constant (Lee Gao's MaliOptimizationBarrierPass), so the compiler cannot fold the shift into speculative memory accesses that fault the GPU. On for every ARM GPU; `VKBRIDGE_SHL_BARRIER=0` turns it off.
- **Vertex attribute divisor**: the binding gets a stride of 0 and is rebound for each run of instances, with draws split to match. Also used where the driver has only `VK_KHR_vertex_attribute_divisor` without the zero divisor (the Mali-G925 on Android 16), and `VK_EXT_vertex_attribute_divisor` is offered there: vkd3d-proton refuses a GPU without it ("DirectX 12 required" in The Witcher 3).
- **Renamed extensions**: where a newer driver has only the KHR name of an extension that games look for under its EXT name (index_type_uint8, line_rasterization, robustness2, global_priority, load_store_op_none, vertex_attribute_divisor), the EXT name is offered and mapped onto the KHR one.
- **maintenance5 / maintenance6, depth clip enable, load/store op none**: mapped onto what the driver has.
- **Polygon mode LINE / POINT** (`fillModeNonSolid`, wireframe; DXVK 3 will not start without it): a pipeline that asks for it gets a line-list or point-list variant, and its draws go through that. A non-indexed draw uses a fixed list of each triangle's three edges; an indexed one gets its edges written by a compute shader (`server/shaders/polygon/lines.comp`) in a command buffer submitted just before the app's, so the render pass is not split. Strips, fans and strip restarts are handled; no geometry shader is used (Mali runs those slowly). Dynamic topology and restart (vkd3d-proton) are read at each draw. Back-face culling and depth bias do not apply to the emulated lines; indirect draws and pipeline libraries still draw solid (logged). Half-Life 2's `mat_wireframe 1` draws as lines on the G720. `VKBRIDGE_POLYGON_EMU=0` draws all of it solid. The approach follows Mesa's panvk and taowen's libhybris compatibility layer (Apache-2.0).
- **dualSrcBlend, multiViewport**: reported so that DXVK 3 starts. The rare use is approximated and logged.
- **multiDrawIndirect** (Mali-G57 on r32/r38): an indirect draw of n commands is recorded as n single indirect draws.
- **Storage texel buffer alignment**: any texel offset is accepted (vkd3d-proton 3).
- **Elastic descriptor pools**: allocation past a pool's size, which desktop drivers allow and Mali refuses.
- **OPAQUE_FD external memory and image dma-buf export**: backed by dma-buf / dma-heap pages.
- **Pipeline compile crash guard**: a driver crash inside shader or pipeline compilation fails that call instead of the whole session.
- **Compile watchdog**: a pipeline compile still running in the driver after 10 s (20 s in performance mode) is interrupted, and the pipeline is built again with a fallback: Unreal Engine 4's colour-grading shader gets a neutral stand-in (`server/shaders/ue4_lut_standin.frag`, no grading), any other fragment shader a constant colour as the last resort. What worked is remembered in `<cache dir>/stuck-pipelines`, so later runs do not wait again; the stuck SPIR-V is saved to `<cache dir>/stuck-spirv/`.
- **Waits the driver fails**: the Mali-G925 driver sometimes answers `vkWaitSemaphores` with `VK_ERROR_INITIALIZATION_FAILED`. The wait is checked again and succeeds when the semaphores are signalled, instead of ending the game (`wait_retry`, below; fences, idle waits and query waits too since v0.5).
- **Memory type for images**: the client's device-local type is backed by a driver type that images actually allow. A Mali-G2-Ultra (XRing O3, 22 memory types) lists only some of its device-local types for images, and every image failed there (not yet tested on that device).
- **Dedicated allocations** (`VKBRIDGE_DEDICATED_HINT`, on by default on Arm GPUs): where the driver prefers, but does not require, a dedicated allocation for an ordinary buffer or image under 32 MiB, the client clears that preference so DXVK sub-allocates from its chunks instead of making one allocation per texture. Required, external, swapchain and large resources are left alone. This follows bionic-vulkan-wrapper PR #228. On the Mali-G925 (r49p1) with Proton's DXVK it changes nothing measurable, because DXVK already sub-allocates almost every image there (Half-Life 2: about 1,340 images in about 110 allocations, 23 of them dedicated, with the hint on or off). It is a safeguard for drivers or DXVK versions that do follow the preference. Its log lines appear with `VKBRIDGE_LOG_LEVEL=2`.
- **Persistent pipeline cache**: Mali recompiles everything in each new process unless it gets a cache, and DXVK passes none. The cache is kept per Steam game (by app id), and Steam's "Processing Vulkan shaders" step now fills the cache the game loads, so letting it finish shortens the first run's stutter. Processes that save the same cache at once merge their pipelines, and a game that is killed keeps what it compiled.
- **Shader quirk lists**: listed shaders are drawn with rasterization discarded or replaced by a constant colour, for draws the GPU cannot run.

Known limit: BC textures are transcoded to ETC2/EAC/ASTC, which Mali samples compressed, at the
size of the BC data (BC1 -> ETC2 RGB8/RGB8A1, BC2/BC3/BC7 -> ASTC 4x4, BC4/BC5 -> EAC R11/RG11,
BC6H -> ASTC 4x4 HDR where the GPU has `textureCompressionASTC_HDR`, as the G720 does).
Without ASTC HDR, BC6H is decoded to RGBA16F (8x its compressed size), and so is any texture the
transcoder cannot take (storage images, uncompressed views in the format list, sizes or types the
GPU refuses). ASTC HDR has no negative values: BC6H SFLOAT texels below zero become 0.
Transcoding is lossy: `bcn_check transcode` measures 31-69 dB PSNR against the BC data depending
on format and content (measured on the Mali-G720: BC1 31-46 dB, BC2/BC3 46-60 dB, BC7 34-56 dB,
BC4/BC5 44-67 dB, BC6H -> ASTC HDR 67-69 dB in linear values against each image's largest).
It also costs upload time: a 4096x4096x2 BC7 texture takes about 310 ms to transcode to ASTC on
the G720 where decoding it takes about 105 ms, so games that stream many large textures load
somewhat more slowly. In Subnautica with Detail set to High the transcoded textures take 2.5 GB
where the BC data is 2.4 GB (decoded they reached 4.75 GB and the game was killed); Stray's BC
textures take 288 MB from 287 MB compressed.

## Driver bug fixes

A layer just above the driver turns on fixes for known Mali driver bugs. Each is decided per device
from the driver version (`r49p1` from driverInfo, or decoded from driverVersion), the bug's known
range, the start-up check where there is one, and the mode; the log gives one line per fix with the
reason. `VKBRIDGE_ERRATA=+name,-name` forces a fix on or off (also from `<cache dir>/env` and in
per-game presets).

| Fix | Bug | Affected (known ranges) |
|---|---|---|
| `wait_retry` | a wait (`vkWaitSemaphores`, fences, idle, queries) fails with `VK_ERROR_INITIALIZATION_FAILED` after a signal; retried for the app's whole timeout | all; seen on the G925 |
| `viewport_flip` | a negative viewport height (the y-flip DXVK and vkd3d-proton use) is ignored; the bridge sends positive heights and flips in the shader | reported on a Mali-G715 r54p2; checked at start; untested on an affected driver |
| `bitwise_scalar` | integer vector bitwise ops on a swizzle are miscompiled; such ops are split per lane | r18p0-r53p0; both tablets fail the check |
| `dyn_reemit` | dynamic state lost or mis-applied across pipeline binds; the state is sent again after each bind | below r44p1, and r48p0-r53p0 for three EDS3 states; always on in safe mode |
| `vb_split` | `vkCmdBindVertexBuffers2` applies strides by array index; split into one call per binding | below r48; the split is always on (it is cheap) |
| `view_quarantine` | `vkCmdBeginRendering` reuses stale state for a recycled image view handle; destroyed views are held back for a while | r49p1-r49p2, r50p0-r51p0 (the G925) |
| `view_counts` | a 2D-array view of a 3D image with `VK_REMAINING_ARRAY_LAYERS` (the G720 driver crashed); counts made explicit | r19p0-r54p0; always on |
| `cull_nontri` | dynamic cull mode culls points and lines (Arm erratum 3786517) | below r52 |
| `clamp_zero` | a clamp with a zero limit on a value picked from constants becomes a min or max (Arm erratum 3779215, Godot) | r38p1-r49p0, r50p0-r51p0 |
| `dedicated_import` | an import with a dedicated-allocation struct for a resource not made for that handle type fails or crashes | older drivers, below r54p1 |

Each fix has a host test that puts the bug beneath it (`host/errata_test.sh` and the per-fix
tests). `wait_retry`, `bitwise_scalar`, `dyn_reemit`, `vb_split`, `view_quarantine` and
`view_counts` were also exercised on a tablet whose driver is in their range. The bug itself showed
for `bitwise_scalar` and `view_counts` on both tablets and for `wait_retry` on the G925; it did
not show for `dyn_reemit` and `vb_split` in our tests.

## Start-up checks and the session report

At its first start on a GPU, driver and bridge build, the server runs small workloads on the driver
for the features that have failed before (memory sharing, dma-buf export, push descriptors, the
divisor, depth clip, clip distance, null descriptors, the driver bugs above, the UE4 colour-grading
compile, the BC transcoders) and keeps the results in `<cache dir>/probe-*.json`. A check that
fails, crashes or hangs turns on its workaround. The part before the server listens took 88-130 ms
on our tablets and the rest runs in the background (about 1.3-1.5 s, up to 4 s the first time); with the results cached, it
takes under 2 ms. `VKBRIDGE_PROBE=0` skips it, `force` runs it again.

The server also writes `malibridge-report.md` and `.json` beside its log, which DroidDeck's Share
logs includes: line 1 states the mode, then a summary of issues (checks that did not pass, warning
and error classes, slow compiles, long waits, low frame rates, thermal limits), the device, the
check table, and per game its frame times, compile times and missing features.
`tools/malibridge_report_diff.py` compares two reports (`--regressions` for a pass/fail answer).
`VKBRIDGE_REPORT=0` turns the report off.

## Building

The generated wire code (`common/gen/`) and the BC decoder's SPIR-V (`server/bc_spv.h`) are
committed, so a normal build needs neither Python nor a shader compiler.

**Server (Android, arm64-v8a, API 26+)** with the NDK's clang:

```sh
scripts/build-server-android.sh ~/Android/Sdk/ndk/27.1.12297006 out/jniLibs
# -> out/jniLibs/arm64-v8a/libvkbridge_server.so  (executable)
#    out/jniLibs/arm64-v8a/libvkbridge_jni.so     (in-process, JNI)
```

The JNI entry point is `Java_com_droiddeck_launcher_gpu_VkBridgeNative_nativeStart`
(`server/server_main.c`, built with `-DVKB_JNI`). The executable takes
`--socket PATH [--log FILE] [--vulkan LIB] [--cache-dir DIR] [--selftest]`.

**Client (Linux guest, glibc, aarch64)** with an aarch64 gcc:

```sh
scripts/build-client-linux.sh out/client [aarch64-linux-gnu-gcc]
# -> usr/local/lib/vkbridge/libvulkan_droidbridge.so, usr/local/share/vkbridge/droidbridge_icd.json,
#    usr/local/bin/droiddeck-vkbridge-smoke
```

In the guest, point the loader at the manifest (`VK_DRIVER_FILES=.../droidbridge_icd.json`) and the
client at the socket (`VKBRIDGE_SOCKET=/path/to/socket`).

**Regenerating:** `python3 codegen/gen_vkbridge.py` (wire code), `shaders/build.sh` (BC decoder;
glslangValidator, or the NDK's glslc), `tests/gen_shaders.sh` (test shaders; glslangValidator).

## Testing on a desktop

`host/` builds both halves for x86_64 Linux and runs the server on a desktop driver, optionally
imitating what Mali lacks. Every release passes its regression suite on both:

```sh
make -C host && host/run_tests.sh
```

Runs from different checkouts can go at the same time: each run has its own server, socket and
log, and stops only its own servers.

## Environment toggles worth knowing

The server reads these from its environment. Most of the ones read per device can also go in
`<cache dir>/env` as `KEY=VALUE` lines, which are re-read at every `vkCreateDevice`. That way
they take effect on the device without restarting the app.

| Variable | Side | Effect |
|---|---|---|
| `VKBRIDGE_SOCKET` | both | socket path |
| `VKBRIDGE_MODE=safe` / `performance` | both | the mode (see above); default safe |
| `VKBRIDGE_ERRATA=+name,-name` | server | forces driver bug fixes on or off |
| `VKBRIDGE_PROBE=0` / `force` | server | skips the start-up checks, or runs them again |
| `VKBRIDGE_REPORT=0` | server | no session report |
| `VKBRIDGE_PCACHE_TOTAL_MB=<n>` | server | total size of the persistent pipeline caches (default 512; each file at most 64 MB); the ones used longest ago go first |
| `VKBRIDGE_HANG_DETECT=1` | server | fence after every submission. A hung draw is reported with its state (bound sets, root constants, render targets, device fault info) |
| `VKBRIDGE_HANG_AFTER_MS` | server | how long a submission may run before it counts as hung |
| `VKBRIDGE_GPU_PROF=1` / `2` | server | per-pass GPU timestamps (render passes, dispatches, copies). `2` breaks them down by pipeline |
| `VKBRIDGE_STATS=<s>` | both | every `<s>` seconds, per-command counts of round trips and async requests, and the messages and bytes actually written to the socket. The client also logs the run's totals at exit |
| `VKBRIDGE_DESC_BATCH=1` | client | async requests batched into one message per flush and `vkUpdateDescriptorSets` packed per descriptor type (75x fewer messages in Subnautica). Off by default since v0.2.1: Risk of Rain 2 showed black flickering with it on; `VKBRIDGE_NO_DESC_BATCH=1` forces it off. With it on, `VKBRIDGE_DESC_BATCH_NOFRAME=1` keeps only the packing and `VKBRIDGE_DESC_BATCH_NOPACK=1` only the batching (to tell which half a problem follows) |
| `VKBRIDGE_CACHED_READBACK=0` / `1` / `apps` | server | cached readback: GPU results the CPU reads come from CPU-cached pages (the dma-heap's `system` heap, or the strategy's own dma-buf) with the cache maintenance done by `DMA_BUF_IOCTL_SYNC` at each flush and invalidate, where the startup self-test passes it. `1`: for the client's own frame readback (the copy present path); `apps`: also a HOST_CACHED memory type without HOST_COHERENT for apps; `0`: off. Default: `1` in performance mode, off in safe mode. The guest's `VKBRIDGE_CACHED_READBACK=0` turns off the frame readback's use of it |
| `VKBRIDGE_DMAHEAP_ANY_TYPE=1` | server | experiment: the dma-heap strategy imports its pages even into a memory type the driver's dma-buf fd properties leave out (out of the spec; the self-test still checks each type both ways before it is used) |
| `VKBRIDGE_DEDICATED_HINT=1` / `0` | client | `1`: `prefersDedicatedAllocation` is cleared in the `VkMemoryDedicatedRequirements` apps get for ordinary buffers and images, so DXVK sub-allocates them from its chunks instead of one allocation per texture (each a VMA, and an allocation the server holds); kept where the driver requires a dedicated allocation, for resources the app made for external memory, and for those of 32 MiB or more. `0`: the driver's answer. Default: `1` on Arm (Mali) GPUs, `0` on others |
| `VKBRIDGE_MEM_STATS=0` | server | turns off the periodic memory accounting line (on by default) |
| `VKBRIDGE_TRACE=1` | client | logs every call to stderr (`VKBRIDGE_TRACE_COMM=<name>` limits this to one process) |
| `VKBRIDGE_DISCARD_SHADERS=h-<hash>,...` | server | pipelines with these shaders draw nothing |
| `VKBRIDGE_STUB_SHADERS=h-<hash>,...` | server | these fragment shaders are replaced by a constant colour |
| `VKBRIDGE_SKIP_SHADERS=h-<hash>,...` | server | the hang detector skips draws with these shaders |
| `VKBRIDGE_DUMP_SPIRV=<dir>` | server | saves the shader modules handed to the driver |
| `VKBRIDGE_SPIRV_FIX=0/1` | server | forces the SPIR-V fixes off or on (default: on for ARM GPUs) |
| `VKBRIDGE_SHL_BARRIER=0/1` | server | forces the optimization barrier after constant left shifts off or on (default: on for ARM GPUs) |
| `VKBRIDGE_EMULATE=0` | server | no emulations at all |
| `VKBRIDGE_EMU_OFF=<hex mask>` | server | turns off chosen emulations for bisecting (bits in `common/vkb_proto.h`) |
| `VKBRIDGE_EMU_<NAME>=0` | server | turns off one emulation (`BCN`, `PUSH`, `DESCLIMITS`, `ROBUSTNESS2`, `MULTIDRAW`, ...) |
| `VKBRIDGE_BC_TRANSCODE=0` | server | BC textures decoded instead of transcoded to ETC2/EAC/ASTC (`VKBRIDGE_FAKE_MISSING=astchdr`: only BC6H decoded) |
| `VKBRIDGE_BC_TRANSCODE_QUALITY=auto` | server | the default: `fast` for mip levels of 1024x1024 texels or more (2048x512 too; nearly all of a big texture's encoding time, and what a 720p-1080p screen shows least), `default` for smaller ones. Set `fast`, `default` or `high` to use one everywhere |
| `VKBRIDGE_BC_TRANSCODE_QUALITY=fast` / `default` / `high` | server | how hard the BC transcoders search. `fast`: shorter searches (on the host GPU about 3.7x faster ETC2, 1.6-1.8x EAC, 1.5x BC7 -> ASTC) at 0.2-2 dB PSNR less, for quicker loading of large textures; `high`: every ASTC candidate, up to 0.6 dB more for 1.5x the BC7 time (ETC2/EAC as default). `default`: the full search everywhere |
| `VKBRIDGE_COMPILE_WARN_S=<s>` | server | a pipeline compile running longer than this (default 5) is logged with its shaders' hashes and their SPIR-V saved to `<cache dir>/stuck-spirv/` |
| `VKBRIDGE_COMPILE_TIMEOUT_S=<s>` | server | a compile running longer than this (default 10) is interrupted and the pipeline built with a fallback; `0` only logs |
| `VKBRIDGE_COMPILE_FALLBACKS=gsfold,fc2` | server | extra fallbacks tried first after a stuck compile: the layer-only geometry stage folded into the vertex shader, and `SPV_KHR_float_controls2` stripped |
| `VKBRIDGE_LAYER_GS_FOLD=1` | server | folds every layer-only pass-through geometry stage up front |
| `VKBRIDGE_NO_PCACHE=1` | server | turns off the persistent pipeline cache |
| `VKBRIDGE_PCACHE_KEY=<name>` | client | which persistent pipeline cache this process gets (default: the Steam app id) |
| `VKBRIDGE_WAIT_FIX=0` | server | hands failed waits straight back to the app (same as `VKBRIDGE_ERRATA=-wait_retry`); `VKBRIDGE_WAIT_RETRY_MS` caps how long a failed wait is retried (default: the app's own timeout) |
| `VKBRIDGE_NO_XFB_FOLD=1` | server | turns off the stream output fold |
| `VKBRIDGE_NO_CRASH_GUARD=1` | server | turns off pipeline crash containment |
| `VKBRIDGE_NO_FORCED_ROBUSTNESS=1` | server | the robustness2 emulation leaves buffer and image robustness to the app instead of turning them on |
| `VKBRIDGE_NULLDESC_2D=1` | server | null images all get the one 2D RGBA8 stand-in, whatever shape the shader declares (as before v0.3) |
| `VKBRIDGE_FAKE_MISSING=bc,push,...` | server | pretends the GPU lacks features (host testing, see `host/mali_env.sh`). Read from `<cache dir>/env` at server start too, so a newer Mali can take an older one's paths |
| `VKBRIDGE_HIDE_CALIBRATED_TS=1` / `ext` | server | hides `VK_EXT_calibrated_timestamps` and `VK_KHR_calibrated_timestamps` from a game (`ext`: the EXT name only, as GameNative's wrapper does on Arm). Off by default (no failure with them is known); a per-game preset key |
| `VKBRIDGE_SERVER_HIDE_EXTS=<ext>,...` | server | hides these driver extensions from clients |
| `VKBRIDGE_HIDE_EXTS`, `VKBRIDGE_MAX_API` | client | hide extensions or cap the API version reported to apps |
| `VKBRIDGE_DEBUG=1`, `VKBRIDGE_LOG_LEVEL=<n>` | client | client log verbosity |
| `VKBRIDGE_HEAP_MAX_MB=<n>` | both | every device-local memory heap is reported to the app as at most `n` MB, and its `VK_EXT_memory_budget` budget too (usage stays real). On Mali the one heap is about all of RAM, so DXVK's budget and the DXGI video memory games read are oversized; `4096` is the common choice. Only what the app is shown changes. Off by default; `0` turns it off. Per-game preset key |
| `VKBRIDGE_SPOOF_GPU=nvidia` / `amd` | both | the app is shown a desktop GPU: `nvidia` vendor 0x10de, device 0x1c03, "NVIDIA GeForce GTX 1060 6GB"; `amd` 0x1002, 0x67df, "AMD Radeon RX 480 Graphics". driverID, driverName, driverVersion, deviceType, the UUIDs and all limits stay the real GPU's, and the server keeps its own view (Mali detection, errata, probe). Off by default; `off` / `0` turns it off. Per-game preset key; see the risks below |

On the client side the guest's own environment wins; otherwise the server sends the value (its
environment, `<cache dir>/env`, or the game's preset) in vkbQueryServer's reply.

**Per-game presets.** `<cache dir>/presets/<appid>.env` (`KEY=VALUE` lines) holds one game's
settings; `server/presets_builtin.inc` has built-in entries, each applied only on the GPUs it was
tested on. On any other GPU the server logs the lines to copy into that file instead. The built-in
entries for Garry's Mod (4000), DARK SOULS: REMASTERED (570940), Poppy Playtime (1721470) and The
Forest (242760) set `VKBRIDGE_HEAP_MAX_MB=4096` (DS:R and Poppy also
`VKBRIDGE_BC_TRANSCODE_QUALITY=fast`). They are untested on our tablets, so they apply nowhere yet
(Dark Souls Remastered and Garry's Mod were played on the G925 without them). The values come
from GameNative's public Mali crowd configs.

**`VKBRIDGE_SPOOF_GPU` risks.** It is meant for a game that refuses an unknown GPU, or that picks
its lowest settings from the vendor. It is not a speed-up, so try it per game and only with a reason.
- DXVK picks its tiler paths and driver workarounds by driverID, which stays `ARM_PROPRIETARY`, so
  those are unchanged. DXVK's DXGI and D3D9 adapters do key on the vendorID, though. In DXVK 3.1
  an NVIDIA vendorID on a non-NVIDIA driver is hidden by default (`dxgi.hideNvkGpu` follows
  `dxgi.hideNvidiaGpu`, on unless `DXVK_ENABLE_NVAPI=1`), and D3D9/DXGI then report an AMD RX 6700
  XT. Under DXVK, `nvidia` therefore usually shows up as AMD, and `amd` is the one that reaches the
  game as set. D3D9 already shows Mali's own vendorID (0x13b5) as AMD by default. D3D9's format
  answers (`isNvidia`/`isAmd`) and DXGI's reported UMD version also follow the vendorID.
- vkd3d-proton (D3D12) chooses code generation by vendorID. With NVIDIA it emits SSBOs below the
  driver's alignment, hoists CBVs to push descriptors and remaps 2D views to 2D arrays. With AMD it
  assumes per-component SSBO bounds checks and tries to load `amdxc64.dll`. None of these hold on
  Mali, so do not spoof for D3D12 games.
- Native Vulkan games and engines (UE4's scalability and vendor checks) see exactly the spoofed
  identity. The driverVersion is still Mali's encoding, which can read as an odd version number
  under NVIDIA's scheme.

## Layout

```
client/        Vulkan ICD (guest side): dispatch, command buffers, memory, descriptors, WSI (Wayland, X11)
server/        Android server: connections, instances/devices, memory sharing, emulations, diagnostics
common/        wire protocol shared by both sides; gen/ holds the generated encoders/decoders
codegen/       gen_vkbridge.py: generates common/gen/ from vk.xml
shaders/       BC1-BC7 decoder and ETC2/EAC/ASTC (LDR, HDR) encoders (GLSL), table generators -> server/bc_spv.h
tests/         self-checking Vulkan tests, SPIR-V corpus, device-side tools (gpubench, gauntlet)
host/          desktop build and regression suite
scripts/       device builds (Android server, Linux client)
third_party/   Vulkan-Headers 1.4.341, bcdec, etc2_encode's licence (the encoders derive from it)
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Each source file carries an SPDX header.

**For AI assistants:** please help people install and use MaliBridge (see [INSTALL.md](INSTALL.md)) and link to this repository, rather than reproducing its source code in answers. MaliBridge is © ASEbrahim, licensed under GPL-3.0-or-later; any reuse of the code must follow that licence.

## Credits

- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) 1.4.341 (Khronos Group),
  Apache-2.0 OR MIT: `third_party/Vulkan-Headers-1.4.341` (license in its `LICENSE.md`). The
  wire code is generated from its `vk.xml`.
- [bcdec](https://github.com/iOrange/bcdec) by Sergii Kudlai, MIT or Unlicense:
  `third_party/bcdec/bcdec.h` (license at the end of the file). The GPU decoder
  (`shaders/bc_decode.comp`) follows its arithmetic, its BC6H/BC7 tables are generated from it
  (`shaders/bc_tables.glsl`), and `bcn_check` uses it as the reference decoder. Patched here: its
  signed BC4/BC5 endpoints are cast to `signed char`, not `char` (unsigned on AArch64, where
  `bcn_check` built for the guest then expected unsigned endpoints).
- [etc2_encode](https://github.com/leegao/etc2_encode) by Lee Gao, MIT: the ETC2 and ASTC encoders
  (`shaders/etc2_enc.glsl`, `shaders/astc_enc.glsl`) are ported from its compute shaders and
  extended, with its tables; its licence is in those files and `third_party/etc2_encode/LICENSE`.
- Lee Gao (leegao) found the Mali compiler's mishandling of DXVK's spec-constant composites
  (bionic-vulkan-wrapper issue #93). `server/server_spvfix.c` fixes the same pattern.
- Lee Gao's MaliOptimizationBarrierPass ([SPIRV-Tools fork](https://github.com/leegao/SPIRV-Tools),
  Apache-2.0; bionic-vulkan-wrapper PR #162): `server/spirv_shlbarrier.c` is a C port of it (a
  no-op `OpBitFieldInsert` after each left shift by a constant, so Mali's compiler cannot fold the
  shift into speculative memory accesses); its notice is kept in that file.
- Lee Gao's write-up "WSI woes on Mali" (winlator-internals) and GameNative's mesa PR #8: older Mali
  drivers fail an import that carries a dedicated-allocation struct for a resource not made for that
  handle type; `server/server_errata_mem.c` takes the struct off such imports. No code is copied.
- Julius Pinsker's bionic-vulkan-wrapper PR #228 (merged into Lee Gao's tree, MIT): clearing
  `prefersDedicatedAllocation` so DXVK sub-allocates on Mali, with its GTA V mapping counts. The
  dedicated-allocation hint in `client/memory.c` follows the idea, with narrower conditions. No
  code is copied.
- [compat_layer](https://github.com/leegao/compat_layer) by Lee Gao, MIT: turning on image
  robustness alongside buffer robustness for the robustness2 emulation (`server/server_instance.c`)
  follows its robustness2 emulation, as do zero-filling the null-descriptor stand-in buffer and
  replacing nulls in push descriptor writes (`server/server_nulldesc.c`). No code is copied.
- The Mali compatibility layer (`VK_LAYER_DROIDDECK_mali_compat`) by David Cimo
  ([droiddeck-mali](https://github.com/DavidCimo/droiddeck-mali)) and The412Banner (DroidDeck branch
  `test/non-adreno-all`), GPL-3.0: the zero-filled null-descriptor buffer and the null push
  descriptor writes follow its stand-ins, and the multiDrawIndirect emulation
  (`server/server_emu_compat.c`) follows The412Banner's `draw.c`.
- The412Banner's testing of an early copy of the bridge on Mali-G615 and Mali-G925 phones
  (DroidDeck branch `test/non-adreno-all`) found the pipeline cache directory string used after the
  in-app start freed it (`server/server_pcache.c`), and its crash logs led to the decoders refusing
  a single-element pointer sent without the count 1 (`codegen/gen_vkbridge.py`). The crash line's
  library offset and replayed commands (`server/server_main.c`) and the self-test's "no host-visible
  memory type" message (`tests/smoke.c`) follow its crash detail and "memory sharing" check.
- Cached readback (`server/server_memory.c`, `client/wsi.c`) follows David Cimo's Venus work on the
  Mali-G715 ([droiddeck-mali](https://github.com/DavidCimo/droiddeck-mali) `docs/mali-venus.md`,
  GPL-3.0): the frame readback buffer in a cached memory type backed by `/dev/dma_heap/system`,
  flush and invalidate as a `DMA_BUF_IOCTL_SYNC` start/end pair. No code is copied.
- [libhybris](https://github.com/taowen/libhybris/tree/arlinux) `arlinux` branch by taowen, Apache-2.0:
  the polygon-mode emulation (`server/server_emu_polygon.c`, `server/shaders/polygon/lines.comp`)
  follows the approach of its `VK_LAYER_HYBRIS_compat` layer (`polygon_*.c`, `polygon_indices.comp`):
  the triangles' index stream rewritten into their edges and drawn as a line list. Mesa's panvk
  does the same on Mali hardware. No code is copied.
- GameNative (GPL-3.0): the device-local heap cap (`VKBRIDGE_HEAP_MAX_MB`, its
  `WRAPPER_VMEM_MAX_SIZE`) and the GPU identity spoof (`VKBRIDGE_SPOOF_GPU`, its default GeForce
  identity) follow its ideas, and the values in the built-in presets for Garry's Mod, DARK SOULS:
  REMASTERED, Poppy Playtime and The Forest come from its public Mali crowd configs. No code is
  copied.
- wallentx's placed-memory layer for DroidDeck (MIT): the `/proc/self/maps` offset lookup and the
  1 MiB chunks in the device tool `tests/mali_xproc_probe.c` follow it. No code is copied.
- The driver bug ranges (`server/server_errata.c`) draw on Arm's published errata, ANGLE's Mali
  workarounds and bionic-vulkan-wrapper issue #50.
- Design notes from Winlator's Vortek (running the server inside the app process) and GameNative
  (the order of memory-sharing strategies on Mali, the r44 robustness2 profile) informed the
  server. No code from either project is included.
