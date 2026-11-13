<!-- SPDX-FileCopyrightText: 2026 Flay Krunegan -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Contributing

Prisma is a C++20 project built with CMake and Ninja. Contributions should
preserve the launcher and runtime behavior across Windows and Linux.

## Build requirements

- CMake 3.20 or later and Ninja.
- Windows: LLVM-MinGW (UCRT).
- Linux: clang or g++, pkg-config, SDL2, and OpenGL development headers.

Build with `build.bat` on Windows or `./build.sh` on Linux. Both scripts
configure the local build directory on the first run and place binaries in
`build/dist/` or `build-linux/dist/`.

The `dist/` folder is what players get: every build copies the GLSL
library, the documentation and the licenses into it again. Do not edit
those copies: the next build replaces them. Packs are not part of the
build: Prisma reads them from `dist/shaderpacks/`, or from the folder set
with `packs = <folder>` under `[prisma]` in `prisma.ini`.

## Validation

Check a change with `prisma_check` and in the game, as below.

To check a single pack, or one outside the repository:

Windows:

```bat
build\dist\prisma_check.exe --internal
build\dist\prisma_check.exe path\to\MyPack
```

Linux:

```sh
build-linux/dist/prisma_check --internal
build-linux/dist/prisma_check path/to/MyPack
```

Run the checker for each pack whose shaders or metadata changed.
The checker uses the available OpenGL driver and tests stored values and
supported option variants. Compute passes are checked when the driver
supports OpenGL 4.3.

Changes to shaders in a pack's `game/` directory must also be checked in
Luanti. The offline checker cannot reproduce the engine-generated shader
headers or all material variants.

For changes to the launcher or injected runtime, verify startup, pack
selection, in-game menu controls, and clean shutdown in Luanti on the
affected operating system.

## Source layout

Project headers are included by their path from `src/`, for example
`#include "core/pack/pack.hpp"`.

| Path | Responsibility |
|---|---|
| `src/common/` | Code shared by the three programs: ini, TOML, logging, utilities, version, pack.toml rules |
| `src/launcher/` | Platform launchers, configuration, and desktop UI |
| `src/core/` | The injected runtime; `core.cpp` is its entry point |
| `src/core/platform/` | How the core gets into the game on Windows and Linux |
| `src/core/hooks/` | The game's intercepted OpenGL and SDL calls, and Prisma's own GL entry points |
| `src/core/tracking/` | What the core knows of the game's GL state: uniforms, programs, Luanti's shaders |
| `src/core/pack/` | Pack loading, options, pass headers, and the `prisma` uniform |
| `src/core/render/` | Pass pipeline, frame timing, Luanti shader changes, lighting, geometry |
| `src/core/ui/` | In-game menu, input, and debug view |
| `src/core/audio/` | Pack sounds |
| `src/check/` | `prisma_check`, the offline shader and pack validator |
| `glsl/prisma/` | Shared GLSL functions included by packs (`#include <prisma/...>`) |
| `glsl/internal/` | GLSL Prisma puts in front of pack code and its own lighting shaders; embedded at build time |
| `cmake/` | Build helpers (`embed_files.cmake`: files embedded into the binaries) |
| `third_party/` | Vendored dependencies |

## Change guidelines

- Keep pack metadata, runtime behavior, and the shader-pack reference in sync.
- Preserve public pack keys, GLSL identifiers, configuration keys, and file
  formats unless the change explicitly updates the API.
- Keep platform-specific changes buildable on both supported platforms.
- Include a concise description of the behavior change and the validation
  performed.

## Licensing of contributions

Each file states its license in an `SPDX-License-Identifier` header. By
opening a pull request, you agree that your contribution is licensed under
the license of each file it changes and, in addition, under
LGPL-2.1-or-later. Contributions to MIT files need nothing more, since MIT
code can already be used in LGPL projects. There is no contributor license
agreement: you keep the copyright of your contribution.

The additional LGPL-2.1-or-later license lets any part of Prisma be offered
to Luanti, which is licensed under LGPL-2.1-or-later and cannot include
GPL-3.0 code. The pull request template asks you to confirm this.
