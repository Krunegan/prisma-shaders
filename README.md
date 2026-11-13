<!-- SPDX-FileCopyrightText: 2026 Flay Krunegan -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Prisma Shaders for Luanti

Prisma is an external shader-pack loader for Luanti. It provides a launcher, an
in-game OpenGL integration layer, and a GLSL API for pack authors. Prisma does
not modify Luanti's installation.

Prisma supports Luanti 5.17.0 only. With another version the launcher warns
and the game may look wrong ([how Prisma works](docs/how-it-works.md)).

## Components

- `src/launcher/` contains the Windows and Linux launchers and configuration UI.
- `src/core/` contains the OpenGL hooks, shader pipeline, pack loader, and runtime.
- `src/check/` implements `prisma_check`, which compiles pack shaders and
  compares a Luanti installation with the supported version (`--luanti`).
- `src/common/` holds the code the three programs share.
- Prisma ships no packs: the launcher downloads them from the repository
  `prisma-packs`, which also holds the author's own, Vesper and Aurora.
- `glsl/prisma/` contains the shared GLSL library.
- [Using Prisma](docs/usage.md): installing, packs, settings, command line.
- [How Prisma works](docs/how-it-works.md): the launcher, the core, and
  what happens in each frame.
- [Shader pack development](docs/shaderpacks.md) documents the pack format and API.
- [Contributing](CONTRIBUTING.md) documents builds, validation, and code layout.

## Build

Requirements:

- CMake 3.20 or later and Ninja.
- Windows: LLVM-MinGW (UCRT).
- Linux: clang or g++, pkg-config, SDL2, and OpenGL development headers.

Build from the repository root:

```bat
build.bat
```

```sh
./build.sh
```

Binaries are written to `build/dist/` on Windows and `build-linux/dist/` on
Linux.

## Validate

Check a single pack on Windows:

```bat
build\dist\prisma_check.exe --internal
build\dist\prisma_check.exe path\to\MyPack
```

On Linux, use `build-linux` instead of `build`:

```sh
build-linux/dist/prisma_check --internal
build-linux/dist/prisma_check path/to/MyPack
```

Run the checker for each changed pack. It compiles passes
and supported option variants with the active OpenGL driver. Changes under a
pack's `game/` directory require an in-game check because they depend on
Luanti-generated shader source.

See [Contributing](CONTRIBUTING.md) for the full validation procedure.

## License

- The program (`src/`, the build files and scripts) is licensed under
  GPL-3.0-or-later; see `LICENSE`.
- The shared GLSL library (`glsl/`) is licensed under MIT, so packs that use
  it can have any license.
- The documentation (`docs/` and these Markdown files) is licensed under
  CC-BY-4.0.

Every file states its license in an SPDX header, or in `REUSE.toml` for files
that cannot carry one. The license texts are in `LICENSES/`, and the licenses
of the bundled third-party code are in `packaging/THIRD_PARTY_NOTICES.txt`.
See [Contributing](CONTRIBUTING.md) for how contributions are licensed.
