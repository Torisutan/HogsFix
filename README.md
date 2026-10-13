# HogsFix

Fix for **Hogs of War** (PC, 2000; German: *Frontschweine*) on Windows 10/11. One ASI plugin, loaded by
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). Everything is patched in memory at
startup; no game file or registry entry is changed.

## Fixes

- Crash / black screen after start (DirectDraw surface loss, Bink crash), "Terminating" message
- Alt-Tab, Windows key and minimize work (GOG's `IgnoreAltTab` shim removed for the game process)
- CPU pigs never move (inverted bounds check in the AI path request)
- Flickering / missing terrain on 32-bit desktops (NVIEW culling tables)
- Widescreen up to 2048 px wide, Hor+ field of view in battle
- Battle at 60 FPS with interpolation (original 25), idle wait instead of 100 % CPU
- Fast menus (were 10–15 FPS), menu animations at their original 25 FPS
- CD music from `MUSIC\TrackNN.ogg` (built-in decoder, no extra DLLs)
- Gamepad: XInput controllers with the PlayStation layout, hot-plug
- Windowed mode (resizable), no CD check, portable install folder
- Title screen text, 32-bpp sprites, windows of other programs no longer disabled

Every fix can be turned off in `HogsFix.ini`.

## Supported versions

- GOG `warhogs_.exe` (unmodified)
- No-CD `Warhogs.exe` of the September 2000 retail build (timestamp `0x39C903E8`, 964,608 bytes)

Other executables are left untouched (see `HogsFix.log` with `Log=1`).

## Install

1. Download `HogsFix-<version>.zip` from [Releases](../../releases).
2. Extract `dinput.dll`, `HogsFix.asi` and `HogsFix.ini` into the game folder (next to `warhogs_.exe`).
3. Start the game as usual.

Using the no-CD `Warhogs.exe`: delete or rename GOG's `winmm.dll` in the game folder.
Details and the controller layout: [dist/README.txt](dist/README.txt) (German/English).

## Gamepad layout

| Xbox | PlayStation | Action |
|---|---|---|
| D-pad / left stick | D-pad | move, menus, aim |
| A | ✕ | select / fire (hold for power) |
| B | ○ | weapon menu / back |
| X | □ | jump |
| Y | △ | vehicles, deselect weapon, back |
| LB / RB | L1 / R1 | camera, precise aim / trajectory up |
| LT / RT | L2 / R2 | — / trajectory down |
| Right stick | — | trajectory |
| Start | Start | pause |

In battle the pad presses the keys set under *Optionen → Steuerung*, so rebinding applies to the pad as well.

## Build

Requires Visual Studio 2019 or newer with the C++ desktop workload (MSVC x86). No other dependencies.

```
build.bat                       -> build\HogsFix.asi
powershell -File package.ps1    -> build\HogsFix-0.1.zip (downloads Ultimate ASI Loader v9.7.4, checks its hash)
```

The plugin is built without the C runtime (`/NODEFAULTLIB`); avoid 64-bit division and CRT calls in the sources.

## Source layout

| File | Content |
|---|---|
| `src/hogsfix.c` | loader, config, per-exe address tables, byte-verified patches, NVIEW loader |
| `src/battle.c` | battle loop pacing and interpolation |
| `src/display.c` | native DirectDraw / Direct3D 7 fixes, windowed mode, menu blit batching |
| `src/cdaudio.c` | MCI cdaudio and mixer emulation, OGG playback |
| `src/gamepad.c` | XInput input mapping |
| `src/stb_vorbis.c` | Ogg Vorbis decoder (Sean Barrett, public domain) |

Patches are listed with their addresses in the comments at the top of each file. Each patch checks the original
bytes first and is skipped if they differ.

## License

HogsFix: MIT, see [LICENSE](LICENSE). Ultimate ASI Loader (shipped as `dinput.dll`): MIT, ThirteenAG.
stb_vorbis: public domain / MIT. Hogs of War is © Infogrames / Eutechnyx; no game files are included.
