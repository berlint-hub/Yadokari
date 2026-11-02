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
(dkp-)pacman -S switch-dev deko3d uam switch-libpng switch-libjpeg-turbo \
                switch-curl switch-ntfs-3g switch-lwext4
```

`uam` is the deko3d shader compiler, which turns the GLSL in `pcsx2/GS/Renderers/DK3D/shaders` into
the `.dksh` blobs bundled in the romfs. `switch-libpng`/`switch-libjpeg-turbo`/`switch-curl` are the
image and networking libraries the core links against, and `switch-ntfs-3g`/`switch-lwext4` are
required for USB game loading.

Initialize the submodules after cloning. The Switch build fails early without them:

```shell
git submodule update --init --recursive
```

### Vulkan (NXVK)

Vulkan is the default renderer, so install [NXVK](https://github.com/PalindromicBreadLoaf/nxvk) as a
devkitPro portlib before configuring. It is a Mesa cross build and wants its own toolchain image, see
the NXVK README for the details:

```shell
git clone https://github.com/PalindromicBreadLoaf/nxvk
cd nxvk
make                      # build the driver and stage the portlib
sudo make install         # copy libnvk.a + nxvk.pc into $DEVKITPRO/portlibs/switch
```

The Switch build discovers the installed `nxvk` package through devkitPro's target `pkg-config`.

### deko3d runtime shaders

The deko3d backend specialises its tfx fragment shader per draw, which needs the
[uam](https://github.com/devkitPro/uam) sources next to the checkout (`../uam` by default; point
`-DUAM_ROOT=` somewhere else if yours live elsewhere). If you don't want that, configure with
`-DDK3D_RUNTIME_SHADERS=OFF` to build with only the prebuilt tfx variants.

### Configure and build

```shell
cmake -S app/src/main/cpp -B build/switch-core \
      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DUSE_OPENGL=OFF \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build/switch-core --target armsx2_nro -j$(nproc)
```

`USE_OPENGL=OFF` is deliberate. The Switch has no GL stack of its own, the only one on offer is the
Zink/EGL half of NXVK, and this port doesn't ship it. Leaving the option at its default makes CMake
look for an `egl` package that isn't installed.

The output binary will be located at `build/switch-core/switch-app/armsx2nx.nro`

### Prebuilt CI artifacts

`.github/workflows/switch_build.yml` builds the Vulkan `.nro` (NXVK included) on GitHub Actions and
uploads `armsx2nx.nro` as an artifact. The NXVK portlib is cached between runs, so only the first run
after the NXVK commit changes pays for the Mesa cross build.

## Graphics backends

The renderer is picked by `Renderer=` in the `[EmuCore/GS]` section of `sdmc:/switch/armsx2/armsx2.ini`
(FullscreenUI writes the same key). The values are `GSRendererType` ordinals:

| Renderer | Value | Notes |
| --- | --- | --- |
| Vulkan | `14` | Default. Requires NXVK. |
| Deko3D | `18` | Native deko3d backend, no NXVK needed. |
| Software | `13` | No GPU involved. Slowest, best compatibility. |
| Null | `11` | No output at all, for debugging. |

If `armsx2.ini` doesn't exist it is generated from `GSUtil::GetPreferredRenderer()`, so a build with
Vulkan compiles to Vulkan and a build without it falls back to software.

## Files and paths

Everything lives under `sdmc:/switch/armsx2`:

| Path | Contents |
| --- | --- |
| `armsx2.ini` | Settings, including the selected renderer and the USB game roots. |
| `games/` | Default game directory, scanned recursively at startup. |
| `bios/` | PS2 BIOS images. Set the file name in `[Filenames] BIOS=`. |
| `logs/emulog.txt` | Emulator log. |

## USB game loading

USB mass-storage drives are mounted automatically and their roots are scanned for games. Supported
filesystems are FAT12/16/32, exFAT, NTFS, and EXT2/3/4. USB volumes are mounted read-only to prevent
corruption if they are unexpectedly unplugged.

Do not unplug a drive while a game is running from it. Return to the game list first so ARMSX2 has
closed the image file.

## UI

You will also need the resources from the releases page to properly display the fullscreen UI. They
are baked into the romfs at build time, so a build from this repo already contains them.

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
