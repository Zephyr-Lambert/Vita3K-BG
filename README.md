# Vita3K

> [!IMPORTANT]
> **This is Vita3K-BG, an unofficial fork of Vita3K** with fixes aimed at *Bullet Girls* (PCSG00361).
> Please don't report bugs from this build to the official Vita3K project.
>
> **Fixed so far**
> - Audio cutting out about 1:45 into a mission, followed by the game hanging at mission end
>   (`sceAudiodecPartlyDecode` wrote past the game's output buffer at music loop points).
> - OpenGL: extra copies of the character's shadow on walls and the ground
>   (render targets used as textures ignored the game's wrap mode and repeated).
> - OpenGL: on Wayland desktops the game window showed the desktop behind it.
>   The build now uses X11 through XWayland automatically (set `VITA3K_BG_WAYLAND=1` to opt out).
> - OpenGL: the game's depth bias (polygon offset) was set but never switched on.
> - Both renderers: triangular cuts in walls and floors at certain camera distances
>   (a triangle corner just in front of the camera had its depth clamped per vertex, which pushed the
>   whole triangle behind the surface beneath it; depth is now clamped per pixel instead).
> - Vulkan (rare, setup-dependent): stage textures misaligned or shifting every frame. Seen on an
>   NVIDIA RTX 2060 on Linux; other users' Vulkan setups were not affected. The stage shaders read
>   registers that are never loaded; most drivers return 0 there, this one returned leftover data.
>   Registers now start at 0 and out-of-range reads return 0. See [BULLET_GIRLS_NOTES.md](BULLET_GIRLS_NOTES.md).
> - Vulkan with Renderer Accuracy set to High (rare; the default is Standard): bloom lagged behind the scene,
>   leaving a glowing trail when turning. The blur passes read a copy of the bloom image taken before
>   the previous pass had drawn. The copy is now taken after each pass.
>
> Recommended: Renderer Accuracy on Standard (the default). Both settings now look the same in this game,
> and Standard is faster.
>
> **Known issues**
> - The first mission load after updating the build can sit on a black screen for a while
>   (with audio) while shaders are rebuilt. It only happens once per update.
>
> Everything below is the original Vita3K README.

[![C/C++ CI](https://github.com/Vita3K/Vita3K/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/Vita3K/Vita3K/actions/workflows/c-cpp.yml)
[![Release](https://img.shields.io/github/v/release/Vita3K/Vita3K-builds?include_prereleases)](https://github.com/Vita3K/Vita3K/releases)
[![Vita3K discord server](https://img.shields.io/discord/408916678911459329?color=5865F2&label=Vita3K%20&logo=discord&logoColor=white)](https://discord.gg/6aGwQzh)

## Introduction

Vita3K is an experimental PlayStation Vita emulator for Windows, Linux, macOS and Android.

* [Website](https://vita3k.org/) (information for users)
* [Wiki](https://github.com/Vita3K/Vita3K/wiki) (information for developers)
* [Discord server](https://discord.gg/MaWhJVH) (recommended)

## Compatibility

The emulator currently runs most homebrew programs and commercial games.

- [Homebrew compatibility page](https://vita3k.org/compatibility-homebrew.html)
- [Commercial compatibility page](https://vita3k.org/compatibility.html)

## Gallery

|               **Persona 4 Golden** by Atlus                   |                     **A Rose in the Twilight** by Nippon Ichi Software                         |
| :-----------------------------------------------------------: | :--------------------------------------------------------------------------------------------: |
| ![Persona 4 Golden screenshot](./_readme/screenshots/P4G.png) | ![A Rose in the Twilight screenshot](./_readme/screenshots/A%20Rose%20in%20the%20Twilight.png) |

|                  **Alone with You** by Benjamin Rivers                     |                 **VA-11 HALL-A** by Sukeban Games                    |
| :------------------------------------------------------------------------: | :------------------------------------------------------------------: |
| ![Alone with You screenshot](./_readme/screenshots/Alone%20With%20You.png) | ![VA-11 HALL-A screenshot](./_readme/screenshots/VA-11%20HALL-A.png) |

|              **Fruit Ninja** by Halfbrick Studios                  |                **Jetpack Joyride** by Halfbrick Studios                    |
| :----------------------------------------------------------------: | :------------------------------------------------------------------------: |
| ![Fruit Ninja Screenshot](./_readme/screenshots/Fruit%20Ninja.png) | ![Jetpack Joyride Screenshot](./_readme/screenshots/Jetpack%20Joyride.png) |

## License

Vita3K is licensed under the **GPLv2** license. This is largely dictated by external dependencies, most notably Unicorn.

## Downloads

You can download the latest builds from [here](https://github.com/Vita3K/Vita3K/releases/tag/continuous).

* Windows
  * Requirements:
    * [Microsoft Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe)
* Linux
  * Arch based:
    * [vita3k-bin](https://aur.archlinux.org/packages/vita3k-bin)<sup><small>AUR</small></sup>
    * [vita3k-git](https://aur.archlinux.org/packages/vita3k-git)<sup><small>AUR</small></sup>
  * Requirements:
    * xdg-desktop-portal
    * OpenGL or Vulkan runtime libraries
* Android
    * [Adreno drivers](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/)
* Others
  * [Download Artifact](https://github.com/Vita3K/Vita3K/actions?query=event%3Apush+is%3Asuccess+branch%3Amaster)
  * [Old builds](https://github.com/Vita3K/Vita3K-builds/releases)

## Building

Please see [`building.md`](./building.md).

## Running
Check our [quickstart guide](https://vita3k.org/quickstart) to make sure your computer meets the minimum requirements to run Vita3K.  
Don't forget to have your graphics driver up to date and to install the [Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/VC_redist.x64.exe) if you are a Windows user.  

## Bugs and issues
The project is in an early stage, so please be mindful when opening new issues. Expect crashes, glitches, low compatibility and poor performance.

## Thanks
Thanks go out to people who offered advice or otherwise made this project possible, such as Davee, korruptor, Rinnegatamante, ScHlAuChi, Simon Kilroy, TheFlow, xerpi, xyz, Yifan Lu and many others.

## Donations
[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/vita3k)
<br>
Thank you to the supporters and to all those who support us on our [ko-fi](https://ko-fi.com/vita3K).
* Among them, those who subscribed to the Nibble Tier and upper: **j0hnnybrav0, Mored4u, TacoOblivion, Undeadbob and uplush**

## Note
The purpose of this emulator is not to enable illegal activity. You can dump games from a Vita by using [NoNpDrm](https://github.com/TheOfficialFloW/NoNpDrm) or [FAGDec](https://github.com/CelesteBlue-dev/PSVita-RE-tools/tree/master/FAGDec/build). You can get homebrew programs from [VitaDB](https://www.rinnegatamante.eu/vitadb/#/).

PlayStation, PlayStation Vita and PlayStation Network are all registered trademarks of Sony Interactive Entertainment Inc. This emulator is not related to or endorsed by Sony, or derived from confidential materials belonging to Sony.
