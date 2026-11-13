// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	Linux: libprisma_core.so is preloaded into Luanti (LD_PRELOAD, set by the
	launcher). It exports the SDL and GL symbols Prisma intercepts, so the
	game's calls land here first; each one goes on to Prisma's detour once
	hooks.cpp has installed it, or straight to the real function (found with
	dlsym(RTLD_NEXT)) otherwise.

	Prisma starts on the first of those calls, not in a library constructor:
	by then every static object of the core is built (constructors of a
	shared library run in no guaranteed order), and a process that never
	calls them (the /bin/sh wrapper some distributions put before Luanti,
	which inherits LD_PRELOAD) never starts it.
*/
#include "core/platform/platform.hpp"
#include "core/core.hpp"
#include "common/log.hpp"
#include "common/util.hpp"

#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

extern char **environ;

namespace prisma::platform {

namespace fs = std::filesystem;

static bool is_game_process()
{
	std::string name = to_utf8(fs::read_symlink("/proc/self/exe").filename());
	return name.starts_with("luanti") || name.starts_with("minetest");
}

static void start_once()
{
	static std::once_flag once;
	std::call_once(once, [] {
		if (is_game_process())
			start();
	});
}

// ---------------------------------------------------------------- the exported symbols

// The detour of each symbol (null: not installed) and the real function
struct Slot {
	const char *symbol;
	void *detour;
	void *real;
};

#define PRISMA_LINUX_SYMBOLS(X) \
	X(void *, SDL_GL_GetProcAddress, (const char *a), (a)) \
	X(void, SDL_GL_SwapWindow, (void *a), (a)) \
	X(int, SDL_GL_SetAttribute, (int a, int b), (a, b)) \
	X(void *, SDL_GL_CreateContext, (void *a), (a)) \
	X(int, SDL_PollEvent, (void *a), (a)) \
	X(int, SDL_SetRelativeMouseMode, (int a), (a)) \
	X(int, SDL_ShowCursor, (int a), (a)) \
	X(void, SDL_WarpMouseInWindow, (void *a, int b, int c), (a, b, c)) \
	X(void, glDrawElements, (unsigned a, int b, unsigned c, const void *d), (a, b, c, d)) \
	X(void, glDrawArrays, (unsigned a, int b, int c), (a, b, c)) \
	X(void, glClear, (unsigned a), (a)) \
	X(void, glViewport, (int a, int b, int c, int d), (a, b, c, d)) \
	X(void, glBindTexture, (unsigned a, unsigned b), (a, b)) \
	X(void, glTexImage2D, (unsigned a, int b, int c, int d, int e, int f, unsigned g, unsigned h, const void *i), \
		(a, b, c, d, e, f, g, h, i)) \
	X(void, glDeleteTextures, (int a, const unsigned *b), (a, b)) \
	X(void, glEnable, (unsigned a), (a)) \
	X(void, glDisable, (unsigned a), (a)) \
	X(void, glCullFace, (unsigned a), (a)) \
	X(void, glFrontFace, (unsigned a), (a)) \
	X(void, glDrawBuffer, (unsigned a), (a)) \
	X(void, glReadBuffer, (unsigned a), (a))

#define PRISMA_SLOT(ret, name, params, args) {#name, nullptr, nullptr},
static Slot g_slots[] = {PRISMA_LINUX_SYMBOLS(PRISMA_SLOT)};
#undef PRISMA_SLOT

enum {
#define PRISMA_INDEX(ret, name, params, args) k_##name,
	PRISMA_LINUX_SYMBOLS(PRISMA_INDEX)
#undef PRISMA_INDEX
};

static void *real(int index)
{
	Slot &s = g_slots[index];
	if (!s.real)
		s.real = dlsym(RTLD_NEXT, s.symbol);
	return s.real;
}

} // namespace prisma::platform

