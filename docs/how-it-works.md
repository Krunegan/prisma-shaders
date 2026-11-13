<!-- SPDX-FileCopyrightText: 2026 Flay Krunegan -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# How Prisma works

Prisma adds shader packs to Luanti from outside the game. It does not change
Luanti's installation or files: it starts the game itself and runs inside
its process. This document describes the parts of Prisma and what happens
from the launch to each frame. How to use Prisma is in
[usage.md](usage.md); how to write a pack is in
[shaderpacks.md](shaderpacks.md).

## The parts

| Part | Files | Role |
|---|---|---|
| Launcher | `prisma.exe` / `prisma` | The window: chooses Luanti, the pack and its settings, downloads packs, and starts the game with the core in it. |
| Core | `prisma_core.dll` / `libprisma_core.so` | Runs inside Luanti. Intercepts the OpenGL and SDL2 functions the game uses and draws the pack. |
| Checker | `prisma_check.exe` / `prisma_check` | Compiles packs without the game, and compares a Luanti installation with the supported version. |
| GLSL library | `glsl/prisma/` | Functions packs include: colour, noise, bloom, Luanti's formulas. |
| Packs | `shaderpacks/<Pack>/` | Not part of Prisma. Each is a folder with `pack.toml` and its shaders. |

## Starting the game

1. The launcher reads `prisma.ini` and the chosen pack's `pack.toml`.
2. If the pack sets some of Luanti's rendering settings (its `[luanti]`
   table), the launcher writes `luanti_prisma.conf`: a copy of the player's
   `minetest.conf` with those settings on top. Luanti is started with
   `--config` pointing at it. When the game closes, whatever else the player
   changed during the session (keys, volume) is copied back to
   `minetest.conf`; the pack's settings are not. Playing without Prisma,
   Luanti keeps its own settings.
3. The launcher starts Luanti with the core in it:
   - **Windows**: Luanti is started suspended, `prisma_core.dll` is loaded
     into it, and only then does the game run.
   - **Linux**: Luanti is started with `LD_PRELOAD` naming
     `libprisma_core.so`.
4. The core starts before the game creates its window. It opens
   `logs/prisma.log`, reads `prisma.ini`, loads the pack, and puts its own
   functions in place of the OpenGL and SDL2 functions the game calls.

## Each frame

The core follows the game's OpenGL calls without changing what the game
draws, except where the pack asks for it:

- **What the game sends to its shaders** (camera, time of day, fog, sun) is
  recorded as the game sets it. The pack receives it in the `prisma`
  uniform.
- **Luanti's shaders.** When Luanti compiles one of its shaders, the core
  can extend, patch or replace it with the pack's `game/<shader>/` files.
- **Prisma's own maps**, when the pack declares them: a shadow map drawn
  from the sun or the moon, a G-buffer with the surfaces' normals, light and
  material, and an overhead map of the terrain's height. They are drawn
  again from the geometry the game sent this frame.
- **The passes.** After Luanti draws the world and before its own
  post-processing, the core runs the pack's passes in order: full-screen
  fragment shaders, or compute shaders with OpenGL 4.3. Each reads the scene,
  its depth and the pack's buffers, and writes the scene or a buffer.
- **Sound.** A pack can play sounds; its shaders set their volume through a
  buffer the core reads.
- **The menu.** The core draws Prisma's menu over the game (Home key) and
  takes the keyboard and mouse while it is open.

The core does nothing it does not need: maps a pack does not use are not
drawn, and with the pack switched off (End key) the game draws as it would
without Prisma.

## When something fails

Prisma fails safe: when a pack, a shader or a hook fails, the game keeps
running as it would without Prisma.

- A pack that cannot be loaded is not used, and the error is written to the
  log and shown on the menu's Errors page.
- When a shader does not compile after a change, Prisma keeps the last
  version that did.
- Paths in a pack must stay inside its folder, and `[luanti]` accepts only
  rendering settings: a pack cannot change Luanti's security, network or
  account settings.

## Supported Luanti version

Prisma supports one Luanti version: 5.17.0. It relies on details of that
version, such as the names of its shaders and of the values they receive.
These details are embedded in the programs, and three checks compare them
with the Luanti in use:

- The launcher warns when the chosen Luanti is another version, and starts
  it anyway.
- In the game, the core writes a compatibility line to `prisma.log` once the
  world is shown, and reports what it relies on and did not find as an error.
- `prisma_check --luanti <folder>` compares an installation with 5.17.0 and
  lists the differences.

With another version, a pack can look wrong or not apply at all.

## Downloading packs

The launcher's "Download packs" page reads the repository
[prisma-packs](https://github.com/Krunegan/prisma-packs) on GitHub, where
packs are reviewed before they are published.

1. It reads the latest commit of the `main` branch, then `index.toml` and
   the files of that one commit, so a download never mixes two versions.
2. Every file must have the size and SHA-256 that the index lists, stay
   inside the pack's folder, and be pack data: `pack.toml`, GLSL, audio,
   text, or PNG screenshots. A pack with any other file is refused.
3. A pack is downloaded to a hidden folder first and moved into the packs
   folder only when all of its files have passed.

Only the launcher uses the network. The core and the checker never do.

## Where files are

Everything Prisma writes is next to its programs:

| Path | Contents |
|---|---|
| `prisma.ini` | The settings ([usage.md](usage.md)). |
| `shaderpacks/` | The installed packs (another folder can be chosen). |
| `glsl/` | The GLSL library. |
| `logs/` | `prisma.log` (the game), `launcher.log` (the launcher). |
| `luanti_prisma.conf` | The copy of Luanti's settings, while a pack sets some of them. |

## Source code

| Path | Contents |
|---|---|
| `src/launcher/` | The launcher: window, settings, downloads, starting the game |
| `src/core/` | The core: `core.cpp` is where it starts |
| `src/core/platform/` | How the core gets into the game on Windows and Linux |
| `src/core/hooks/` | The game's intercepted OpenGL and SDL2 calls |
| `src/core/tracking/` | What the core knows of the game's state: uniforms, shaders, programs |
| `src/core/pack/` | Loading packs, their options, and the `prisma` uniform |
| `src/core/render/` | The passes, Luanti's shader changes, the shadow map, G-buffer and overhead map |
| `src/core/ui/` | The in-game menu |
| `src/core/audio/` | Pack sounds |
| `src/check/` | `prisma_check` |
| `src/common/` | Code the three programs share |
| `glsl/prisma/` | The GLSL library for packs |
| `glsl/internal/` | GLSL the core adds to pack code, and its own shaders |

How to build Prisma and contribute is in
[CONTRIBUTING.md](../CONTRIBUTING.md).
