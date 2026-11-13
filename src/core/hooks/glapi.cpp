// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/hooks/glapi.hpp"
#include "core/hooks/hooks.hpp"
#include "common/log.hpp"

namespace prisma::gl {

#define PRISMA_GL_DEFINE(ret, name, params) PFN_##name name = nullptr;
PRISMA_GL_FUNCS(PRISMA_GL_DEFINE)
PRISMA_GL_OPTIONAL_FUNCS(PRISMA_GL_DEFINE)
#undef PRISMA_GL_DEFINE

static bool g_loaded = false;

bool load()
{
	if (g_loaded)
		return true;
	bool ok = true;
#define PRISMA_GL_LOAD(ret, fn, params) \
	fn = reinterpret_cast<PFN_##fn>(hooks::raw_proc("gl" #fn)); \
	if (!fn) { \
		log::error("OpenGL function not available: gl" #fn); \
		ok = false; \
	}
	PRISMA_GL_FUNCS(PRISMA_GL_LOAD)
#undef PRISMA_GL_LOAD
#define PRISMA_GL_LOAD_OPTIONAL(ret, fn, params) fn = reinterpret_cast<PFN_##fn>(hooks::raw_proc("gl" #fn));
	PRISMA_GL_OPTIONAL_FUNCS(PRISMA_GL_LOAD_OPTIONAL)
#undef PRISMA_GL_LOAD_OPTIONAL
	if (ok && !has_mipmaps())
		log::info("The driver has no glGenerateMipmap: buffers with mipmaps = true will have no levels");
	g_loaded = ok;
	return ok;
}

bool loaded()
{
	return g_loaded;
}

bool has_mipmaps()
{
	return GenerateMipmap != nullptr;
}

bool has_compute()
{
	static int version = -1; // the context doesn't change once the game has it
	if (version < 0) {
		GLint major = 0, minor = 0;
		GetIntegerv(0x821B /* GL_MAJOR_VERSION */, &major);
		GetIntegerv(0x821C /* GL_MINOR_VERSION */, &minor);
		GetError(); // GL_MAJOR_VERSION is 3.0+: harmless before
		version = major * 10 + minor;
	}
	return version >= 43 && DispatchCompute && MemoryBarrier && BindImageTexture;
}

bool has_timer_queries()
{
	return GenQueries && DeleteQueries && QueryCounter && GetQueryObjectiv && GetQueryObjectui64v && FenceSync &&
		ClientWaitSync && DeleteSync;
}

} // namespace prisma::gl
