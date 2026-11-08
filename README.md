# TotK Recomp (Generic Switch Recomp Fork)

> [!NOTE]
> ### 🌟 About This Fork & Upstream Acknowledgement / 关于本 Fork 与致谢上游
>
> **致谢上游 (Upstream Acknowledgement):**  
> 本项目是 [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) 的下游分支。我们对 [valentderah](https://github.com/valentderah) 及其所有贡献者在《塞尔达传说：王国之泪》(TOTK) 原生静态重编译上的开创性工作表示最崇高的敬意与由衷感谢！上游项目奠定了卓越的逆向工程、静态重编译工具链和底层图形/音频运行时基础。
>
> *(We express our sincere gratitude and highest respect to [valentderah](https://github.com/valentderah) and all contributors of `totk-pc-recomp` for their groundbreaking work on statically recompiling The Legend of Zelda: Tears of the Kingdom into native PC code.)*
>
> **定位与愿景：探索通用的 NS 游戏重编译 (Generic Switch Recompilation):**  
> 官方上游项目专注于 TotK 单一游戏。而本 Fork 的核心目标是**探索通用的 Nintendo Switch 游戏原生静态重编译架构**。我们引入了模块化 **Target Seam 抽象机制**，扩展了对不同版本 Nintendo SDK（包括早期 SDK 9.x+）的系统调用与运行时支持，并补齐了通用图形/音频/文件系统管线（如 Separate Shader Bindings、DrawArraysIndirect、Dynamic NPDM 主线程配置、RomFS 目录遍历、Audren 音频路由等）。目前本运行时已支持：
> - 🗡️ **The Legend of Zelda: Tears of the Kingdom**（包含 Linux/UMA APU 硬件加速、OSD 监控、无边框全屏等优化）
> - 🌸 **Atelier Ryza 2: Lost Legends & the Secret Fairy / 莱莎的炼金工房2**（原生 50-60 FPS 流畅运行，全面适配 Gust 引擎特性）
>
> **分支架构与维护指南 (Branch Strategy & Workflow):**
> - [`totk`](https://github.com/snailium/totk-pc-recomp/tree/totk)：TotK 优化主线，包含 Linux 平台增强、AMD UMA 自动优化预设、GPU 监控与本地修复，紧跟 `upstream/main`。
> - [`ryza`](https://github.com/snailium/totk-pc-recomp/tree/ryza)：多游戏重编译扩展线，基于 `totk` 分支，集成 Target Seam 抽象与通用 SDK 增强特性。
> - 详细同步维护指南请查阅 [docs/WORKFLOW.md](docs/WORKFLOW.md)。
> - 所有被上游拒收的通用 PR 及技术复盘详见 [GitHub Issues](https://github.com/snailium/totk-pc-recomp/issues)。

**TotK Recomp** is a static recompilation of *The Legend of Zelda: Tears of the Kingdom* (Nintendo Switch, 2023) into a native x86-64 application for Windows and Linux ([docs/architecture.md](docs/architecture.md)).

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Tears of the Kingdom*, dumped from your own console: the base game and the 1.1.0, 1.2.1 or 1.4.3 update. Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not share it.

## Status

Early development. The game is playable on Windows and Linux; visual bugs and crashes are still expected.

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- Native code, not emulation
- Game updates 1.1.0, 1.2.1 and 1.4.3; 1.2.1 and 1.4.3 saves carry over between them
- Takes NSP and XCI game files, and compressed NSZ and XCZ as they are: the setup unpacks them itself
- 60 fps by default; up to 144 fps or the monitor's refresh rate (experimental)
- Custom resolutions (1080p, 2k...), supersampling and AMD FSR 1 upscaling
- NVIDIA DLSS / DLAA on RTX GPUs: the setup downloads `nvngx_dlss.dll` from NVIDIA's DLSS SDK repository
  (github.com/NVIDIA/DLSS, NVIDIA RTX SDK license) when it finds an RTX GPU; untick it in the setup to skip it
- HDR10 output with peak, game and HUD brightness settings
- 16:10 (Steam Deck, handhelds), 3:2 (Surface, Framework), 21:9 and 32:9 (ultrawide) game resolutions, with the camera and the interface in that shape
- Anisotropic texture filtering up to 16x, for sharp ground into the distance
- In-game settings menu (F1) and performance overlay (F10)
- Play time across runs on the Game tab of the settings menu, kept in `playtime.txt` next to the saves
- Discord Rich Presence ("Playing TotK Recomp" with the play time), off by default, own application id supported
- Setup and in-game menu in English, Russian, Brazilian Portuguese and Simplified Chinese
- Keyboard, mouse and gamepad with remapping, rumble and gyro
- Stereo or 5.1 surround sound output
- Xbox button prompts: LB, RB, LT, RT, Menu and View, and A, B, X and Y as labelled on the button to press, with either button layout
- Shaders prepared at install instead of during play
- Mods made for emulators: unpack a mod into the `mods` folder next to `totk.exe`; switch and order them in the menu. RomFS
  mods, TKMM's merged output and ExeFS code patches (`.ips`, `.pchtxt`) work; the game rebuilds itself for code patches
  ([docs/mods.md](docs/mods.md)). HD texture packs work: the port grows the texture heap a pack asks for by itself. Tested
  with the HDPacks.com 4K Texture Pack v1.0 on 1.4.3 and 1.2.1; other packs come with no guarantee
- Atmosphère cheats: put a cheat file into a mod and switch its cheats in the menu ([docs/cheats.md](docs/cheats.md))

## Planned

- VRR

## Installing

### Windows

Requirements: Windows 10 or later, an x86-64 CPU with SSE4.2 (AVX2/FMA machines get a faster build), a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`, about 36 GiB free during setup (20 GiB after).

1. Download `TotK-Setup-<version>-windows.zip` from [Releases](../../releases/latest) and unpack it.
2. Start `TotK-Setup.exe`. If SmartScreen warns about an unsigned app, choose *More info*, then *Run anyway*.
3. Pick your game files, the language and an install folder, and press *Start*. The setup builds the game once. With
   an NVIDIA RTX GPU it also downloads NVIDIA DLSS, unless you untick *Download NVIDIA DLSS*; turn DLSS on in the
   menu (F1).
4. Play `totk.exe` from the install folder.

### Linux

Requirements: as on Windows, plus Linux 6.7 or later and glibc 2.34 or later (Ubuntu 22.04, Debian 12, Fedora 35, SteamOS 3).

1. Download `TotK-Setup-x86_64.AppImage` from [Releases](../../releases/latest), or build it ([docs/installer.md](docs/installer.md), "Linux release").
2. Make it executable (`chmod +x TotK-Setup-x86_64.AppImage`) and start it. On a Steam Deck, do this in Desktop Mode.
3. Pick your game files, the language and an install folder, and press *Start*.
4. Play `totk` from the install folder.

To update, run a newer setup on the same folder; saves and settings are kept. Settings are in the in-game menu and in `totk.ini` ([totk.ini.example](totk.ini.example) lists every key). Controls: [docs/controls.md](docs/controls.md).

## Credits

TotK Recomp builds on the projects below. What was taken from each, with its license, is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) (Official upstream project)
- [LAS Recomp](https://github.com/peiikai/LASRecomp)
- [MacNativeGames](https://github.com/ly05010419/MacNativeGames)
- [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and [nxnative](https://github.com/seedds/nxnative)
- [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)
- [Ryujinx](https://github.com/Ryujinx/Ryujinx)
- [deko3d](https://github.com/devkitPro/deko3d)
- [hactool](https://github.com/SciresM/hactool)
- [SDL 3](https://libsdl.org), [Dear ImGui](https://github.com/ocornut/imgui), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [libopus](https://opus-codec.org), [libvpx](https://chromium.googlesource.com/webm/libvpx), [Zstandard](https://github.com/facebook/zstd)
- [Switchbrew](https://switchbrew.org) and the Switch homebrew community

## License

The project's own code is under the [MIT License](LICENSE); third-party code keeps its own license ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The license grants nothing for Nintendo's game.

TotK Recomp is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo. *The Legend of Zelda*, *Tears of the Kingdom* and *Nintendo Switch* are trademarks of Nintendo.

## Building from source

[docs/building.md](docs/building.md) lists the toolchain and the steps. Everything the build writes stays under `build\` and `generated\`, which git ignores; run `tools/check_content.py` before a commit. Testers: [docs/tester-guide.md](docs/tester-guide.md).

Most of the code and documentation is written by AI coding agents (Claude Code) under the maintainer's direction; the maintainer plays every build and decides what is merged. [AGENTS.md](AGENTS.md) holds the rules they work under.
