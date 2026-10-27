<h1 align="center">MaliBridge（中文说明）</h1>

<p align="center"><a href="README.md">English</a> | 中文</p>

<p align="center"><b>在使用 Mali GPU 的安卓手机和平板上玩 Steam PC 游戏</b><br>
联发科天玑、Google Tensor、小米玄戒、较早的三星 Exynos，以及其他 Arm Mali / Immortalis 芯片。<br>
直接使用设备自带的驱动，不需要另外安装 GPU 驱动。</p>

<p align="center">
  <a href="https://github.com/ASEbrahim/MaliBridge/releases/latest"><img alt="%E4%B8%8B%E8%BD%BD" src="https://img.shields.io/badge/%E4%B8%8B%E8%BD%BD-%E6%9C%80%E6%96%B0%E7%89%88%E6%9C%AC-2ea44f?style=for-the-badge&logo=android&logoColor=white"></a>
  <a href="https://github.com/ASEbrahim/MaliBridge/releases/latest"><img alt="Latest version" src="https://img.shields.io/github/v/release/ASEbrahim/MaliBridge?style=for-the-badge&label=version&color=orange"></a>
  <a href="https://github.com/ASEbrahim/MaliBridge/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/ASEbrahim/MaliBridge/total?style=for-the-badge&label=downloads&color=blue"></a>
  <a href="https://discord.gg/9xC67qTjDq"><img alt="Discord" src="https://img.shields.io/badge/Discord-join-5865F2?style=for-the-badge&logo=discord&logoColor=white"></a>
  <a href="https://ko-fi.com/asebrahim"><img alt="Ko-fi" src="https://img.shields.io/badge/Ko--fi-support-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white"></a>
</p>

<p align="center">
  <a href="https://github.com/ASEbrahim/MaliBridge/releases/latest"><b>下载</b></a> ·
  <a href="INSTALL.md"><b>安装指南（英文）</b></a> ·
  <a href="#我的设备能用吗"><b>我的设备能用吗？</b></a> ·
  <a href="#能运行的游戏"><b>能运行的游戏</b></a> ·
  <a href="#最佳设置与性能建议"><b>最佳设置</b></a> ·
  <a href="#获取帮助"><b>获取帮助</b></a> ·
  <a href="#常见问题"><b>常见问题</b></a>
</p>

<p align="center"><img src="docs/img/ingame-v054.jpg" alt="《生化危机2》《只狼》《黑暗之魂3》《雨中冒险：回归》Balatro 和 Celeste 通过 MaliBridge 运行" width="900"><br>
<i>在 Galaxy Tab S11 Ultra 上用 v0.5.4 实际游玩的画面。</i></p>

<p align="center"><a href="https://youtu.be/nikeajHpf5s"><img src="docs/img/ds3-video.jpg" alt="视频：在 Galaxy Tab S11 Ultra 上玩《黑暗之魂3》，古达 Boss 战" width="720"></a><br>
<i>用 v0.5.1 录制，以 2 倍速播放。</i><br>
<a href="https://youtu.be/nikeajHpf5s">在 YouTube 观看</a> · <a href="https://www.reddit.com/r/GalaxyTab/comments/1xea3as/">Reddit 帖子</a> · <a href="https://www.youtube.com/@malibridge">更多视频</a> · <a href="https://space.bilibili.com/3747573858831484">哔哩哔哩</a></p>

