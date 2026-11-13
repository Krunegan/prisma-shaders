// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	The few things of SDL2 that Prisma uses directly (the game's window). Its
	functions are looked up at runtime: we don't link against SDL2.
*/

#ifdef _WIN32
#define SDLCALL __cdecl
#else
#define SDLCALL
#endif

namespace prisma::sdl {

// Size of the window in screen points (where the mouse events are) and in
// pixels (its framebuffer). They only differ on high-density screens.
bool window_size(void *window, int &w, int &h);
bool drawable_size(void *window, int &w, int &h);

} // namespace prisma::sdl
