# Shadow of the Colossus PC

An independent PC port of the **North American PS2 edition (SCUS-97472)** with a
Linux and Windows launcher, a local recompiler, and a native OpenGL renderer.
Built on [Celosia](https://github.com/HenryKun55/celosia) and
[PS2Recomp](https://github.com/ran-j/PS2Recomp).

## How it works

The disc’s MIPS executable and the KERNEL, MANAGER, and GAMECORE modules are
translated to C++ and compiled for x86_64 on your computer. The port reads models,
textures, animations, and audio from your disc and runs without PCSX2 or a BIOS.
The runtime implements the PS2 services the game uses. Manual decompilation
continues upstream in Celosia.

**No ROM, BIOS, or generated game code is distributed.**

## Language support

**English and Brazilian Portuguese (Português) are available in the launcher
and in the port’s in-game settings menu.** The two settings are independent.

| Where | How to change the language |
| --- | --- |
| Launcher | Open **Settings → Launcher language** (**Configurações → Idioma do launcher**), choose **English** or **Português**, and select **Apply** (**Aplicar**). The launcher also offers **Español** and **日本語**. It initially uses the system language if supported, with English as the fallback. |
| In-game settings | Press **F1** and open **System → Language** (**Sistema → Idioma**). Choose **English**, **Português**, or **Automatic** to follow the system language. With a controller, press **Select/Back** twice quickly to open the menu. |

The original game’s text and audio come from your disc.

## Campaign validation

**25% validated on Linux · 4 of 16 colossi.** Valus, Quadratus, Hydrus, and
Basaran are **100% validated** on the versions shown below. Two colossi have
been partially tested and ten are pending. The ending and credits are also
pending validation.

A colossus counts toward the total after testing the route, fight, HUD, weak
points, defeat effects, and save reload. Defeats recorded on imported memory
cards are outside the count. Results apply to the Linux versions listed.

| # | Colossus | Validation | Evidence or next test |
| --- | --- | --- | --- |
| 1 | Valus | **100% validated** | Fight, defeat, save, and reload verified. [Linux v0.2.7](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.2.7) |
| 2 | Quadratus | **100% validated** | Route, climbing, HUD, both sigils, defeat, save, and reload verified. [Linux v0.9.5](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/v0.9.5) · [Test record](docs/CAMPAIGN-VALIDATION.md#quadratus-na-v094-linux) |
| 3 | Gaius | Partial | The ascent, surface, and descent of the ramp were tested. The fight is pending. [Linux v0.3.1](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.3.1) |
| 4 | Phaedra | Pending | — |
| 5 | Avion | Pending | — |
| 6 | Barba | Pending | — |
| 7 | Hydrus | **100% validated** | Rematch, sigil before damage, underwater camera, defeat, and reload on shore verified. [Linux v0.9.4](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/v0.9.4) · [Test record](docs/CAMPAIGN-VALIDATION.md#hydrus-na-v093-linux) |
| 8 | Kuromori | Pending | — |
| 9 | Basaran | **100% validated** | Fight, sigil, defeat effects, save, and reload verified. [Linux v0.3.4](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.3.4) |
| 10 | Dirge | Pending | — |
| 11 | Celosia | Pending | — |
| 12 | Pelagia | Pending | — |
| 13 | Phalanx | Pending | — |
| 14 | Cenobia | Pending | — |
| 15 | Argus | Pending | — |
| 16 | Malus | Partial | The route was tested past the second tunnel. The fight and ending are pending. |

[Validation criteria and test history](docs/CAMPAIGN-VALIDATION.md)

<img width="1109" height="751" alt="Shadow of the Colossus PC screenshot" src="https://github.com/user-attachments/assets/856dbfab-25f1-40a2-95b1-1b8060d062c9" />

## Play

1. Download the package for your system from [Releases](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases).
2. Extract the entire package and open `ShadowOfTheColossusPC` on Linux or
   `ShadowOfTheColossusPC.exe` on Windows.
3. Choose your North American disc image (`.iso`, `.7z`, `.zip`, or `.rar`).
4. Wait for the first build. On later launches, select **Play**.

Use **Compile / update** next to **Play** to prepare a build in the background
from an imported disc. The launcher shows **Build ready** when it finishes.
Select **Play** when you’re ready. **Compile / update** also checks the cache
and prepares a build for an updated runtime.

The compiler is bundled, so you can play without installing Docker or an SDK
or running commands. Initial setup can take a while. Up to two compilation
jobs run in parallel, and the launcher shows their progress. The launcher keeps
separate caches for each compiler and runtime version and verifies a cache
before reusing it. Updates limited to the launcher interface, shaders, or
documentation reuse the existing build.

### RecompLauncher

The packages include `recomplauncher.json` so RecompLauncher can set up the
port from a North American SCUS-97472 ISO. RecompLauncher runs preparation
in the background. Its settings cover aspect ratio, full screen, window size,
VSync, FSR 1, FPS display, and volume.

Both launchers use the same data directory, memory cards, settings, and
recompilation cache. At startup, the game uses the imported disc and verifies
the runtime cache.

To update through RecompLauncher’s library, open the **Updates** tab. The
catalog offers approved versions for each platform. To download approved
updates automatically, enable automatic updates in **Manage**. Initial setup
uses an `.iso` disc image.

The common requirements are an x64 CPU with SSE4.1, a GPU with OpenGL 3.3,
8 GB of RAM, and 15 GB of free storage. Linux requires **glibc 2.39 or later**
(Ubuntu 24.04+, Fedora 40+). The portable Windows package requires Windows 10
version 1903 or later, or Windows 11. A modern CPU and 16 GB of RAM are
recommended for recompilation.

The game prefers the discrete GPU on hybrid systems. On Linux, the launcher
uses `switcheroo-control` to select the driver when it starts the game. On
systems without that service, fallback detection covers NVIDIA configurations
and laptops with both integrated and discrete Radeon graphics. Explicit driver
settings such as `DRI_PRIME=0` take priority. On Windows, the launcher requests
high performance for the compiled `game.exe` and respects an existing preference
in Windows graphics settings.

The active GPU appears in `last-game.log` under `Renderer:`. The `[stats]`
lines show game timing, window presentation rate, and uploads of new frames
to the GPU. Toggle the on-screen counter with **Graphics → Show FPS** in the
launcher, **F1 → Video → Show FPS** during play, or **FPS display** in RecompLauncher.

Open **About** to create a shortcut in your applications menu. The data location
is `~/.local/share/ShadowOfTheColossusPC` on Linux (under `XDG_DATA_HOME` if set)
and `%LOCALAPPDATA%\ShadowOfTheColossusPC` on Windows. Saves are memory card
images in `player/memcards`. Use **My saves** to import or export an 8 MB
PS2/PCSX2 memory card (`.ps2`, 8,650,752 bytes) between computers. Close the
game before transferring it. Importing a card backs up the previous card in
`player/memcards/backups`. After importing, choose **Load Game** from the game’s
menu. **Open folder** uses the system’s default application and restores the
system library environment, including in the portable package. Your original
disc image stays intact.

## Launcher preferences and updates

Select **Full screen** in the footer or press **F11** to toggle launcher full
screen. The launcher remembers this preference. Tabs and the save and exit
shortcuts stay visible as the page scrolls. Use **Settings → Interface sounds
→ Apply** to change whether navigation and confirmation sounds play.

The launcher checks public GitHub releases on startup. On the **Home** tab,
the **Updates** panel appears below **Your game**. When a compatible version
is available, the panel displays **Update**. Select **Check now** to check again
and see whether the launcher is up to date.

**Update** downloads the package, verifies the SHA-256 published by GitHub,
updates the launcher directory, and opens the new version. The data directory
is separate. Install the first version with the updater from a release package.
If v0.4.2 reports HTTP 403 when checking GitHub, install v0.5.0 from the
[Releases page](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases).
Since v0.5.0 the launcher uses the manifest published with the packages to check
for new versions.

## Save states

On Linux, press **F5** to quick save and **F8** to quick load. In **F1 → States**,
choose the quick slot or one of five manual slots to save, load, or delete a
state. The manager displays timestamps and file sizes and asks for confirmation
before overwriting or deleting a slot. Files are stored in `player/savestates`.
The launcher displays the shortcuts and opens the folder from **My saves**.

Save states capture execution state, memory, audio, and native services. They
require the same runtime executable and disc edition. The loader rejects
states from other builds and incomplete files before touching the running game.
Writes are atomic. The initial implementation supports the launcher’s default
Linux native mode. If the game reports an unsupported save point, wait for the
video or loading operation to finish. Rendering caches are rebuilt on load.

Keep saving at shrines. The memory card is how you carry progress between
versions. Loading a state preserves the memory card file. If the card changed
since the state was captured, the loader rejects the state to protect persistent
saves.

## Full screen and shaders

<img width="1109" height="751" alt="Shadow of the Colossus PC graphics screenshot" src="https://github.com/user-attachments/assets/f4dfb9da-b702-46d1-b79a-d151fa18b36a" />

In **Graphics**, choose **Fullscreen · monitor resolution** to fill the current
monitor. **Widescreen 16:9** expands the original view horizontally. On an
ultrawide monitor the 16:9 frame is centered with side bars. **Original 4:3**,
**Stretch**, and **Pixels** are also available. Press **F11** to toggle full
screen and use **F1 → Video** to change the aspect ratio during play.

Under **Graphics → Graphics profile**, choose **Original**, **Balanced**,
**Enhanced**, or **Ultra**. Profiles adjust internal resolution, FXAA, texture
filtering, and the level of model and water detail. Use **F1 → Quality** to
change these options during play. [Graphics profile table](docs/SHADERS.md#perfis-gráficos)

Set the window size and **FSR 1** in **Graphics** or **F1 → Video**. Use
**F1 → Quality → Resolution** to set the internal render resolution. FSR 1
upscales the image from the internal resolution to the display area.

**F1 → Video → VSync** synchronizes frame presentation with the monitor to reduce
tearing. VSync is enabled by default and can be changed during play.

**Import shader…** copies a **GLSL 330** post-processing fragment shader (`.frag`,
`.fs`, or `.glsl`) and compiles it with your graphics driver. The launcher keeps
the previous selection if validation fails. **Use HD sharpening** enables the
bundled filter. **Disable** restores the original image. Changing shaders takes
effect on the next launch and reuses the existing game build.

The bundled template uses `texture0`, `fragTexCoord`, `fragColor`, `colDiffuse`,
`sourceSize`, `outputSize` (physical pixels), and `time` (seconds). The port
applies filters to the final game image before drawing the menu and FPS
counter. Texture and geometry resolution come from the game and its render
settings. ReShade `.fx` and RetroArch `.slangp` presets lack direct compatibility.
[Shader format and examples](docs/SHADERS.md)

## Texture mods

In **Textures**, choose the folder containing the texture pack’s PNGs. For an
archived pack, extract it first and choose the folder containing the images.
The selection takes effect the next time you start the game. **Disable pack**
restores the original textures and keeps the mod’s files.

To create a pack, enable **Export textures during gameplay**, visit the areas
you want to modify, and select **Open export folder**. Each supported native
texture is exported as a PNG with a 16-digit hexadecimal filename. Keep the
filename and aspect ratio when editing. The port accepts PNGs up to 4096 px
in either dimension, rebuilds mipmaps, and falls back to the original texture
if it cannot load the PNG. The port generates the filenames. Each session can
export up to 512 MiB of new images.

## Controls

In the launcher, use the D-pad or stick to move focus, **A/✕** to confirm, and
**B/○** to go back or close dialogs. **LB/RB** or **L1/R1** switch tabs.
**Start/Options** starts the prepared game, and **←/→** adjust the selected
control. In the file picker, **Y/△** opens the parent folder. Controllers
recognized by SDL2 use the bundled mappings and the profile saved through
**Configure my controller…**. The launcher shows shortcuts when a controller
is connected. Controller navigation works while the launcher window has focus.
The launcher releases the device while the game or calibration is open.

**Select/Back** or **F1** opens the launcher’s slide-out menu. Focus stays in
the menu until you choose a page or go back. Buttons grow when focused or
hovered, and their clickable areas stay the same size. **F11** toggles launcher
full screen. Arrow keys, Enter, and Esc also work with Steam Input profiles
that emulate the keyboard. The launcher detects newly connected controllers
and combines physical and virtual input to avoid duplicate confirmations. The
**Controls** tab shows detected devices and flags unmapped controllers.

Under **Settings → In-game button icons**, choose **Automatic**, **PlayStation**,
**Xbox**, or **Keyboard and mouse**. For new profiles, **Automatic** is the
default. Icons follow the last device used during play. An idle controller or
minor stick drift leaves the current selection in place. Keyboard and mouse
icons follow the bindings in **Controls**. Existing profiles retain the manual
choice until you select **Automatic**. Controllers exposed by Steam Input as
Xbox devices use Xbox icons, and you can still choose a mode manually.

This setting changes the game elements that use the controller icon atlas.
Tutorial captions with lettering baked into the original art retain their
original appearance. Mode changes take effect when you next start the game.

Under **Settings → Assists (cheats)**, **Infinite health** and **Infinite grip**
are independent options. Both are off by default and take effect on the next
launch. They prevent normal health or grip depletion. Wander’s maximum health
and grip and the game’s scripted events are retained. [Assist scope and tests](docs/ASSISTS.md)

Support for the **European PAL edition (SCES-53326)** is pending until a disc
can be recompiled and tested. The launcher recognizes the edition and reports
its status. [PAL support](docs/PAL-SUPPORT.md)

| Keyboard | PS2 control |
| --- | --- |
| WASD | Move |
| Space | Jump / mount · △ |
| Mouse | Move camera |
| Left / right / middle click | Attack · □ / grab · R1 / action · ○ |
| Q / E | Previous / next weapon |
| F / R | L1 / L2 |
| C / Left Shift | Call / spur Agro · ✕ / R2 |
| Esc / I | Start / Select |
| Enter / Backspace | Confirm / go back in menus |
| ↑ / ↓ | D-pad up / down |
| ← / → / Page Up / Page Down | Move camera with keyboard |
| Left Ctrl / V | L3 / R3 |
| F1 | Settings, release cursor, and **System → Return to launcher** |
| F5 / F8 | Quick save / quick load |
| F11 | Toggle windowed and full screen |

Under **Controls → Customize keyboard and mouse…**, rebind actions to a key
or mouse button. Adjust sensitivity, invert either axis, or disable mouse
camera controls. The profile is saved locally and applied the next time you
start the game. F1, F5, F8, F9 (diagnostics), and F11 are reserved. Delete
clears a binding in the editor. Opening the F1 menu or switching windows
releases the cursor. Release any controls held during the transition before
resuming play. Esc pauses the game.

USB and Bluetooth controllers are supported. The F1 menu includes video,
effects, sound, and language settings for the port’s interface. On Linux,
built-in mappings cover the Zikway HID gamepad / GameSir Nova Lite in HID mode
(USB `3537:1041`), including the D-pad, sticks, and triggers.

The runtime also bundles 743 Linux entries from the community
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB), with
GUIDs adapted for GLFW. The game and the F1 menu respond to the recognized
controller currently sending input, regardless of USB position. Reconnecting a
controller works without restarting the game. For an unmapped device, open
**Controls → Configure my controller…**, select the device, release the sticks,
and follow the 20 steps. The profile persists across launches and also works
in the F1 menu. Xbox/XInput, PlayStation, and generic controllers work when
Linux exposes them as joysticks. Mappings are independent of the device’s
marketing name or physical layout. Enter skips unavailable inputs, Backspace
repeats the previous step, and Esc cancels and keeps the existing profile.
Each device has its own profile. Developers can also specify mapping files
through `PS2X_GAMEPAD_MAPPINGS` or use `SDL_GAMECONTROLLERCONFIG`.

## Fidelity fixes under validation

The renderer captures additional HUD routines, including the attack charge
ring. The texture cache checks the entire animated palette to avoid reusing
stale colors or alpha values when an entry changes. Collisions use the translated
original code by default. Experimental rewrites are disabled so that the VU0
register state follows the original behavior.

Synthetic tests cover these fixes. The October 28 test record describes three
routes around the Cliff Path shrine using the supplied memory card and a fix
for stale graphics in the cache. See [campaign validation](#campaign-validation)
for fight status and the [October 28 test record](docs/FIDELITY-2026-10-28.md).

## Build and test

For development, install Docker and Python 3, then run the following commands.

```sh
python3 -m unittest discover -s tests/linux -v
tools/linux/build.sh
```

This build runs **without a ROM** and produces the launcher, runtime libraries,
and compilation kit. The packaging checks exclude disc images, PS2 executables,
XFF modules, generated game code, and saves. The Linux workflow runs these tests
and builds on every push and pull request. Gameplay is validated locally with a
user-supplied disc. CI runs without game files.

CI also tests OpenGL presentation on virtual 16:9 and ultrawide monitors. It
checks native resolution, side bars, stretching, filtered pixels, rejection of
invalid GLSL, disabling the filter, and restoration of the previous window size.

The original development pipeline is available in `./celosia`, with its stages
covered in the [upstream documentation](docs/UPSTREAM-CELOSIA.md). Select a
release package to use the graphical launcher.

## Credits and licenses

- The main upstream revision is **HenryKun55/celosia e0bc43d6d4eb8cc1a022c4b277f396be3ee4fbbe**,
  with selected native routines and optimizations from **c2514d9**. Manual game
  decompilation and native coverage of PS2 services and rendering are tracked
  independently. [Celosia integration notes](docs/CELOSIA-INTEGRATION-2026-11-17.md)
- Celosia and PS2Recomp are under **GPL-3.0**. Corresponding source for the port
  ships in `compiler/source`. [LICENSE](LICENSE)
- The Linux launcher and original vector illustration are by GenissonEmilio
  under the MIT license. The flow was adapted from the ForbiddenMemoriesPC
  and CrashBandicootPC launchers. [Launcher license](tools/desktop/LICENSE)
- [Tenkaichi3Decomp](https://github.com/stufno102/Tenkaichi3Decomp) was a reference
  for PS2 disc import and compilation. Its game code is specific to Dragon Ball.
- Compiler libraries keep their licenses and version information in
  `compiler/licenses` and `compiler/packages.txt` inside the release package.

Shadow of the Colossus belongs to Sony / Team Ico. This is an unofficial project.
Full upstream credits are available in the Celosia documentation.

[![Verified developer on RecompStore](https://recompstore.com/assets/press/verified-dark.svg)](https://recompstore.com/dev/genissonemilio)
[![Get it on RecompLauncher](https://recomplauncher.com/assets/press/launcher/badge-dark.svg)](https://recomplauncher.com/game/shadowofthecolossuspc)
