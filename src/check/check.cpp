// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	prisma_check(.exe) — compiles a shader pack's passes outside the game.

	Uses the same pack loader, the same headers (API, options, helpers) and
	the same OpenGL driver as Prisma inside Luanti, in a hidden window, so a
	pack author (or Prisma's own development) can find shader errors without
	starting the game. Every value of every #define option is tried, since
	each one compiles different code.

	Usage:
	    prisma_check.exe <pack>          name inside the packs folder, or a path
	    prisma_check.exe --internal      only Prisma's own internal shaders
	    prisma_check.exe --stub <pack>   writes <pack>/prisma_api.glsl, the
	                                     declarations of the pass header for
	                                     code editors (passglsl::editor_stub)
	    prisma_check.exe --luanti <dir>  compares a Luanti folder with the
	                                     baseline of the supported version
	                                     (check/luanti_check.hpp)

	The game/ folder (changes to Luanti's own shaders) needs Luanti's
	generated sources and is only checked inside the game.
*/
#include "core/ui/debugview.hpp"
#include "core/hooks/glapi.hpp"
#include "core/render/lighting_glsl.hpp"
#include "core/pack/options.hpp"
#include "common/ini.hpp"
#include "core/pack/pack.hpp"
#include "core/pack/passglsl.hpp"
#include "common/util.hpp"
#include "check/luanti_check.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <SDL.h>
#include <cstdio>
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// glapi.cpp resolves functions through the hooks module; here there are no
// hooks, only the driver.
namespace prisma::hooks {
void *raw_proc(const char *name)
{
#ifdef _WIN32
	auto p = reinterpret_cast<intptr_t>(wglGetProcAddress(name));
	if (p == 0 || p == 1 || p == 2 || p == 3 || p == -1)
		return reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name));
	return reinterpret_cast<void *>(p);
#else
	return SDL_GL_GetProcAddress(name);
#endif
}
} // namespace prisma::hooks

using namespace prisma;

static void say(const std::string &text)
{
	std::string line = text + "\n";
#ifdef _WIN32
	DWORD written = 0;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), (DWORD)line.size(), &written, nullptr);
#else
	std::fwrite(line.data(), 1, line.size(), stdout);
#endif
}

#ifdef _WIN32
static bool create_context()
{
	WNDCLASSW wc{};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"PrismaCheck";
	RegisterClassW(&wc);
	HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr,
		wc.hInstance, nullptr);
	HDC dc = GetDC(wnd);
	PIXELFORMATDESCRIPTOR pfd{};
	pfd.nSize = sizeof pfd;
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;
	int format = ChoosePixelFormat(dc, &pfd);
	if (!format || !SetPixelFormat(dc, format, &pfd))
		return false;
	HGLRC rc = wglCreateContext(dc);
	return rc && wglMakeCurrent(dc, rc);
}
#else
// A hidden SDL window, with the context Prisma asks for in the game: 4.3
// with the compatibility profile, else 3.2 (as Luanti asks)
static bool create_context()
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		return false;
	SDL_Window *window = SDL_CreateWindow("prisma_check", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	if (!window)
		return false;
	for (int minor : {43, 32}) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, minor / 10);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor % 10);
		if (SDL_GLContext gl = SDL_GL_CreateContext(window))
			return SDL_GL_MakeCurrent(window, gl) == 0;
	}
	return false;
}
#endif

static std::string info_log(GLuint object, bool program)
{
	GLint len = 0;
	program ? gl::GetProgramiv(object, GL_INFO_LOG_LENGTH, &len) : gl::GetShaderiv(object, GL_INFO_LOG_LENGTH, &len);
	std::string s(len > 1 ? len : 1, '\0');
	program ? gl::GetProgramInfoLog(object, (GLsizei)s.size(), nullptr, s.data()) :
		gl::GetShaderInfoLog(object, (GLsizei)s.size(), nullptr, s.data());
	return s.c_str();
}

// Compiles and links; returns the driver's complaints ("" = fine).
static std::string build(const std::string &vs, const std::string &fs)
{
	std::string error;
	GLuint shaders[2] = {gl::CreateShader(GL_VERTEX_SHADER), gl::CreateShader(GL_FRAGMENT_SHADER)};
	const std::string *src[2] = {&vs, &fs};
	for (int i = 0; i < 2; i++) {
		const char *s = src[i]->c_str();
		gl::ShaderSource(shaders[i], 1, &s, nullptr);
		gl::CompileShader(shaders[i]);
		GLint ok = 0;
		gl::GetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
		if (!ok)
			error += info_log(shaders[i], false);
	}
	if (error.empty()) {
		GLuint p = gl::CreateProgram();
		gl::AttachShader(p, shaders[0]);
		gl::AttachShader(p, shaders[1]);
		gl::LinkProgram(p);
		GLint ok = 0;
		gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok)
			error = info_log(p, true);
		gl::DeleteProgram(p);
	}
	gl::DeleteShader(shaders[0]);
	gl::DeleteShader(shaders[1]);
	return error;
}

