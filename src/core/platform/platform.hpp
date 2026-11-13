// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	What depends on the operating system in how Prisma gets into the game.

	Prisma doesn't patch the code of any library. It intercepts the symbols
	the game imports from them (SDL2 and OpenGL), which every system allows
	in its own way:

	  * Windows: the import table of luanti.exe. Its entries are filled in
	    when the process starts, before our DLL is loaded; we overwrite them.
	  * Linux: the preloaded library (LD_PRELOAD) exports the same symbols,
	    and the real ones come from dlsym(RTLD_NEXT).

	Only the game's own calls are intercepted, never those of SDL itself, of
	the driver or of Prisma (the menu, the passes), which call the real
	functions directly.
*/

#include <filesystem>

namespace prisma::platform {

// Libraries by role, whatever their file is called on each system
enum class Library {
	sdl, // SDL2.dll / libSDL2-2.0.so.0
	gl,  // opengl32.dll / libGL.so.1
};

// A symbol of `library` that the game imports, routed to `detour`.
// `original` receives the real function. Not `required`: the game may well
// not import it (it can also ask SDL for it).
struct Interpose {
	Library library;
	const char *symbol;
	void *detour;
	void **original;
	bool required = true;
};

// Routes the game's calls to each symbol of `list`. False if a required one
// is missing (the others are applied all the same).
bool interpose(Interpose *list, int count);

template <int N>
bool interpose(Interpose (&list)[N])
{
	return interpose(list, N);
}

// The real function `symbol` of `library` (already loaded by the game).
void *library_symbol(Library library, const char *symbol);

// Luanti's data: the folder with client/shaders and fonts. On Windows the
// parent of bin/; on Linux that too for a "run in place" build, otherwise
// where the system package installs it (/usr/share/luanti...).
std::filesystem::path game_data_dir();

unsigned long process_id();

// Tells the launcher whether Prisma got in (it waits for this).
void report_ready(bool ok);

// Shows `folder` in the system's file manager.
void open_folder(const std::filesystem::path &folder);

} // namespace prisma::platform
