PRISMA 1.0.0 — Linux
====================

Shader packs for Luanti.

Requirements
------------
* 64-bit Linux with glibc 2.34 or later.
* Luanti 5.17 built with SDL2. Flatpak and Snap builds are not supported.
* OpenGL 3.2 or later. Compute passes require OpenGL 4.3.

Installation
------------
Extract the archive and launch Prisma:

  ./prisma

The launcher searches PATH for luanti or minetest. Use --configure to select
the executable explicitly. Prisma includes no packs: install one on
"Download packs", select it and choose Play.

In-game controls
----------------
* Home: open or close the Prisma menu.
* End: enable or disable the active pack.

Documentation
-------------
docs/usage.md          using Prisma: packs, settings (prisma.ini), command line
docs/how-it-works.md   how Prisma works
docs/shaderpacks.md    writing shader packs

Logs are written under logs/ beside prisma. See prisma.log for pack and shader
errors.