// The same for a compute shader on its own
static std::string build_compute(const std::string &cs)
{
	std::string error;
	GLuint shader = gl::CreateShader(GL_COMPUTE_SHADER);
	const char *s = cs.c_str();
	gl::ShaderSource(shader, 1, &s, nullptr);
	gl::CompileShader(shader);
	GLint ok = 0;
	gl::GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
		error = info_log(shader, false);
	if (error.empty()) {
		GLuint p = gl::CreateProgram();
		gl::AttachShader(p, shader);
		gl::LinkProgram(p);
		gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok)
			error = info_log(p, true);
		gl::DeleteProgram(p);
	}
	gl::DeleteShader(shader);
	return error;
}

static int check_internal()
{
	int failures = 0;
	struct {
		const char *name, *vs, *fs;
	} programs[] = {
		{"shadows", lighting::glsl::CASTER_VS, lighting::glsl::CASTER_FS},
		{"G-buffer", lighting::glsl::GBUFFER_VS, lighting::glsl::GBUFFER_FS},
	};
	std::string viewer = build(passglsl::VERTEX, debugview::VIEWER_FS);
	if (!viewer.empty()) {
		failures++;
		say("Internal texture viewer shader:\n" + viewer);
	}
	for (auto &p : programs)
		for (int v = 0; v < 4; v++) {
			std::string header = std::string("#version 150\n") + (v & 1 ? "#define ARRAY 1\n" : "") +
				(v & 2 ? "#define SKINNED 1\n" : "");
			std::string error = build(header + p.vs, header + p.fs);
			if (!error.empty()) {
				failures++;
				say(std::string("Internal shader of ") + p.name + (v & 1 ? " (array)" : "") + (v & 2 ? " (skeleton)" : "") +
					":\n" + error);
			}
		}
	return failures;
}

// The packs folder, as prisma.ini next to us says; `name` is a folder name in it, or a path
static fs::path pack_dir(const fs::path &name)
{
	std::string packs_text = ini::get(core_dir() / "prisma.ini", "prisma", "packs");
	fs::path packs = packs_text.empty() ? core_dir() / "shaderpacks" : utf8_path(packs_text);
	return fs::is_directory(name) ? name : packs / name;
}

// Options are read from a copy of prisma.ini: trying values must not change
// the player's saved ones.
static fs::path scratch_ini()
{
	fs::path scratch = fs::temp_directory_path() / "prisma_check.ini";
	std::error_code ec;
	fs::copy_file(core_dir() / "prisma.ini", scratch, fs::copy_options::overwrite_existing, ec);
	return scratch;
}

static int write_stub(const fs::path &name)
{
	fs::path dir = pack_dir(name);
	pack::Pack p = pack::load(dir);
	for (const std::string &e : p.errors)
		say("  " + e);
	if (!p.errors.empty())
		return 1;
	fs::path scratch = scratch_ini();
	options::set_pack(p, scratch);
	std::string text = passglsl::editor_stub(p);
	std::error_code removed;
	fs::remove(scratch, removed);
	fs::path out = dir / "prisma_api.glsl";
	std::ofstream f(out, std::ios::binary | std::ios::trunc);
	f.write(text.data(), (std::streamsize)text.size());
	if (!f) {
		say("Can't write " + to_utf8(out));
		return 1;
	}
	say("Wrote " + to_utf8(out));
	say("Include it at the start of each pass (#include \"prisma_api.glsl\") so that the editor knows the API;");
	say("inside the game nothing changes.");
	return 0;
}

static int run(const std::vector<fs::path> &args);

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
	SetConsoleOutputCP(CP_UTF8);
	std::vector<fs::path> args(argv, argv + argc);
	return run(args);
}
#else
int main(int argc, char **argv)
{
	std::vector<fs::path> args(argv, argv + argc);
	return run(args);
}
#endif

