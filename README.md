<div align="center">

![ARMSX2-NX](app_icons/icon.png)

# ARMSX2-NX

[![License](https://img.shields.io/github/license/ARMSX2/ARMSX2)](https://www.gnu.org/licenses/gpl-3.0.html)
(https://patreon.com/ARMSX2)
(https://github.com/sponsors/PCSX2)

</div>

ARMSX2-NX (please help me come up with a better name) is a free and open-source PlayStation 2 emulator for the Nintendo Switch based on [ARMSX2](https://armsx2.net/).

ARMSX2-NX is in no way associated with the ARMSX2 team. Do not contact them regarding any bugs or help with this port of the emulator. They have nothing to do with this project.

The goal here is to prove that PS2 games can run on the Switch, not that they run well. They very much don't.

**NOTE: Do NOT expect playable performance out of this project. The only full-speed thing I've seen run is the PS2 BIOS, load screens, and the MegaMan Legacy Collection.**

## Requirements

You need to have a hacked Nintendo Switch to run this. You also will need to overclock your Switch to get even somewhat acceptable performance. I'm not going to explain how to do that here, that's on you to figure out.

Please note that a BIOS dump from a legitimately-owned PS2 console is required to use the emulator.

## Download

ARMSX2-NX can be downloaded from [Github Releases](https://github.com/PalindromicBreadLoaf/ARMSX2-NX/releases).

## Building
This assumes you have DevkitPro installed. Install it if you don't.

```shell
(dkp-)pacman -S switch-dev switch-libpng switch-libjpeg-turbo switch-curl switch-ntfs-3g switch-lwext4
```

- `switch-curl` is required for RetroAchievements (the Switch build always links curl).
- `switch-ntfs-3g` and `switch-lwext4` are required for USB game loading.

Initialize the submodules after cloning:

```shell
git submodule update --init --recursive
```

Only `switch/externals/libusbhsfs` is needed for the Switch build. The other submodules are used by the
Android app, so `git submodule update --init switch/externals/libusbhsfs` is enough if you only build the `.nro`.

### NXVK (Vulkan)

The Switch build is Vulkan only. The Vulkan driver comes from NXVK, which has to be installed as a
Switch portlib before configuring ARMSX2-NX:

```shell
git clone https://github.com/PalindromicBreadLoaf/nxvk
cd nxvk
make
sudo make install
```

The Switch build discovers the installed `nxvk` package through devkitPro's target `pkg-config`.

### Building the `.nro`

Configure the native core with the Switch toolchain, then build the `.nro` target:

```shell
cmake -S app/src/main/cpp -B build/switch-core \
      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build/switch-core --target armsx2_nro -j$(nproc)
```

The output binary will be located at `build/switch-core/switch-app/armsx2nx.nro`

The GitHub Actions workflow `.github/workflows/switch_build.yml` does all of the above and uploads the
`.nro` as a build artifact.

### Optional renderers

OpenGL (Zink) and deko3d are off by default on Switch. They can still be built:

- `-DUSE_OPENGL=ON` needs NXVK's OpenGL package (`make gl` / `sudo make install-gl`) and EGL headers
  for CMake's `find_package(EGL)` (devkitPro's `switch-mesa`).
- `-DUSE_DK3D=ON` builds the deko3d renderer. It compiles shaders at runtime with uam, which is built into
  the `.nro` from source: clone https://github.com/devkitPro/uam next to the ARMSX2-NX checkout (or pass
  `-DUAM_ROOT=/path/to/uam`) and install `bison`, `flex` and `python3` with `mako`. Add
  `-DDK3D_RUNTIME_SHADERS=OFF` to use only the prebuilt shaders instead.

## Controls

| Switch | PS2 |
| --- | --- |
| A / B / X / Y | Circle / Cross / Triangle / Square |
| L / R | L1 / R1 |
| ZL / ZR | L2 / R2 |
| Left / right stick click | L3 / R3 |
| + / - | Start / Select |

Hold **+** and **-** together to open the pause menu. Press both at the same time: if one of them is
pressed noticeably earlier, the game may still see that button first.

Two players are supported. Player 1 is the first controller (or the attached Joy-Cons in handheld mode),
player 2 is the second controller. Use **Controller Setup** in the pause menu to open the system controller
applet and assign the controllers to players.

## CPU cores

The EE, VU1 and GS threads are pinned to the first three cores the process may use. Every other thread
(audio, ISO/USB reading, input, the Vulkan driver's helpers) is moved off the EE core when it starts: to
the GS core, and to core 3 when the process is allowed to use it. The log (`sdmc:/switch/armsx2/logs/emulog.txt`)
shows which cores were granted, e.g. `process may use mask 0x7` for cores 0-2 or `0xf` for cores 0-3.

Launched from hbmenu the app gets cores 0-2. A forwarder runs with the cores its `main.npdm` allows:
forwarders built from hbloader's NPDM also allow 0-2 (`"highest_cpu_id": 2`). With
`"highest_cpu_id": 3` in the forwarder's NPDM, core 3 is used for the helper threads. Core 3 is shared
with the system, which is why the EE/VU1/GS threads stay on cores 0-2.

With [sphaira](https://github.com/berlint-hub/sphaira) there are two ways to get core 3:

- Create a forwarder for `armsx2nx.nro` and set **CPU Cores** to **4 (Advanced)** in the forwarder editor.
- Or, while sphaira itself runs from its forwarder, open the options for `armsx2nx.nro` in the homebrew
  menu and set **CPU Cores** to **4 (Advanced)**.

## Updating

Every push builds the `.nro` with GitHub Actions and publishes it to an update channel: a pre-release on
this repository tagged `nightly` for the default branch, or `nightly-<branch>` for other branches.

Choose **Update** in the app's main menu to check the channel the running build came from. If there is a
newer build, the app downloads it and installs it when it closes. **Restart now** restarts straight into
the new build. The previous build is kept next to it as `armsx2nx.nro.bak`. The app also checks once on
startup when it is online. Both need an internet connection on the Switch.

The defaults come from the build. They can be overridden in `sdmc:/switch/armsx2/armsx2.ini`:

```ini
[Horizon]
CheckForUpdatesOnStartup = true
UpdateRepository = berlint-hub/Yadokari
UpdateChannel = nightly
```

## USB game loading

USB mass-storage drives are mounted automatically and their roots are scanned for games. Supported
filesystems are FAT12/16/32, exFAT, NTFS, and EXT2/3/4. USB volumes are mounted read-only to prevent
corruption if they are unexpectedly unplugged.

Do not unplug a drive while a game is running from it. Return to the game list first so ARMSX2 has
closed the image file.

## UI

You will also need the resources from the releases page to properly display the fullscreen UI.

## Affiliation

ARMSX2-NX is NOT affiliated with ARM Holding LTD in any way shape or form. Nor is the project associated with ARMSX2 or PCSX2 or any derivatives of said projects. Please donate to either of these projects before even trying to donate to this.

## Contributing

All contributions to this project are welcome! There's a lot still to be re-implemented that was stubbed out from ARMSX2 and a lot of performance that needs to be gained if possible.

If you don't know/don't want to code, please just open issues for games not rendering properly, app crashes, missing features, or anything else that needs done/fixing.

## Additional Credits

[ARMSX2](https://armsx2.net/) - ARMSX2-NX would never have got anywhere near the performance it (already doesn't) have without the outstanding work of the ARSMX2 team. 

[PCSX2](https://github.com/PCSX2/pcsx2) - ARMSX2 would not be possible without the legendary work from the PCSX2 team and their patience and understanding regarding this project!

[PCSX2_ARM64](https://github.com/pontos2024/PCSX2_ARM64) - ARMSX2 originally started off as a fork of developer Pontos work. 

Thank you to [@fffathur](https://github.com/fffathur) and [@Vivimagic](https://github.com/Vivimagic) for creating and working on the logo!
