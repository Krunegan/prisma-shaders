<!-- SPDX-FileCopyrightText: 2026 Flay Krunegan -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Changelog

Notable changes to Prisma. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the program
follows [Semantic Versioning](https://semver.org/). The pack API has its own
version, `api` in `pack.toml`.

## [1.0.0] — 2026-11-13

The first release.

### Added

- A launcher for Windows and Linux: chooses Luanti and the pack, and starts
  the game with Prisma in it, without changing Luanti's installation. The
  Luanti settings a pack asks for are applied through a copy of the
  player's settings file.
- "Download packs" in the launcher: the packs published in `prisma-packs`,
  eight to a page with their screenshots, installed, updated and removed
  there. Every file is checked against the size and SHA-256 the
  repository lists.
- An in-game menu (Home) with the pack's options by group, the choice of
  pack and keys, the errors, and a debugging page with pass timings and a
  texture viewer. End turns the pack on or off.
- Pack API 1 (`docs/shaderpacks.md`): fragment and compute passes, buffers
  and pass chains, options, changes to Luanti's own shaders, a shadow map,
  a G-buffer, an overhead map, a pack's own light under a hidden sky,
  sounds (Ogg Vorbis and WAV) and screenshots.
- `prisma_check`: compiles a pack and its option variants without the game,
  writes the API declarations for an editor (`--stub`), and compares a
  Luanti installation with the supported version (`--luanti`).
- A GLSL library for packs, under the MIT licence.
- Developer mode for pack authors: reload on save, the shaders as Prisma
  changed them, and a trace of one frame.
- Support for Luanti 5.17.0: the launcher warns about another version, and
  the game writes a compatibility report to `prisma.log`.

### Security

- A pack can change only Luanti's rendering settings, never its security,
  network or account settings.
- Every file a pack refers to must be inside its folder.
- Pack audio is limited to Ogg Vorbis and WAV.