static int run(const std::vector<fs::path> &argv)
{
	const size_t argc = argv.size();
	if (argc < 2 || ((argv[1] == "--stub" || argv[1] == "--luanti") && argc < 3)) {
		say("Usage: prisma_check <pack>   (name in the packs folder, or a path)");
		say("     prisma_check --internal");
		say("     prisma_check --stub <pack>   (writes prisma_api.glsl for the editor)");
		say("     prisma_check --luanti <folder>   (compares it with Luanti " PRISMA_LUANTI_VERSION ")");
		return 2;
	}
	if (argv[1] == "--stub")
		return write_stub(argv[2]);
	if (argv[1] == "--luanti") {
		say("Luanti folder: " + to_utf8(argv[2]));
		luanti::Report r = luanti::compare_installation(argv[2], luanti::baseline());
		for (const std::string &l : r.lines)
			say(l);
		say(r.differences ? std::to_string(r.differences) + " differences from Luanti " PRISMA_LUANTI_VERSION
				". See docs/how-it-works.md, \"Supported Luanti version\"." :
			"The same as Luanti " PRISMA_LUANTI_VERSION ".");
		return r.differences ? 1 : 0;
	}
	if (!create_context() || !gl::load()) {
		say("Can't create an OpenGL context");
		return 2;
	}
	const GLubyte *renderer = gl::GetString ? gl::GetString(GL_RENDERER) : nullptr;
	const GLubyte *version = gl::GetString ? gl::GetString(GL_VERSION) : nullptr;
	say(std::string("OpenGL: ") + (renderer ? reinterpret_cast<const char *>(renderer) : "?") + " (" +
		(version ? reinterpret_cast<const char *>(version) : "?") + ")");

	int failures = check_internal();
	if (argv[1] == "--internal") {
		say(failures ? "There are errors." : "Internal shaders: ok.");
		return failures ? 1 : 0;
	}

	fs::path dir = pack_dir(argv[1]);
	say("Pack: " + to_utf8(dir));

	pack::Pack p = pack::load(dir);
	for (const std::string &e : p.errors)
		say("  " + e);
	for (const std::string &w : p.warnings)
		say("  Warning: " + w);
	if (!p.errors.empty())
		return 1;

	fs::path scratch = scratch_ini();
	options::set_pack(p, scratch);
	const bool compute = gl::has_compute();
	if (pack::uses_compute(p) && !compute)
		say("  Warning: this driver does not give OpenGL 4.3: .comp passes are not checked.");

	// The saved values, then each value of each #define option on its own
	struct Variant {
		size_t option;
		double value;
	};
	std::vector<Variant> variants{{SIZE_MAX, 0}};
	const auto &defs = options::defs();
	for (size_t i = 0; i < defs.size(); i++) {
		if (!defs[i].define)
			continue;
		double lo = defs[i].type == pack::OptionDef::Bool ? 0 : defs[i].min;
		double hi = defs[i].type == pack::OptionDef::Bool ? 1 : defs[i].max;
		double step = std::max(1.0, std::ceil((hi - lo) / 7)); // at most 8 values
		for (double v = lo; v <= hi; v += step)
			if (v != options::value(i))
				variants.push_back({i, v});
	}

	std::vector<double> saved;
	for (size_t i = 0; i < defs.size(); i++)
		saved.push_back(options::value(i));
	for (const Variant &v : variants) {
		for (size_t i = 0; i < defs.size(); i++)
			options::set(i, saved[i]);
		std::string label = "stored values";
		if (v.option != SIZE_MAX) {
			options::set(v.option, v.value);
			const pack::OptionDef &d = defs[v.option];
			label = d.id + " = " + (d.type == pack::OptionDef::Choice ? d.values[(size_t)v.value] :
				std::to_string((long long)v.value));
		}
		std::string header = passglsl::fragment_header(false, p.buffers, compute);
		for (const pack::Pass &pass : p.passes) {
			if (pass.compute && !compute)
				continue;
			std::string error = pass.compute ?
				build_compute(passglsl::compute_header(false, p.buffers, pass) + pass.code.code) :
				build(passglsl::VERTEX, header + pass.code.code);
			if (!error.empty()) {
				failures++;
				say("  " + pass.name + " (" + label + "):");
				say(pack::translate_log(error, pass.code.files));
			}
		}
	}
	std::error_code removed;
	fs::remove(scratch, removed);

	say(std::to_string(p.passes.size()) + " pass(es), " + std::to_string(variants.size()) + " option combination(s).");
	if (!p.game.empty())
		say("Changes to Luanti's shaders (game/) are only checked inside the game.");
	say(failures ? "There are errors." : "Everything compiles.");
	return failures ? 1 : 0;
}