// What the game calls. A detour that calls its own symbol again (SDL may
// call its exported functions from inside) gets the real one.
#define PRISMA_THUNK(ret, name, params, args) \
	extern "C" __attribute__((visibility("default"))) ret name params \
	{ \
		using namespace prisma::platform; \
		using F = ret(*) params; \
		start_once(); \
		static thread_local bool inside = false; \
		void *target = g_slots[k_##name].detour; \
		if (target && !inside) { \
			struct Guard { \
				bool &flag; \
				~Guard() { flag = false; } \
			} guard{inside}; \
			inside = true; \
			return reinterpret_cast<F>(target) args; \
		} \
		return reinterpret_cast<F>(real(k_##name)) args; \
	}
PRISMA_LINUX_SYMBOLS(PRISMA_THUNK)
#undef PRISMA_THUNK

namespace prisma::platform {

bool interpose(Interpose *list, int count)
{
	bool ok = true;
	for (int i = 0; i < count; i++) {
		Interpose &e = list[i];
		int found = -1;
		for (int k = 0; k < (int)std::size(g_slots); k++)
			if (std::strcmp(g_slots[k].symbol, e.symbol) == 0)
				found = k;
		void *r = found >= 0 ? real(found) : nullptr;
		if (!r) {
			if (e.required) {
				log::error("Can't hook %s: %s", e.symbol,
					found < 0 ? "missing in platform_linux.cpp" : "the game does not have it");
				ok = false;
			}
			continue;
		}
		*e.original = r;
		g_slots[found].detour = e.detour;
	}
	return ok;
}

void *library_symbol(Library, const char *symbol)
{
	// The next one after us: the library's own, never our export
	return dlsym(RTLD_NEXT, symbol);
}

fs::path game_data_dir()
{
	std::error_code ec;
	auto has_shaders = [&](const fs::path &dir) { return fs::is_directory(dir / "client" / "shaders", ec); };
	// A "run in place" build: <game>/bin/luanti
	fs::path beside = host_exe_dir().parent_path();
	if (has_shaders(beside))
		return beside;
	for (const char *dir : {"/usr/share/luanti", "/usr/local/share/luanti", "/usr/share/games/luanti", "/app/share/luanti",
			 "/usr/share/minetest", "/usr/local/share/minetest", "/usr/share/games/minetest", "/app/share/minetest"})
		if (has_shaders(dir))
			return dir;
	return beside;
}

unsigned long process_id()
{
	return (unsigned long)getpid();
}

// The launcher passes the end of a pipe in PRISMA_READY_FD and waits for a byte
void report_ready(bool ok)
{
	if (const char *fd_text = std::getenv("PRISMA_READY_FD")) {
		int fd = std::atoi(fd_text);
		char c = ok ? '1' : '0';
		if (fd > 2 && write(fd, &c, 1) == 1)
			close(fd);
		unsetenv("PRISMA_READY_FD");
	}
	// What Luanti starts (a browser for a link...) doesn't get Prisma
	if (const char *preload = std::getenv("LD_PRELOAD")) {
		Dl_info info{};
		std::string self = dladdr(reinterpret_cast<void *>(&report_ready), &info) && info.dli_fname ? info.dli_fname : "";
		std::string rest, list = preload;
		size_t start = 0;
		while (start <= list.size()) {
			size_t end = list.find_first_of(": ", start);
			if (end == std::string::npos)
				end = list.size();
			std::string item = list.substr(start, end - start);
			if (!item.empty() && item != self && fs::path(item).filename() != fs::path(self).filename())
				rest += (rest.empty() ? "" : ":") + item;
			start = end + 1;
		}
		if (rest.empty())
			unsetenv("LD_PRELOAD");
		else
			setenv("LD_PRELOAD", rest.c_str(), 1);
	}
}

void open_folder(const fs::path &folder)
{
	std::string path = folder.string();
	char *argv[] = {const_cast<char *>("xdg-open"), path.data(), nullptr};
	pid_t pid = 0;
	if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0) {
		// Reaped from another thread so the game doesn't wait for it
		std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
	}
}

} // namespace prisma::platform
