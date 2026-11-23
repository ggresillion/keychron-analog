# keychron-analog

Analog movement in **Cyberpunk 2077** with a **Keychron K2 HE** (hall-effect)
keyboard, on **stock Keychron firmware**, including on **Linux / Proton**.

This repo contains a small DLL that stands in for the Wooting Analog SDK and
reads key depth straight from the Keychron keyboard. It is meant to be used
with [AnalogSense](https://github.com/AnalogSense/AnalogSense.asi), which does
the in-game part.

> Not affiliated with Keychron, Wooting, AnalogSense or CD Projekt Red.

## Why this approach

- **No gamepad emulation.** AnalogSense keeps W/A/S/D as normal keyboard keys
  and replaces the value each key feeds into the movement axes with its depth.
  The game stays in keyboard mode: keyboard prompts, taps, double-tap dodge and
  crouch all behave like vanilla, and direction stays analog (W + a bit of D =
  a slight angle).
- Approaches that feed a virtual controller stick (keyboard "gamepad mode",
  XInput emulation) make Cyberpunk flip prompts and, while the stick is held,
  lose keyboard taps and releases (dodge and crouch break while moving).
- **No Wooting SDK or plugin install, no custom firmware.** The shim talks to
  the keyboard's raw HID interface (usage page `0xFF60`) with the command
  Keychron Launcher uses for its live key-travel view
  (`0xA9 0x30 row col`, `AMC_GET_REALTIME_TRAVEL`).

## Tested with

- Keychron K2 HE **ANSI**, firmware 1.3.0, **wired** (USB cable)
- Cyberpunk 2077 2.5, AnalogSense 0.4.1, Ultimate ASI Loader 10.0.2
- Linux, Proton-CachyOS 11 (Wayland and X11)

Other Keychron HE models / layouts probably use different matrix positions or
product IDs: see `[Keys]` and `ProductIds` in `config/KeychronAnalog.ini`.
The stock firmware does not forward raw HID over the 2.4 GHz receiver, so the
keyboard must be on the cable.

## Install

Into `Cyberpunk 2077/bin/x64/`:

| File | From |
|---|---|
| `winhttp.dll` | [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) x64 (`dinput8.dll` renamed to `winhttp.dll`) |
| `plugins/AnalogSense.asi` | [AnalogSense releases](https://github.com/AnalogSense/AnalogSense.asi/releases) |
| `wooting_analog_sdk.dll` | this repo (see Build, or the GitHub release) |
| `KeychronAnalog.ini` | `config/KeychronAnalog.ini` (optional) |

`winhttp.dll` rather than the usual `version.dll`: the game's crash reporter
(`REDEngineErrorReporter.exe`) also loads `version.dll` and AnalogSense would
show "This game is not supported" there.

AnalogSense overwrites the value of every key bound to a movement or driving
axis. Keys the shim does not poll (arrows, Ctrl, ...), and every key when the
keyboard is not found, report their normal on/off state, so they keep working.

### Linux / Proton launch options

```
SDL_GAMECONTROLLER_IGNORE_DEVICES= PROTON_ENABLE_HIDRAW=0x3434/0x0E20 WINEDLLOVERRIDES="winmm,winhttp=n,b" %command%
```

- `PROTON_ENABLE_HIDRAW` exposes the keyboard's raw HID interface to the game.
- `SDL_GAMECONTROLLER_IGNORE_DEVICES=` (empty): Steam may list the K2 HE as a
  controller it handles (the keyboard has a gamepad interface), which makes
  Proton hide the whole device.
- `winhttp=n,b` loads the ASI loader instead of Wine's built-in winhttp.
- Avoid `WAYLANDDRV_FOCUS_LOSS=0`: the game then always believes it has focus,
  so keys pressed in other windows would still move the character.

On Windows only the DLL placement applies.

## Keyboard settings

The game only starts tracking a key once the keyboard sends its normal
keypress, so **the actuation point is the movement deadzone**. In Keychron
Launcher, for W/A/S/D (ideally in a separate gaming profile):

- actuation point at the minimum (0.2 mm)
- Rapid Trigger off (it releases the key while it is still half pressed)

## Tuning

AnalogSense's response curve lives in
`%ProgramData%\AnalogSense\curve_points.json` (under Proton:
`steamapps/compatdata/1091500/pfx/drive_c/ProgramData/AnalogSense/`). It is
read at game start. A small boost helps light presses get the character
moving, for example:

```json
[{"x": 0.04, "y": 0.3}]
```

(4 % travel -> 30 % output, then linear to 100 %.)

Shim settings: `KeychronAnalog.ini` next to the DLL. Its log,
`KeychronAnalog.log`, lists the HID interfaces it sees and why it did or did
not open one.

## Build

MinGW-w64 cross compiler:

```
make            # build/wooting_analog_sdk.dll, build/test_shim.exe
```

On NixOS:

```
nix shell nixpkgs#pkgsCross.mingwW64.stdenv.cc -c make
```

(If the linker cannot find `-lmcfgthread`, add
`LDFLAGS=-L<mcfgthread store path>/lib`.)

`test_shim.exe` loads `wooting_analog_sdk.dll` from its folder and prints key
values, which is handy to check device access under Proton, e.g.
`PROTON_ENABLE_HIDRAW=0x3434/0x0E20 umu-run ./test_shim.exe 15`.

## License

MIT for the code in this repo. AnalogSense and Ultimate ASI Loader are separate
projects with their own terms and are not included.