> [!IMPORTANT]
> 请从本页的 [Releases](https://github.com/ASEbrahim/MaliBridge/releases) 下载 DroidDeck Mali。其他网站上的副本可能是旧版本或被改动过。
> 官方 [DroidDeck](https://github.com/Droid-Deck/DroidDeck) 目前还没有包含 MaliBridge；包含之后，从官方下载也可以。

> 这是英文 README 的中文翻译。如有出入，以[英文版](README.md)为准，那里更新得最快。

MaliBridge 运行在 **DroidDeck Mali** 里。DroidDeck Mali 是一个可以配合 DroidDeck 使用的版本（DroidDeck 是在安卓上运行 Steam 客户端和 PC 游戏的应用）。在三星 Galaxy Tab S10+（Mali-G720）和 Galaxy Tab S11 Ultra（Mali-G925）上，我测试过的 34 款游戏达到了可玩或更好的水平，例如《半条命2》约 94 fps、《空洞骑士：丝之歌》、《星露谷物语》、Risk of Rain Returns、《黑暗之魂 重制版》、《盖瑞模组》。玩家还从另外 19 款 Mali 手机和平板发来了反馈，下面的表格中共有 83 款不同的游戏。MaliBridge 是免费的。

## v0.5.4 新内容

**修复**
- **天玑 9500**（Mali-G1-Ultra）上《空洞骑士》《丝之歌》《哈迪斯》等游戏的**绿色方块和偏红贴图**应该已经消失。
- **三星 Exynos（Xclipse）手机上有声音但画面全黑**：现在应该能显示游戏画面了。
- **较旧的 Mali GPU**（G57、G68 等，例如 Galaxy A16 和 F15）上 **Steam 黑屏**应该已经修复。如果再次出现，把 **Steam menus** 设为 *software*。
- **Balatro 等 OpenGL 游戏中文字缺失**的问题已修复。
- **手机内存不足时崩溃更少**，游戏之后再启动时卡顿也更少。
- 会话被安卓在后台暂停很久之后，结束时**游戏画面不再丢失**。
- 某个设置以错误的格式保存时，**会话开始时应用不再崩溃**。

**更快**
- **每一帧游戏画面只刷新一次屏幕，而不是两次。** 手机的负担更小：在 Tab S11 Ultra 上《半条命2》快了约 6%。

**新功能**
- **应用内更新：** **更新** 页面会显示新版本的内容，点一下就能安装（可以设置为只在 Wi-Fi 下更新）。
- **部分游戏会自动设置好**：《饥荒联机版》、较旧手机上的《半条命2》和《环世界》，以及 Indiana Jones and the Emperor's Tomb 会自动用上它们需要的设置。你自己改过的设置会保留。
- **Send logs to MaliBridge：** 一键上传一次会话的日志，并给你一个短代码，贴到反馈里即可（见[获取帮助](#获取帮助)）。游戏自己的日志也会一起上传。
- **性能 HUD**（侧边菜单 → 显示）会在 fps 旁边显示电量、电池温度和功耗（见[查看帧数](#查看帧数)）。
- **Steam 性能叠加层**现在在三星平板上也能用。
- 打开 Mali bridge 后，**配置**页面会把 Mali GPU 显示为就绪，而不是 “GPU not supported”。

完整更新内容：[发布说明](https://github.com/ASEbrahim/MaliBridge/releases/latest)。

## 我的设备能用吗？

设备需要使用 Arm Mali GPU（Immortalis 也属于 Mali）。在下面的第一个表格里找你的设备，或在第二个表格里查你的芯片。

**怎么查看芯片：** 在“设置 → 关于手机”（或“关于平板”）里查看处理器，或者安装 CPU-Z 之类的应用，它也会显示 GPU。

**已测试**

| 设备 | 芯片 / GPU | 驱动 | 结果 |
|---|---|---|---|
| 三星 Galaxy Tab S10+ | 天玑 9300+ / Immortalis-G720 MC12 | r44p1 | **运行良好**（深入测试） |
| 三星 Galaxy Tab S11 Ultra | 天玑 9400 / Immortalis-G925 MC12 | r49p1 | **运行良好**（深入测试） |
| 小米 18 Fold | 玄戒 O3 / Mali-G2-Ultra-NX MC16 | r56p0 | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/issues/7)：v0.5.2 起可用（自检 13/13，Steam，Brotato 约 77 fps） |
| Google Pixel 9 | Tensor G4 / Mali-G715 | r54p3 | [玩家反馈](https://github.com/Droid-Deck/DroidDeck/pull/517#issuecomment-6220985381)：Steam 可以启动；游戏尚未确认 |
| Google Pixel 8 / 8 Pro | Tensor G3 / Mali-G715 | r54 | [玩家反馈](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6306332804)：游戏可以运行（Cuphead、Hades、Balatro、Dishonored） |
| vivo X200 | 天玑 9400 / Immortalis-G925 MC12 | | [玩家反馈](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6314012693)：游戏可以运行（Euro Truck Simulator 2、CarX Drift Racing Online） |
| OPPO Find X9 Pro 等 | 天玑 9500 / Mali-G1-Ultra MC12 | r54p1 | [玩家反馈](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6312412336)：v0.5.2 起游戏可以运行（Balatro、Slay the Spire）；部分游戏有绿色方块贴图问题，v0.5.4 应已修复 |
| 一加 Ace 2V（天玑 9000） | Mali-G710 | r32p1（Vulkan 1.1） | [玩家反馈](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6335790759)：用 DXVK 1.10.3 时《半条命2》可以启动，《环世界》可以玩 1-2 小时（在这类手机上 v0.5.4 会自动选择它） |
| POCO X6 Pro | 天玑 8300-Ultra / Mali-G615 MC6 | | [玩家反馈](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6311114838)：较轻的游戏可以运行（GTA San Andreas、Burnout Paradise） |
| realme Neo7 | 天玑 9300+ / Immortalis-G720 MC12 | r44p1 | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/issues/5)：《星露谷物语》可以运行；Balatro 可以运行（文字缺失的问题已在 v0.5.4 修复） |
| 天玑 8350 手机 | Mali-G615 | | [玩家反馈](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6322050101)：Rivals of Aether 约 55 fps |
| 小米平板 9 Pro Max | 玄戒 O3 | | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19136786)：Human Fall Flat 可以玩，《传送门2》1080p 下 40-50 fps |
| 小米 17T Pro | 天玑 9500 / Mali-G1-Ultra MC12 | r54p1 | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991)：20 款游戏可以运行（Hollow Knight、Hades、Dead Cells、Celeste 等）；部分游戏有绿色方块贴图问题，v0.5.4 应已修复 |
| 小米 14T Pro | 天玑 9300+ / Immortalis-G720 MC12 | r44p1 | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19146763)：Steam、手柄和下载都正常；Alan Wake 低画质 20-50 fps |
| vivo X100 Pro | 天玑 9300 / Immortalis-G720 MC12 | r44p1 | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/issues/25)：Steam 可以运行，但菜单较慢（见已知问题）；Forza Horizon 4 进入游戏时关闭，正在排查 |
| POCO X8 Pro | 天玑 8500 / Mali-G720 MC8 | r49p1 | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/issues/9#issuecomment-6328655497)：Indiana Jones and the Emperor's Tomb 约 30 fps；Hob 可以玩，偶有卡顿 |
| POCO X7 Pro | 天玑 8400-Ultra / Mali-G720 MC7 | | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/issues/23)：较轻的游戏可以运行（Stardew Valley、Rain World、Tukoni: Forest Keepers 等） |
| 小米 15T Pro | 天玑 9400+ / Immortalis-G925 MC12 | r49p1 | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/issues/17)：自检 13/13；Mega Man Star Force Legacy Collection（DX12）在标题画面前关闭，正在排查 |
| Redmi Note 12S | Helio G96 / Mali-G57 MC2 | | [玩家反馈](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19128201)：第一份 G57 反馈；Steam 第一次就能用，较轻的游戏可以运行（DDNet、YOMI Hustle、Space Menace） |

