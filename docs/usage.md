<!-- SPDX-FileCopyrightText: 2026 Flay Krunegan -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Using Prisma

This guide covers installing Prisma, getting packs, playing with them, and
the settings file. How Prisma works inside is in
[how-it-works.md](how-it-works.md).

## Requirements

| | Windows | Linux |
|---|---|---|
| System | 64-bit Windows 10 or later | 64-bit Linux with glibc 2.34 or later |
| Luanti | 5.17.0, the official 64-bit build | 5.17.0 built with SDL2, run in place or installed; Flatpak and Snap builds are not supported |
| Graphics | OpenGL 3.2 or later; compute passes need OpenGL 4.3 | the same |

Prisma supports Luanti 5.17.0 only. With another version the launcher
warns, and packs may look wrong.

## Installing

1. Extract the archive to a folder you can write to. Prisma keeps its
   settings, packs and logs in that folder.
2. Open `prisma.exe` (Windows) or `./prisma` (Linux).
3. On **Settings**, choose Luanti: on Windows, `luanti.exe` in Luanti's
   `bin` folder; on Linux, the launcher looks for `luanti` or `minetest` in
   `PATH`, and **Browse…** chooses another one.

Prisma does not change Luanti's installation. To remove Prisma, delete its
folder.

## Getting packs

Prisma comes with no packs. The launcher's **Download packs** page lists the
packs published in [prisma-packs](https://github.com/Krunegan/prisma-packs),
eight to a page, each with its screenshots.

- **Install** downloads a pack into the packs folder.
- **Update** appears when a newer version is published.
- **Remove** deletes a pack that was downloaded there.
- **Details** shows the pack's description and all of its screenshots.

Packs can also be copied by hand into the packs folder (`shaderpacks/`
next to Prisma, unless another one is chosen on **Settings**). The launcher
updates and removes only the packs it downloaded.

## Playing

Choose a pack in the list on the left and press **Play**. The pack page
shows the Luanti settings the pack sets, if any; **Apply the Luanti
settings the pack asks for** can be turned off there. Luanti then opens with
the player's own settings, and the pack may look different or run slower.

In the game:

| Key | Action |
|---|---|
| Home | Open or close Prisma's menu |
| End | Turn the pack on or off, to compare with Luanti's own look |

Both keys can be changed on **Settings**. The menu shows the pack's options
by group; changes apply at once and are saved for the next game. **Pack and
keys** chooses another pack, and **Errors** lists what went wrong.

With **Show this window** turned off on **Settings**, Prisma starts the
game directly. To see the window again, open Prisma with `--configure`, or
on Windows hold Shift while opening it.

## Settings file

`prisma.ini`, next to the programs, holds the settings. The launcher and the
menu write it; it can also be edited by hand while Prisma is closed.

```ini
[prisma]
luanti=                 ; the Luanti executable
arguments=              ; extra arguments for Luanti, e.g. --go --worldname world1
pack=                   ; the pack in use (its folder name); empty or not installed: the first one installed
packs=                  ; the packs folder (default: shaderpacks next to Prisma)
apply_luanti_settings=  ; 0: do not apply the Luanti settings a pack sets
show_window=            ; 0: start the game without the window
modern_context=         ; 0: keep the OpenGL context Luanti asks for (no compute passes)

[keys]
menu=                   ; the key that opens the menu (default: Home)
toggle=                 ; the key that turns the pack on or off (default: End)

[audio]
volume=                 ; volume of pack sounds, 0 to 4 (0: no sound)

[options:<Pack>]        ; each pack's option values, written by the menu

[debug]                 ; tools for pack authors
```

Of `[debug]`, only `developer_mode=1` is described and kept from one
version to the next ([shaderpacks.md](shaderpacks.md)); its other keys may
change.

## Command line

| Command | Effect |
|---|---|
| `prisma --configure` | Open the window, even with `show_window=0` |
| `prisma <path to Luanti>` | Start the game at once with that Luanti, and remember it |
| `prisma -- <arguments>` | Start the game at once; the arguments after `--` go to Luanti |
| `prisma_check <pack>` | Compile a pack (its folder name, or a path) |
| `prisma_check --internal` | Compile Prisma's own shaders |
| `prisma_check --stub <pack>` | Write `prisma_api.glsl`, the API declarations, for an editor |
| `prisma_check --luanti <folder>` | Compare a Luanti installation with 5.17.0 |

On Windows the programs are `prisma.exe` and `prisma_check.exe`.

## When something goes wrong

- **The pack does not show.** Open the menu (Home) and look at **Errors**,
  then at `logs/prisma.log`.
- **The launcher warns about the Luanti version.** Prisma supports 5.17.0
  only; another version may work partly or not at all.
- **Download packs cannot reach the repository.** Check the internet
  connection and press **Check again**. The launcher writes to
  `logs/launcher.log`.
- **The game does not start with Prisma on Windows.** Some antivirus
  programs block loading `prisma_core.dll` into another program; the
  launcher says so.
