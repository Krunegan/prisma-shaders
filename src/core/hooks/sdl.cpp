// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/hooks/sdl.hpp"
#include "core/platform/platform.hpp"

namespace prisma::sdl {

using PFN_Size = void(SDLCALL *)(void *, int *, int *);

static bool size(PFN_Size &fn, const char *symbol, void *window, int &w, int &h)
{
	if (!fn)
		fn = reinterpret_cast<PFN_Size>(platform::library_symbol(platform::Library::sdl, symbol));
	w = h = 0;
	if (!fn || !window)
		return false;
	fn(window, &w, &h);
	return w > 0 && h > 0;
}

bool window_size(void *window, int &w, int &h)
{
	static PFN_Size fn = nullptr;
	return size(fn, "SDL_GetWindowSize", window, w, h);
}

bool drawable_size(void *window, int &w, int &h)
{
	static PFN_Size fn = nullptr;
	return size(fn, "SDL_GL_GetDrawableSize", window, w, h);
}

} // namespace prisma::sdl