**需要帮助：尚未测试。** 这些设备的反馈最有帮助：

| 芯片 | GPU | 设备举例 |
|---|---|---|
| 天玑 9300 / 9300+（其他设备） | Immortalis-G720 MC12 | vivo X100、OPPO Find X7 |
| 天玑 9400 / 9400+（其他设备） | Immortalis-G925 MC12 | OPPO Find X8 |
| 天玑 9000 / 9200（其他设备） | Mali-G710 / Immortalis-G715 | vivo X90、OPPO Find X6 |
| 天玑 7300、8100、8200、Helio G99 等 | Mali-G615 / G610 / G57 | 很多中端手机（GPU 较小：只适合轻量游戏） |
| Exynos 1280 / 1380 / 2100 | Mali-G68 / G78 | Galaxy A53、A54、S21 |

与已测试设备芯片相同的手机，表现通常也相近；不过各厂商的驱动版本不同，所以依然值得反馈。你设备的日志很有帮助（见[获取帮助](#获取帮助)）。

**其他 GPU：** 骁龙使用 Adreno GPU，请直接使用官方 DroidDeck。MaliBridge 是为 Mali 制作和测试的，但 DroidDeck Mali 在所有非 Adreno 的 GPU 上都会使用它，所以下面这些也可以试试：
- **三星 Xclipse**（较新的 Exynos：2200、2400、2500、2600）：一位玩家的 Galaxy S26+ 可以运行 Steam 和游戏，v0.5.4 应该能修好它之前游戏画面全黑的问题。我自己没有测试过 Xclipse 设备。
- **PowerVR**（Pixel 10 的 Tensor G5 等）：还没有人反馈。如果你试了，请发送日志。

## 快速开始

1. 先在上面的表格里查一下你的芯片。
2. 从[最新版本](https://github.com/ASEbrahim/MaliBridge/releases/latest)下载 `DroidDeck-MaliBridge-....apk` 并安装。安装后的应用名是 **DroidDeck Mali**，可以和官方 DroidDeck 同时存在。
3. 打开 DroidDeck Mali（**Mali bridge** 默认开启，不确定的话到 **配置**（Setup）页面的“概览”里看一下），启动 **Steam** 会话并登录。
4. 每个游戏都需要在“属性 → 兼容性”里选择 **Proton Experimental ARM64 (Compatible)**。

**需要：** 64 位 Android 8.0 或更新；8 GB 内存以上（较大的游戏建议 12 GB 以上）；游戏之外约 8-10 GB 空闲空间。

**更新：** v0.5.4 起，应用的 **更新** 页面会提示新版本，点一下就能安装。从旧版本升级时，直接把新的 APK 安装在旧版本上即可，游戏、设置和 Steam 登录都会保留。

[安装指南（英文）](INSTALL.md)里有每一步的详细说明。

## 能运行的游戏

这些是我在两台平板上玩过的游戏：Galaxy Tab S10+（Mali-G720）和 Galaxy Tab S11 Ultra（Mali-G925），使用[安装指南](INSTALL.md#settings-to-check)里的设置。同一款游戏在 S11 Ultra 上大约快 1.4-2 倍。在其他设备上，表现大致取决于 GPU 的强弱。

**评级**，从差到好：勉强运行、勉强可玩、可玩、很流畅、极佳。我亲自玩过的游戏按实际体验评级，其他的按帧率评级。

| 游戏 | Mali-G720（Tab S10+） | Mali-G925（Tab S11 Ultra） |
|---|---|---|
| Hades | **极佳** | 未测试 |
| Hollow Knight | **极佳**，87-124 fps | **极佳**，110+ fps（玩家反馈） |
| ULTRAKILL | **很流畅**，55-57 fps | 未测试 |
| Celeste | **极佳**，57-59 fps | **极佳**，游戏中 60 fps（上限） |
| Portal | **极佳**，80 fps | 未测试 |
| Cuphead | **极佳** | 未测试 |
| Dead Cells | **极佳** | 未测试 |
| ENDER LILIES | **可玩**，42-44 fps | 未测试 |
| Lethal Company | **可玩**，43-45 fps | 未测试 |
| Subnautica | **可玩**，细节设为 High 时 20-24 fps | **很流畅**，游戏中 28-40 fps（温控降频时），菜单 81 fps |
| Inscryption | **极佳**，68-83 fps | 未测试 |
| Doki Doki Literature Club | **极佳**，标题画面 91-92 fps | 未测试 |
| Half-Life 2 | **极佳**，开场章节 59-74 fps | **极佳**，开场章节 94-103 fps；v0.5.4 在 Water Hazard 约 110 fps |
| Portal 2 | **可玩**，游戏中约 45-50 fps，偶尔掉到 30 左右 | **尚未评级**（仅菜单）：菜单 115 fps |
| Hollow Knight: Silksong | **极佳**，游戏中稳定 62-66 fps | **极佳**，72-96 fps |
| The Elder Scrolls V: Skyrim Special Edition | **极佳**，Medium 下约 60 fps（上限）；第三人称流畅，首次加载时有些卡顿 | 未测试 |
| Stardew Valley | **极佳**，标题画面 60 fps（上限） | **极佳**，57-62 fps |
| Risk of Rain 2 | **勉强可玩**，半分辨率下，人多、特效多的 Boss 战 18-30 fps（1280x720 下 15-23 fps） | **可玩**，1280x720 下 30-43 fps；拥挤的 Boss 战 20-28 fps（录屏时测量） |
| Dark Souls Remastered | **尚未评级**（仅菜单）：标题和主菜单 60 fps（上限） | **很流畅**，玩了 10 分钟，中位数 53.7 fps，最低 43.9，平板不热时 60 fps（上限）；过场动画正常 |
| Garry's Mod | **很流畅**，gm_construct 地图 49-54 fps（菜单 59） | **极佳**，简单地图中位数 104 fps；Golden City Sandbox 地图中位数 82 fps，最低 34 |
| The Witcher 3 (DX12) | **勉强运行**，加载时 GPU 长时间卡住 | **勉强运行**，开场动画 60 fps；读取存档后游戏卡住（已知问题，见下文） |
| Stray | **勉强运行**，开场 17 fps（受 CPU 限制） | **尚未评级**（仅标题画面）：DX11（默认），标题画面 58 fps，颜色较平淡（见已知问题）；尚未测游戏中帧率 |
| Deep Rock Galactic | **勉强可玩**，较慢 | 未测试 |
| Poppy Playtime (Chapter 1) | **尚未评级**（仅菜单）：第一章主菜单 62-64 fps | **勉强可玩**，工厂大厅 28-32 fps；颜色较平淡，和 Stray 一样 |
| R.E.P.O. | **尚未评级**（仅菜单）：预热 50 秒后主菜单约 42 fps | **可玩**，单人模式约 40 fps |
| Muck | **尚未评级**（仅菜单）：3D 岛屿背景的主菜单 43 fps | **可玩**，岛上 45-48 fps |
| Phoenix Wright: Ace Attorney Trilogy | **尚未评级**（仅菜单）：标题和菜单约 118 fps | **极佳**，约 118 fps（120 Hz 屏幕刷新率） |
| Helltaker | **尚未评级**（仅菜单）：标题画面 59.8 fps | **极佳**，约 59 fps，运行完美 |
| GRIS | **极佳**，继续游戏后约 105 fps | **极佳**，约 119 fps |
| A Dance of Fire and Ice | **尚未评级**（仅菜单）：标题场景 45-52 fps | **极佳**，约 106-113 fps；用蓝牙 PS5 手柄没有感觉到输入延迟 |
| Stick Fight: The Game | **勉强可玩**，本地大厅稳定 30 fps | **极佳**，59-60 fps（上限） |
| Risk of Rain Returns | **极佳**，游戏中包括 Boss 战 59-60 fps | **极佳**，标题画面和游戏中 59-60 fps |
| Don't Starve Together | **尚未评级**（仅菜单）：主菜单 60 fps（使用 `PROTON_USE_WINED3D=1`） | **极佳**，使用 `PROTON_USE_WINED3D=1` 时 60 fps，v0.5.4 会自动设置；不设置的话游戏启动几秒后就会关闭 |
| ASTRONEER | **尚未评级**（仅菜单）：3D 菜单 27-29 fps | **勉强可玩**，画面正常（没有白屏）；全低画质约 20 fps，Ultra 约 17；受 CPU 限制；颜色较平淡 |
| The Forest | **尚未评级**（仅菜单）：菜单 60 fps；第一次启动在 20 秒后卡住过一次，第二次正常 | **可玩**，四分之一、一半和全分辨率下都是 30-45 fps；受 CPU 限制，所以分辨率没有影响。第一次启动时崩溃过一次，之后没有再出现 |
| Outer Wilds | **勉强运行**，游戏中约 10 fps（标题 73）：平板内存不足（12 GB，大量交换） | **可玩**，起始星球 35-45 fps，太空中约 75-80，Brittle Hollow 二十多 fps，看向黑洞时约 17（GPU 一半空闲）；受 CPU 限制 |
| Balatro | **尚未评级**（仅菜单）：标题约 100 fps | **极佳**，一局游戏中约 112 fps |
| Dark Souls III | **勉强可玩**，游戏中站立约 17 fps | **勉强可玩**，游戏中站立约 35 fps；移动时短暂卡顿（已知问题） |
| Sekiro: Shadows Die Twice | 未测试 | **可玩**，游戏中站立约 38 fps |
| Resident Evil 2 (DX11 beta) | 未测试 | **可玩**，警察局里约 41 fps |

<details>
<summary>这些数字是怎么测的</summary>

G925 上的 Celeste、Balatro、Dark Souls III、Sekiro 和 Resident Evil 2，以及 Half-Life 2 的 Water Hazard 数据，是 2026 年 10 月用 v0.5.4 测量的：进入游戏后站立不动，每款 60 秒，用游戏自己的帧数统计。G720 上的 Dark Souls III 是用 v0.5.4 预发布版站立测量的。G925 上的 Dark Souls Remastered 和 Garry's Mod 是在安全模式下亲手玩的。G925 的其他数据来自 v0.4，只有 Subnautica 的菜单、Half-Life 2 和 Stray 是用 v0.5 测量的。从 Poppy Playtime 到 Outer Wilds 这几款，是 2026 年 10 月用 v0.5 预发布版在 G925 上亲手玩的（fps 读自启动器的帧数计数器）。Risk of Rain Returns 以前会在开场视频卡住，v0.5 在两台平板上都修好了。

</details>

**玩家反馈。** 其他玩家用较早的版本在自己设备上跑起来的游戏。fps 是他们自己读到的数字。按 GPU 分组，性能强的在前：点开一组即可看到其中的游戏。

<details>
<summary><b>Mali-G1-Ultra（天玑 9500）：小米 17T Pro、Redmi K90 Max 等</b>（27 款游戏）</summary>

| 游戏 | 设备（GPU） | 结果 |
|---|---|---|
| 20 Minutes Till Dawn | 小米 17T Pro（天玑 9500） | [60+ fps，敌人多时掉帧](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Balatro | 天玑 9500 手机 (G1-Ultra) | [约 55 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6312412336) |
| Cairn | 小米 17T Pro（天玑 9500） | [最低画质下 8-12 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Celeste | 小米 17T Pro（天玑 9500） | [稳定 60 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Chants of Sennaar | 小米 17T Pro（天玑 9500） | [40+ fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Constance | 小米 17T Pro（天玑 9500） | [稳定 40 fps；有绿色方块贴图问题（v0.5.4 应已修复）](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Cult of the Lamb | 小米 17T Pro（天玑 9500） | [可以启动，800x600 最低预设下约 10 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Cuphead | 小米 17T Pro（天玑 9500） | [80+ fps；有绿色方块贴图问题（v0.5.4 应已修复）](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Dead Cells | 小米 17T Pro（天玑 9500） | [80+ fps，画面没有问题](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Death's Door | 小米 17T Pro（天玑 9500） | [15-20 fps；有绿色方块贴图问题（v0.5.4 应已修复）](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| ENDER LILIES | 小米 17T Pro（天玑 9500） | [40-60 fps，攻击时偶有波动](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Firewatch | 小米 17T Pro（天玑 9500） | [20-30 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Hades | 小米 17T Pro（天玑 9500） | [DX 模式 100-120 fps，Vulkan 模式 45-55；有绿色方块贴图问题（v0.5.4 应已修复）](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Hades II | 小米 17T Pro（天玑 9500） | [80-110 fps；有绿色方块贴图问题（v0.5.4 应已修复）](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Have a Nice Death | 小米 17T Pro（天玑 9500） | [30-50 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Hollow Knight | 小米 17T Pro（天玑 9500） | [60-80 fps；有绿色方块贴图问题（v0.5.4 应已修复）](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Hollow Knight: Silksong | 小米 17T Pro（天玑 9500） | [游戏中 60-75 fps，菜单 100；有绿色方块贴图问题（v0.5.4 应已修复）](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Library of Ruina | 天玑 9500 手机 (G1-Ultra) | [15-35 fps；部分背景有绿色像素](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6312412336) |
| Little Nightmares | Redmi K90 Max（天玑 9500） | [约 10 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6330586020) |
| Little Nightmares II | Redmi K90 Max（天玑 9500） | [约 10 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6330586020) |
| Lobotomy Corporation | 天玑 9500 手机 (G1-Ultra) | [约 30 fps；部分背景有绿色像素](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6312412336) |
| Megabonk | 小米 17T Pro（天玑 9500） | [开始时 50-60 fps，敌人越多越低](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Ori and the Will of the Wisps | 小米 17T Pro（天玑 9500） | [最低预设下 20-30 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Portal | 小米 17T Pro（天玑 9500） | [50-115 fps，有波动](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Reshaping Creatures | 天玑 9500 (G1-Ultra) | [可以启动；5-9 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6307692808) |
| Sable | 小米 17T Pro（天玑 9500） | [游戏中 8-24 fps，菜单 60](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19149991) |
| Slay the Spire | 天玑 9500 手机 (G1-Ultra) | [60-70 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6312412336) |

在小米 17T Pro 上还不能运行：Blasphemous 和 Dishonored 停在加载画面，SOMA 在第一次加载结束时关闭。

</details>

<details>
<summary><b>Mali-G2-Ultra（玄戒 O3）：小米 18 Fold、小米平板 9 Pro Max</b>（3 款游戏）</summary>

| 游戏 | 设备（GPU） | 结果 |
|---|---|---|
| Brotato | 小米 18 Fold（玄戒 O3） | [约 77 fps](https://github.com/ASEbrahim/MaliBridge/issues/7) |
| Human Fall Flat | 小米平板 9 Pro Max（玄戒 O3） | [可以玩](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19136786) |
| Portal 2 | 小米平板 9 Pro Max（玄戒 O3） | [1080p 下 40-50 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19137389) |

</details>

<details>
<summary><b>Immortalis-G925（天玑 9400）：vivo X200</b>（2 款游戏）</summary>

| 游戏 | 设备（GPU） | 结果 |
|---|---|---|
| CarX Drift Racing Online | vivo X200 (G925) | [最低画质下游戏中 9-20 fps，有卡顿](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6314012693) |
| Euro Truck Simulator 2 | vivo X200 (G925) | [游戏中约 20 fps（Medium，1600x720），菜单流畅](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6314012693) |

</details>

<details>
<summary><b>Immortalis-G720（天玑 9300）：realme Neo7、小米 14T Pro</b>（3 款游戏）</summary>

| 游戏 | 设备（GPU） | 结果 |
|---|---|---|
| Alan Wake | 小米 14T Pro (G720) | [Low 下 20-50 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19146763) |
| Balatro | realme Neo7 (G720) | [55-72 fps，但文字缺失（v0.5.4 已修复）](https://github.com/ASEbrahim/MaliBridge/issues/5) |
| Stardew Valley | realme Neo7 (G720) | [约 59 fps](https://github.com/ASEbrahim/MaliBridge/issues/5) |

</details>

<details>
<summary><b>Mali-G720 MC7/MC8（天玑 8400/8500）：POCO X7 Pro、POCO X8 Pro</b>（7 款游戏）</summary>

| 游戏 | 设备（GPU） | 结果 |
|---|---|---|
| Good Company | POCO X7 Pro (G720 MC7) | [可以运行；需要用 PC 键盘操作](https://github.com/ASEbrahim/MaliBridge/issues/23) |
| Hob | POCO X8 Pro (G720 MC8) | [可玩，偶有卡顿，正在排查](https://github.com/ASEbrahim/MaliBridge/issues/14) |
| Indiana Jones and the Emperor's Tomb | POCO X8 Pro (G720 MC8) | [使用 Stability FEX 预设时游戏中约 30 fps（v0.5.4 会自动选择）；Performance 预设会让它报 "invalid camera matrix" 错误并停止](https://github.com/ASEbrahim/MaliBridge/issues/9#issuecomment-6328655497) |
| Japanese Rural Life Adventure | POCO X7 Pro (G720 MC7) | [可以玩](https://github.com/ASEbrahim/MaliBridge/issues/23) |
| Rain World | POCO X7 Pro (G720 MC7) | [可以玩](https://github.com/ASEbrahim/MaliBridge/issues/23) |
| Stardew Valley | POCO X7 Pro (G720 MC7) | [可以玩](https://github.com/ASEbrahim/MaliBridge/issues/23) |
| Tukoni: Forest Keepers | POCO X7 Pro (G720 MC7) | [可以玩](https://github.com/ASEbrahim/MaliBridge/issues/23) |

</details>

<details>
<summary><b>Mali-G715（Tensor G3）：Pixel 8、Pixel 8 Pro</b>（4 款游戏）</summary>

| 游戏 | 设备（GPU） | 结果 |
|---|---|---|
| Cuphead | Pixel 8 (G715) | [60-100 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6306332804) |
| Dishonored | Pixel 8 Pro (G715) | [菜单 30 fps，序章 15-25，移动时掉帧](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19088953) |
| Hades | Pixel 8 (G715) | [30-60 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6306332804) |
| The Elder Scrolls V: Skyrim | Pixel 8 Pro (G715) | [Low 下 17-20 fps，战斗时掉帧](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19098757) |

</details>

<details>
<summary><b>Mali-G710（天玑 9000）：一加 Ace 2V</b>（2 款游戏）</summary>

| 游戏 | 设备（GPU） | 结果 |
|---|---|---|
| Half-Life 2 | 一加，天玑 9000 (G710) | [手动选择 DXVK 1.10.3 后可以启动（v0.5.4 会自动选择）](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6307774367) |
| RimWorld | 一加 Ace 2V (G710) | [用 DXVK 1.10.3 可以玩 1-2 小时（v0.5.4 会自动选择）](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6335790759) |

</details>

<details>
<summary><b>Mali-G615（天玑 8300/8350）：POCO X6 Pro 等</b>（5 款游戏）</summary>

| 游戏 | 设备（GPU） | 结果 |
|---|---|---|
| Burnout Paradise | POCO X6 Pro (G615) | [20-30 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6311114838) |
| GTA San Andreas | POCO X6 Pro (G615) | [约 30 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6311114838) |
| Palia | POCO X6 Pro (G615) | [4-10 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6311114838) |
| Prince of Persia: Warrior Within | POCO X6 Pro (G615) | [约 30 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6311114838) |
| Rivals of Aether | 天玑 8350 手机 (G615) | [约 55 fps](https://github.com/Droid-Deck/DroidDeck/issues/218#issuecomment-6322050101) |

</details>

<details>
<summary><b>Mali-G57（Helio G96）：Redmi Note 12S</b>（6 款游戏）</summary>

| 游戏 | 设备（GPU） | 结果 |
|---|---|---|
| DDNet | Redmi Note 12S (G57) | [OpenGL 模式下可以玩，目前只能离线](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19166394) |
| Death Road to Canada | Redmi Note 12S (G57) | [30-60 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19128201) |
| Left 4 Dead | Redmi Note 12S (G57) | [15-40 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19128201) |
| Space Menace | Redmi Note 12S (G57) | [60 fps，偶尔小卡顿](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19166394) |
| YOMI Hustle | Redmi Note 12S (G57) | [10-60 fps，装了 mod](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19166394) |
| Zombie Blocks 2 | Redmi Note 12S (G57) | [Low 下约 30 fps](https://github.com/ASEbrahim/MaliBridge/discussions/2#discussioncomment-19128201) |

</details>


## 最佳设置与性能建议

- **Proton：** 每个游戏都选 **Proton Experimental ARM64 (Compatible)**。列表中 Valve 官方的 “Proton (ARM64)” 和 “Steam Linux Runtime” 在 DroidDeck 里无法启动。同时有 Linux 版本的游戏（《传送门2》《半条命2》等）需要手动设置。
- **安全模式：** 保持 **Safe mode** 开启（默认）。它的兼容性最好，而在我的平板上性能模式（Safe mode 关闭）只快 0-2%。游戏能正常运行后可以试试性能模式；如果出了问题，请先切回安全模式再反馈。
- **DXVK: automatic**（默认）会在 GPU 太旧、跑不了 Proton 自带 DXVK 的手机上换用较旧的 DXVK。
- **Steam menus: automatic**（默认）可以避免较旧的 Mali GPU 上 Steam 菜单黑屏。如果 Steam 还是黑屏，改成 **Steam menus: software**，游戏仍然使用 GPU。
- **v0.5.4 会自动设置的游戏：** 《饥荒联机版》（Don't Starve Together）、较旧手机上的《半条命2》和《环世界》，以及 Indiana Jones and the Emperor's Tomb。旧版本上，《饥荒联机版》需要把启动选项设为 `PROTON_USE_WINED3D=1 %command%`。
- **等 “Processing Vulkan shaders” 处理完**：Steam 启动游戏前如果显示它，等它完成，第一次运行时卡顿会少一些。玩得越多越流畅。
- **每次会话只启动一个游戏**；8 GB 内存的手机请关掉其他应用；较重的游戏可以降低分辨率。

Safe mode、DXVK 和 Steam menus 都在 **配置**（Setup）页面的 **Mali bridge** 按钮下面。[安装指南（英文）](INSTALL.md#how-demanding-each-game-is)按负载高低给游戏分了组。

两种模式在我的平板上的帧率对比（fps 中位数，每个游戏每种模式跑一次，接 USB 供电，温控状态 0-1）。游戏是按顺序跑的，没有交替，所以几个百分点的差别属于正常误差。

| 游戏（场景） | G720 安全 | G720 性能 | G925 安全 | G925 性能 |
|---|---|---|---|---|
| Half-Life 2（火车场景） | 74.1 | 75.9 | 93.7 | 93.7 |
| Subnautica（菜单） | 62.0 | 62.3 | 81.3 | 81.5 |
| Stray（标题） | 17.1 | 15.8 | 58.4 | 58.0 |
| Stardew Valley（标题，60 fps 上限） | 60.0 | 60.0 | 60.0 | 60.0 |
| Risk of Rain 2（标题） | 35.6 | 35.8 | 56.8 | 没有完整的一次（见已知问题） |
| Skyrim Special Edition（主菜单，60 fps 上限） | 60.7 | 60.7 | 未测试 | 未测试 |

### 查看帧数

- **Steam 性能叠加层**（Steam 会话中）：按 **⋯** 键打开 Steam 快捷菜单，进入 **Performance**（⚡ 页），调整
  **Performance Overlay Level**：1 只显示帧数，4 显示全部信息（CPU、内存、电池、帧时间曲线）。只有打开时才运行。
  所有设备都能用；从 v0.5.4 起，隐藏温度传感器的三星平板也能用。手机不让应用读取的数值（常见的是 CPU 温度或 GPU 功耗）显示为 0。
- **性能 HUD**（DroidDeck Mali 自带）：侧边菜单 → **显示** → **性能 HUD**。显示帧数，以及电量、电池温度、功耗（瓦）和
  安卓的温控状态，数据来自安卓本身，所以所有设备都能用。功耗只在会报告电池电流的手机上显示，温控状态需要安卓 10 或更新版本。
  Steam 会话中默认关闭，因为已有 Steam 的叠加层。

## 获取帮助

> [!TIP]
> 最快的帮助在 [Discord](https://discord.gg/9xC67qTjDq)（中国大陆可能无法直接访问）：其他玩家和我都会在那里回答。

**Send logs to MaliBridge（v0.5.4 及以后）。**

1. 让游戏关闭，然后在侧边菜单点 **停止**（Stop）。
2. 点 **Send logs to MaliBridge**（在 **配置 > Diagnostics**、会话结束画面或侧边菜单里）。它会给你一个短代码，例如 `ML-7F3K`。
3. 把代码贴到 Discord 或 [GitHub Issues](https://github.com/ASEbrahim/MaliBridge/issues) 里。

上传的内容（同意前应用会这样说明）：你的设备型号、你运行过的游戏名称，以及这次会话的日志。账户名、密码和令牌会先被移除。日志保存 90 天，只有开发者能看到。

**也可以自己分享日志：** 在 **配置 > Diagnostics** 里点 **最新会话日志** 旁边的 **分享日志**，把生成的 zip 附到 issue 或 Discord 帖子里。

**新建 issue：** 打开 [Issues](https://github.com/ASEbrahim/MaliBridge/issues)，写上手机型号、MaliBridge 版本、你做了什么、发生了什么（比如“Steam 一直没出现”或“游戏一启动就关闭”），以及日志代码或 zip。用中文或英文写都可以，机器翻译也没问题。

**最有帮助的反馈：** 表格里没有的 Mali 设备、黑屏，以及一启动就关闭的游戏。每次会话只启动一次游戏，这样日志里只会有一次尝试。

[安装指南（英文）](INSTALL.md#if-something-goes-wrong)里有更详细的说明。

## 已知问题

- 一些边走边加载贴图的游戏（《黑暗之魂3》、Hob）在**移动时会短暂卡顿**。
- 在 **vivo 手机**上，Steam 的菜单可能只有约 15 fps，打开菜单时会白屏闪一下。如果你有 vivo 手机，v0.5.4 的日志会有帮助。
- **Mali-G925 上的《巫师3》（DX12）**开场动画是 60 fps，但读取存档后帧率掉到 1 fps 以下。
- **Mali-G925 上的 Stray** 颜色较平淡，没有游戏自己的调色效果（Poppy Playtime 和 ASTRONEER 也是）。正在处理。
- **Stray 加 `-dx12` 启动**时，在 G925 上大约 60 秒后关闭。默认的 DX11 没有问题。
- **Risk of Rain 2**：在 G925 上有一次性能模式启动时，标题加载约 2 分钟没有画面，之后恢复正常。之后没有再出现。
- **Mali-G925 上的 Steam 窗口**可能有些慢。游戏不受影响。
- **Just Cause 2** 无法启动：Steam 的防拷贝检查在 DroidDeck 里耗时太长。
- **《生化危机2》**菜单上可能出现 “Failed to send play data to RE NET”。按 OK 即可，游戏正常运行，只影响 Capcom 的在线游玩统计。
- **《黑暗之魂3》在较慢的设备上**可能提示 “Frame rate unsuitable for online play” 并回到标题画面。这是游戏自己的规则（低于约 20 fps）：在这些设备上请离线游玩。
- **Outer Wilds、ASTRONEER 和 The Forest** 在 G925 上受 CPU 限制，降低分辨率或画质帮助不大。

## 接下来

- 修复移动时的短暂卡顿（《黑暗之魂3》、Hob）。
- 更多性能优化。
- 第一次进入新区域时更流畅。
- 受 CPU 限制的游戏（The Forest、ASTRONEER、Outer Wilds）以及 Just Cause 2。
- DirectX 12：《巫师3》卡住的问题、DX12 下的 Stray、《艾尔登法环》。
- 更多设备：来自我没测试过的 Mali GPU 的反馈，是最快的途径。

## 常见问题

**需要多少存储空间？**
游戏之外约 **8-10 GB**：Linux 运行环境、Steam 客户端和 Proton。每个游戏另外需要它本身的大小，再加几百 MB。

**可以保留官方 DroidDeck 吗？**
可以。DroidDeck Mali 是独立的应用，有自己的 Steam 登录、游戏和设置。

**带反作弊的网络游戏能玩吗？**
我没有测试过。很多反作弊系统在 Proton 下根本无法运行，所以基本无法游玩。

**和 PanVK 一样吗？**
不一样。PanVK 会替换手机的驱动，MaliBridge 保留手机自带的驱动。它们是不同的项目。

**能在 GameNative、Winlator 等应用里用吗？**
不能。MaliBridge 内置在 Releases 页面的 DroidDeck Mali 里。

**免费吗？是官方的吗？**
免费。它不是官方 DroidDeck 的一部分：DroidDeck Mali 是一个可以配合 DroidDeck 使用的版本，由一个单独的项目制作。

更多问题见[英文安装指南的 FAQ](INSTALL.md#faq)。所有版本：[发布历史](https://github.com/ASEbrahim/MaliBridge/releases)。

## 交流与支持

所有测试、发送日志和支持 MaliBridge 的朋友都在 [致谢名单](CREDITS.zh-CN.md) 里。谢谢大家！

- Discord：[加入服务器](https://discord.gg/9xC67qTjDq)，获取帮助和新消息（中国大陆可能无法直接访问）
- 支持开发：[Ko-fi](https://ko-fi.com/asebrahim)
- 视频：[哔哩哔哩](https://space.bilibili.com/3747573858831484) · [YouTube](https://www.youtube.com/@malibridge)
- 反馈问题和设备报告：[GitHub Issues](https://github.com/ASEbrahim/MaliBridge/issues)

## 致谢与许可

MaliBridge 采用 GPL-3.0-or-later 许可，并在第 7 节附加了两条条款（见 [NOTICE](NOTICE)）：发布 MaliBridge 或基于它的作品时，需要保留明显的 “MaliBridge by ASEbrahim” 署名和指向本页的链接；修改过的版本不能自称 MaliBridge。详见 [LICENSE](LICENSE)。源文件都带有 SPDX 许可声明头和版权行。

<details>
<summary>名称、DroidDeck 与致谢</summary>

**致谢：** 感谢 Mondyro 找到天玑 9500 的贴图问题（[#24](https://github.com/ASEbrahim/MaliBridge/issues/24)）。

**关于名称。** 欢迎在许可范围内 fork，但 “MaliBridge” 这个名字只指本项目；请给你的 fork 起一个自己的名字。

DroidDeck Mali 基于 [DroidDeck](https://github.com/Droid-Deck/DroidDeck)（GPL-3.0），但不是官方 DroidDeck 的一部分。第三方代码和致谢名单见[英文 README](README.md#credits-and-licence)。

</details>
