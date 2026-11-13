// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/hooks/hooks.hpp"
#include "common/util.hpp"
#include "core/dev/hookcost.hpp"
#include "core/render/frame.hpp"
#include "core/hooks/gl_defs.hpp"
#include "common/log.hpp"
#include "core/render/gameshaders.hpp"
#include "core/render/geometry.hpp"
#include "core/render/pipeline.hpp"
#include "core/platform/platform.hpp"
#include "core/render/screen.hpp"
#include "core/hooks/sdl.hpp"
#include "core/tracking/tracker.hpp"

#include <cstdint>
#include <cstring>
#include <iterator>

/*
	The game reaches OpenGL in two ways, and both are intercepted (see
	platform.hpp):

	  * SDL_GL_GetProcAddress: everything newer than GL 1.1 (shaders,
	    framebuffers, uniforms...) and part of GL 1.1 too. We hand out our
	    wrappers instead of the driver's functions, remembering the real ones.

	  * A few GL 1.1 functions that luanti.exe imports from the GL library
	    itself (glClear, glDrawElements...).

	The end of each frame is SDL_GL_SwapWindow.
*/

namespace prisma::hooks {

// HOOK_COST at the start of a hook and REAL(o_glX)(...) for the real call:
// Prisma's own time in the hook, in developer mode (core/dev/hookcost.hpp)
#define HOOK_COST \
	static hookcost::Counter hook_counter_(__func__); \
	hookcost::CallScope hook_cost_(hook_counter_)
template <typename F>
struct Real;
template <typename R, typename... A>
struct Real<R (*)(A...)> {
	R (*f)(A...);
	R operator()(A... a) const
	{
		hookcost::RealScope real;
		return f(a...);
	}
};
#define REAL(f) Real<decltype(f)>{f}

#define GLFN(ret, name, params) 	using PFN_##name = ret(APIENTRY *) params; 	static PFN_##name o_##name = nullptr;

// ---------------------------------------------------------------- SDL

using PFN_SDL_GL_GetProcAddress = void *(SDLCALL *)(const char *);
using PFN_SDL_GL_SwapWindow = void(SDLCALL *)(void *);
static PFN_SDL_GL_GetProcAddress o_SDL_GL_GetProcAddress = nullptr;
static PFN_SDL_GL_SwapWindow o_SDL_GL_SwapWindow = nullptr;

static void SDLCALL hk_SDL_GL_SwapWindow(void *window)
{
	int64_t start = hookcost::g_on ? hookcost::now_ns() : 0, passes = hookcost::g_passes_ns;
	frame::on_swap(window);
	tracker::on_frame_end();
	if (start) // the passes, if they ran here, are counted apart
		hookcost::add_frame_end(hookcost::now_ns() - start - (hookcost::g_passes_ns - passes));
	o_SDL_GL_SwapWindow(window);
	screen::begin_frame();
}

/*
	[debug] measure_only = 1: Prisma only counts frames, to measure the game
	without Prisma. No other hook is
	installed: the game gets its own GL functions and context.
*/
static double g_first_frame = -1, g_period_start = 0;
static int g_period_frames = 0;

static void SDLCALL hk_SDL_GL_SwapWindow_count(void *window)
{
	o_SDL_GL_SwapWindow(window);
	double now = uptime();
	if (g_first_frame < 0) {
		g_first_frame = g_period_start = now;
		log::info("First frame at %.2f s", now);
	}
	g_period_frames++;
	if (now - g_period_start >= 5.0) {
		log::info("%.1f FPS", g_period_frames / (now - g_period_start));
		g_period_start = now;
		g_period_frames = 0;
	}
}

bool install_frame_counter()
{
	platform::Interpose sdl[] = {
		{platform::Library::sdl, "SDL_GL_SwapWindow", (void *)&hk_SDL_GL_SwapWindow_count, (void **)&o_SDL_GL_SwapWindow},
	};
	return platform::interpose(sdl);
}

/*
	The GL context. Luanti's opengl3 driver asks for OpenGL 3.2 (with the
	compatibility profile), and drivers give just that, even when they could
	do 4.6. A 4.3 context of the same profile runs everything 3.2 does and
	adds what packs can use on top (compute shaders), so Prisma asks for it
	first and falls back to what Luanti asked for when the driver can't.
	Other requests (OpenGL 2, OpenGL ES) are left as they are.
*/
using PFN_SDL_GL_SetAttribute = int(SDLCALL *)(int, int);
using PFN_SDL_GL_CreateContext = void *(SDLCALL *)(void *);
static PFN_SDL_GL_SetAttribute o_SDL_GL_SetAttribute = nullptr;
static PFN_SDL_GL_CreateContext o_SDL_GL_CreateContext = nullptr;

// SDL_GLattr and SDL_GLprofile
constexpr int SDL_GL_CONTEXT_MAJOR_VERSION = 17, SDL_GL_CONTEXT_MINOR_VERSION = 18,
	SDL_GL_CONTEXT_PROFILE_MASK = 21;
constexpr int SDL_GL_CONTEXT_PROFILE_ES = 4;

static bool g_modern_context = true;
static int g_asked_major = 0, g_asked_minor = 0, g_asked_profile = 0; // what the game set

static int SDLCALL hk_SDL_GL_SetAttribute(int attr, int value)
{
	if (attr == SDL_GL_CONTEXT_MAJOR_VERSION)
		g_asked_major = value;
	else if (attr == SDL_GL_CONTEXT_MINOR_VERSION)
		g_asked_minor = value;
	else if (attr == SDL_GL_CONTEXT_PROFILE_MASK)
		g_asked_profile = value;
	return o_SDL_GL_SetAttribute(attr, value);
}

static void *SDLCALL hk_SDL_GL_CreateContext(void *window)
{
	const int asked = g_asked_major * 10 + g_asked_minor;
	log::info("Luanti asks for an OpenGL %d.%d context (profile %d)", g_asked_major, g_asked_minor, g_asked_profile);
	if (!g_modern_context || (g_asked_profile & SDL_GL_CONTEXT_PROFILE_ES) || asked < 32 || asked >= 43)
		return o_SDL_GL_CreateContext(window);
	o_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	o_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	void *context = o_SDL_GL_CreateContext(window);
	// As the game left them, for any later context
	o_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, g_asked_major);
	o_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, g_asked_minor);
	if (context) {
		log::info("OpenGL 4.3 context instead of the %d.%d Luanti asks for", g_asked_major, g_asked_minor);
		return context;
	}
	log::info("The driver does not give an OpenGL 4.3 context: using Luanti's %d.%d (no compute shaders)",
		g_asked_major, g_asked_minor);
	return o_SDL_GL_CreateContext(window);
}

void set_modern_context(bool enabled)
{
	g_modern_context = enabled;
}

// ---------------------------------------------------------------- GL 1.1

GLFN(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))
GLFN(void, glDrawArrays, (GLenum, GLint, GLsizei))
GLFN(void, glClear, (GLbitfield))
GLFN(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
GLFN(void, glBindTexture, (GLenum, GLuint))
GLFN(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
GLFN(void, glDeleteTextures, (GLsizei, const GLuint *))
GLFN(void, glDrawBuffer, (GLenum))
GLFN(void, glReadBuffer, (GLenum))
GLFN(void, glEnable, (GLenum))
GLFN(void, glDisable, (GLenum))
GLFN(void, glCullFace, (GLenum))
GLFN(void, glFrontFace, (GLenum))

// Framebuffer 0 may really be Prisma's stand-in (screen.hpp): its only
// colour buffer is an attachment, not the window's back buffer.
static void APIENTRY hk_glDrawBuffer(GLenum mode)
{
	HOOK_COST;
	REAL(o_glDrawBuffer)(screen::map_buffer(mode));
}

static void APIENTRY hk_glReadBuffer(GLenum mode)
{
	HOOK_COST;
	REAL(o_glReadBuffer)(screen::map_buffer(mode));
}

static void APIENTRY hk_glEnable(GLenum cap)
{
	HOOK_COST;
	tracker::on_enable(cap, true);
	REAL(o_glEnable)(cap);
}

static void APIENTRY hk_glDisable(GLenum cap)
{
	HOOK_COST;
	tracker::on_enable(cap, false);
	REAL(o_glDisable)(cap);
}

static void APIENTRY hk_glCullFace(GLenum mode)
{
	HOOK_COST;
	tracker::on_cull_face(mode);
	REAL(o_glCullFace)(mode);
}

static void APIENTRY hk_glFrontFace(GLenum mode)
{
	HOOK_COST;
	tracker::on_front_face(mode);
	REAL(o_glFrontFace)(mode);
}

static void APIENTRY hk_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	HOOK_COST;
	if (geometry::skip_draw() || frame::skip_overlay(0, count, type, indices))
		return;
	frame::before_draw();
	geometry::on_draw(mode, count, type, indices, 0, 1);
	tracker::on_draw("elements", count, 1, type, indices);
	REAL(o_glDrawElements)(mode, count, type, indices);
}

static void APIENTRY hk_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	HOOK_COST;
	if (geometry::skip_draw() || frame::skip_overlay(first, count, 0, nullptr))
		return;
	frame::before_draw();
	tracker::on_draw("arrays", count, 1);
	REAL(o_glDrawArrays)(mode, first, count);
}

static void APIENTRY hk_glClear(GLbitfield mask)
{
	HOOK_COST;
	frame::before_clear(mask);
	geometry::on_clear(mask);
	tracker::on_clear(mask);
	REAL(o_glClear)(mask);
}

static void APIENTRY hk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	HOOK_COST;
	tracker::on_viewport(x, y, w, h);
	REAL(o_glViewport)(x, y, w, h);
}

static void APIENTRY hk_glBindTexture(GLenum target, GLuint texture)
{
	HOOK_COST;
	tracker::on_bind_texture(target, texture);
	REAL(o_glBindTexture)(target, texture);
}

// GL reuses the names of deleted objects: whoever remembers a texture or a
// framebuffer by its number has to forget it when it goes away.
static void APIENTRY hk_glDeleteTextures(GLsizei n, const GLuint *textures)
{
	HOOK_COST;
	for (GLsizei i = 0; i < n; i++) {
		tracker::on_delete_texture(textures[i]);
		pipeline::on_texture_deleted(textures[i]);
	}
	REAL(o_glDeleteTextures)(n, textures);
}

static void APIENTRY hk_glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei w, GLsizei h,
	GLint border, GLenum format, GLenum type, const void *pixels)
{
	HOOK_COST;
	tracker::on_tex_image(target, level, (GLenum)internal_format, w, h);
	REAL(o_glTexImage2D)(target, level, internal_format, w, h, border, format, type, pixels);
}

// ---------------------------------------------------------------- shaders

GLFN(GLuint, glCreateShader, (GLenum))
GLFN(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
GLFN(void, glCompileShader, (GLuint))
GLFN(void, glAttachShader, (GLuint, GLuint))
GLFN(void, glLinkProgram, (GLuint))
GLFN(void, glDeleteProgram, (GLuint))
GLFN(void, glUseProgram, (GLuint))
GLFN(GLint, glGetUniformLocation, (GLuint, const GLchar *))

static GLuint APIENTRY hk_glCreateShader(GLenum type)
{
	HOOK_COST;
	GLuint id = REAL(o_glCreateShader)(type);
	tracker::on_create_shader(id, type);
	return id;
}

static void APIENTRY hk_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	HOOK_COST;
	std::string source;
	for (GLsizei k = 0; k < count; k++) {
		if (!strings[k])
			continue;
		if (lengths && lengths[k] >= 0)
			source.append(strings[k], lengths[k]);
		else
			source.append(strings[k]);
	}
	std::string patched = gameshaders::source_for(shader, tracker::shader_type(shader), source);
	if (patched.empty()) {
		tracker::on_shader_source(shader, source);
		REAL(o_glShaderSource)(shader, count, strings, lengths);
		return;
	}
	tracker::on_shader_source(shader, patched);
	const GLchar *str = patched.c_str();
	REAL(o_glShaderSource)(shader, 1, &str, nullptr);
}

static void APIENTRY hk_glCompileShader(GLuint shader)
{
	HOOK_COST;
	REAL(o_glCompileShader)(shader);
	std::string fallback = gameshaders::after_compile(shader);
	if (!fallback.empty())
		tracker::on_shader_source(shader, fallback);
}

static void APIENTRY hk_glAttachShader(GLuint program, GLuint shader)
{
	HOOK_COST;
	tracker::on_attach_shader(program, shader);
	gameshaders::on_attach(program, shader);
	REAL(o_glAttachShader)(program, shader);
}

static void APIENTRY hk_glLinkProgram(GLuint program)
{
	HOOK_COST;
	REAL(o_glLinkProgram)(program);
	gameshaders::after_link(program);
	tracker::on_link_program(program);
}

static void APIENTRY hk_glDeleteProgram(GLuint program)
{
	HOOK_COST;
	tracker::on_delete_program(program);
	gameshaders::on_delete_program(program);
	REAL(o_glDeleteProgram)(program);
}

static void APIENTRY hk_glUseProgram(GLuint program)
{
	HOOK_COST;
	tracker::on_use_program(program);
	REAL(o_glUseProgram)(program);
}

static GLint APIENTRY hk_glGetUniformLocation(GLuint program, const GLchar *name)
{
	HOOK_COST;
	GLint loc = gameshaders::on_get_uniform_location(program, name, REAL(o_glGetUniformLocation)(program, name));
	tracker::on_get_uniform_location(program, name, loc);
	return loc;
}

// ---------------------------------------------------------------- uniforms

GLFN(void, glUniform1f, (GLint, GLfloat))
GLFN(void, glUniform2f, (GLint, GLfloat, GLfloat))
GLFN(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GLFN(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GLFN(void, glUniform1i, (GLint, GLint))
GLFN(void, glUniform2i, (GLint, GLint, GLint))
GLFN(void, glUniform3i, (GLint, GLint, GLint, GLint))
GLFN(void, glUniform4i, (GLint, GLint, GLint, GLint, GLint))
GLFN(void, glUniform1fv, (GLint, GLsizei, const GLfloat *))
GLFN(void, glUniform2fv, (GLint, GLsizei, const GLfloat *))
GLFN(void, glUniform3fv, (GLint, GLsizei, const GLfloat *))
GLFN(void, glUniform4fv, (GLint, GLsizei, const GLfloat *))
GLFN(void, glUniform1iv, (GLint, GLsizei, const GLint *))
GLFN(void, glUniform2iv, (GLint, GLsizei, const GLint *))
GLFN(void, glUniform3iv, (GLint, GLsizei, const GLint *))
GLFN(void, glUniform4iv, (GLint, GLsizei, const GLint *))
GLFN(void, glUniformMatrix2fv, (GLint, GLsizei, GLboolean, const GLfloat *))
GLFN(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *))
GLFN(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))

// Luanti sees virtual locations for the programs Prisma may rebuild.
static inline GLint real_loc(GLint loc)
{
	return gameshaders::g_virtual_locations ? gameshaders::real_location(tracker::current_program(), loc) : loc;
}

#define UNIFORM_SCALAR_F(n, ...) \
	static void APIENTRY hk_glUniform##n##f(GLint loc, __VA_ARGS__) \
	{ \
		HOOK_COST; \
		const GLfloat v[] = {UNIFORM_ARGS_##n}; \
		REAL(o_glUniform##n##f)(real_loc(loc), UNIFORM_ARGS_##n); \
		tracker::on_uniform(loc, tracker::UKind::Float, n, 1, v); \
	}
#define UNIFORM_SCALAR_I(n, ...) \
	static void APIENTRY hk_glUniform##n##i(GLint loc, __VA_ARGS__) \
	{ \
		HOOK_COST; \
		const GLint v[] = {UNIFORM_ARGS_##n}; \
		REAL(o_glUniform##n##i)(real_loc(loc), UNIFORM_ARGS_##n); \
		tracker::on_uniform(loc, tracker::UKind::Int, n, 1, v); \
	}
#define UNIFORM_ARGS_1 a
#define UNIFORM_ARGS_2 a, b
#define UNIFORM_ARGS_3 a, b, c
#define UNIFORM_ARGS_4 a, b, c, d

static void APIENTRY hk_glUniform1f(GLint loc, GLfloat a)
{
	HOOK_COST;
	REAL(o_glUniform1f)(real_loc(loc), geometry::filter_uniform(loc, a));
	tracker::on_uniform(loc, tracker::UKind::Float, 1, 1, &a);
}
UNIFORM_SCALAR_F(2, GLfloat a, GLfloat b)
UNIFORM_SCALAR_F(3, GLfloat a, GLfloat b, GLfloat c)
UNIFORM_SCALAR_F(4, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
UNIFORM_SCALAR_I(1, GLint a)
UNIFORM_SCALAR_I(2, GLint a, GLint b)
UNIFORM_SCALAR_I(3, GLint a, GLint b, GLint c)
UNIFORM_SCALAR_I(4, GLint a, GLint b, GLint c, GLint d)

#define UNIFORM_VECTOR(n, suffix, type, kind) \
	static void APIENTRY hk_glUniform##n##suffix##v(GLint loc, GLsizei count, const type *v) \
	{ \
		HOOK_COST; \
		REAL(o_glUniform##n##suffix##v)(real_loc(loc), count, v); \
		tracker::on_uniform(loc, tracker::UKind::kind, n, count, v); \
	}

static void APIENTRY hk_glUniform1fv(GLint loc, GLsizei count, const GLfloat *v)
{
	HOOK_COST;
	if (count == 1 && v) {
		GLfloat value = geometry::filter_uniform(loc, v[0]);
		REAL(o_glUniform1fv)(real_loc(loc), 1, &value);
	} else {
		REAL(o_glUniform1fv)(real_loc(loc), count, v);
	}
	tracker::on_uniform(loc, tracker::UKind::Float, 1, count, v);
}
UNIFORM_VECTOR(2, f, GLfloat, Float)
UNIFORM_VECTOR(3, f, GLfloat, Float)
UNIFORM_VECTOR(4, f, GLfloat, Float)
UNIFORM_VECTOR(1, i, GLint, Int)
UNIFORM_VECTOR(2, i, GLint, Int)
UNIFORM_VECTOR(3, i, GLint, Int)
UNIFORM_VECTOR(4, i, GLint, Int)

#define UNIFORM_MATRIX(n) \
	static void APIENTRY hk_glUniformMatrix##n##fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) \
	{ \
		HOOK_COST; \
		REAL(o_glUniformMatrix##n##fv)(real_loc(loc), count, transpose, v); \
		tracker::on_uniform(loc, tracker::UKind::Matrix, n * n, count, v); \
	}

UNIFORM_MATRIX(2)
UNIFORM_MATRIX(3)
UNIFORM_MATRIX(4)

// ---------------------------------------------------------------- framebuffers & textures

GLFN(void, glBindFramebuffer, (GLenum, GLuint))
GLFN(void, glDeleteFramebuffers, (GLsizei, const GLuint *))
GLFN(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLFN(void, glFramebufferTexture, (GLenum, GLenum, GLuint, GLint))
GLFN(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
GLFN(void, glBindRenderbuffer, (GLenum, GLuint))
GLFN(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))
GLFN(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))
GLFN(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
GLFN(void, glDrawBuffers, (GLsizei, const GLenum *))
GLFN(void, glActiveTexture, (GLenum))
GLFN(void, glTexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))

static void APIENTRY hk_glBindFramebuffer(GLenum target, GLuint fbo)
{
	HOOK_COST;
	frame::before_bind_framebuffer(target, fbo);
	tracker::on_bind_framebuffer(target, fbo);
	REAL(o_glBindFramebuffer)(target, screen::map_framebuffer(fbo));
}

static void APIENTRY hk_glDeleteFramebuffers(GLsizei n, const GLuint *fbos)
{
	HOOK_COST;
	for (GLsizei i = 0; i < n; i++) {
		tracker::on_delete_framebuffer(fbos[i]);
		frame::on_framebuffer_deleted(fbos[i]);
	}
	REAL(o_glDeleteFramebuffers)(n, fbos);
}

static void APIENTRY hk_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
	GLint level)
{
	HOOK_COST;
	tracker::on_framebuffer_texture(target, attachment, texture, level, textarget == 0x9100 /* GL_TEXTURE_2D_MULTISAMPLE */);
	REAL(o_glFramebufferTexture2D)(target, attachment, textarget, texture, level);
}

static void APIENTRY hk_glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level)
{
	HOOK_COST;
	tracker::on_framebuffer_texture(target, attachment, texture, level);
	REAL(o_glFramebufferTexture)(target, attachment, texture, level);
}

static void APIENTRY hk_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb)
{
	HOOK_COST;
	tracker::on_framebuffer_renderbuffer(target, attachment, rb);
	REAL(o_glFramebufferRenderbuffer)(target, attachment, rbtarget, rb);
}

static void APIENTRY hk_glBindRenderbuffer(GLenum target, GLuint rb)
{
	HOOK_COST;
	tracker::on_bind_renderbuffer(rb);
	REAL(o_glBindRenderbuffer)(target, rb);
}

static void APIENTRY hk_glRenderbufferStorage(GLenum target, GLenum fmt, GLsizei w, GLsizei h)
{
	HOOK_COST;
	tracker::on_renderbuffer_storage(fmt, w, h, 0);
	REAL(o_glRenderbufferStorage)(target, fmt, w, h);
}

static void APIENTRY hk_glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum fmt, GLsizei w,
	GLsizei h)
{
	HOOK_COST;
	tracker::on_renderbuffer_storage(fmt, w, h, samples);
	REAL(o_glRenderbufferStorageMultisample)(target, samples, fmt, w, h);
}

static void APIENTRY hk_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
	GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
	HOOK_COST;
	tracker::on_blit_framebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask);
	REAL(o_glBlitFramebuffer)(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
	frame::after_blit(tracker::read_fbo(), tracker::draw_fbo(), mask);
}

static void APIENTRY hk_glDrawBuffers(GLsizei n, const GLenum *bufs)
{
	HOOK_COST;
	tracker::on_draw_buffers(n, bufs);
	if (screen::active() && n > 0 && n <= 8 && bufs) {
		GLenum mapped[8];
		for (GLsizei i = 0; i < n; i++)
			mapped[i] = screen::map_buffer(bufs[i]);
		REAL(o_glDrawBuffers)(n, mapped);
		return;
	}
	REAL(o_glDrawBuffers)(n, bufs);
}

static void APIENTRY hk_glActiveTexture(GLenum unit)
{
	HOOK_COST;
	tracker::on_active_texture(unit);
	REAL(o_glActiveTexture)(unit);
}

static void APIENTRY hk_glTexStorage2D(GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h)
{
	HOOK_COST;
	tracker::on_tex_image(target, 0, fmt, w, h);
	REAL(o_glTexStorage2D)(target, levels, fmt, w, h);
}

// ---------------------------------------------------------------- vertex data

GLFN(void, glBindBuffer, (GLenum, GLuint))
GLFN(void, glBindVertexArray, (GLuint))
GLFN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
GLFN(void, glEnableVertexAttribArray, (GLuint))
GLFN(void, glDisableVertexAttribArray, (GLuint))
GLFN(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))
GLFN(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *))
GLFN(void, glDeleteBuffers, (GLsizei, const GLuint *))
GLFN(void, glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void *))
GLFN(void *, glMapBuffer, (GLenum, GLenum))
GLFN(void *, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))
GLFN(GLboolean, glUnmapBuffer, (GLenum))

static void APIENTRY hk_glBindBuffer(GLenum target, GLuint buffer)
{
	HOOK_COST;
	tracker::on_bind_buffer(target, buffer);
	REAL(o_glBindBuffer)(target, buffer);
}

static void APIENTRY hk_glBindVertexArray(GLuint vao)
{
	HOOK_COST;
	tracker::on_bind_vertex_array(vao);
	REAL(o_glBindVertexArray)(vao);
}

static void APIENTRY hk_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
	GLsizei stride, const void *pointer)
{
	HOOK_COST;
	tracker::on_vertex_attrib_pointer(index, size, type, normalized, stride, pointer);
	REAL(o_glVertexAttribPointer)(index, size, type, normalized, stride, pointer);
}

static void APIENTRY hk_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
	const void *pointer)
{
	HOOK_COST;
	tracker::on_vertex_attrib_pointer(index, size, type, 0, stride, pointer, true);
	REAL(o_glVertexAttribIPointer)(index, size, type, stride, pointer);
}

// Mapped buffers can be written behind our back: count them as changed.
static void *APIENTRY hk_glMapBuffer(GLenum target, GLenum access)
{
	HOOK_COST;
	tracker::on_buffer_data(target, 0, nullptr, 0, true);
	return REAL(o_glMapBuffer)(target, access);
}

static void *APIENTRY hk_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
	HOOK_COST;
	tracker::on_buffer_data(target, length, nullptr, 0, true);
	return REAL(o_glMapBufferRange)(target, offset, length, access);
}

static GLboolean APIENTRY hk_glUnmapBuffer(GLenum target)
{
	HOOK_COST;
	tracker::on_buffer_data(target, 0, nullptr, 0, true);
	return REAL(o_glUnmapBuffer)(target);
}

static void APIENTRY hk_glEnableVertexAttribArray(GLuint index)
{
	HOOK_COST;
	tracker::on_enable_vertex_attrib(index, true);
	REAL(o_glEnableVertexAttribArray)(index);
}

static void APIENTRY hk_glDisableVertexAttribArray(GLuint index)
{
	HOOK_COST;
	tracker::on_enable_vertex_attrib(index, false);
	REAL(o_glDisableVertexAttribArray)(index);
}

static void APIENTRY hk_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	HOOK_COST;
	tracker::on_buffer_data(target, size, data, usage, false);
	REAL(o_glBufferData)(target, size, data, usage);
}

static void APIENTRY hk_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	HOOK_COST;
	tracker::on_buffer_data(target, size, data, 0, true);
	REAL(o_glBufferSubData)(target, offset, size, data);
}

static void APIENTRY hk_glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
	HOOK_COST;
	for (GLsizei i = 0; i < n; i++)
		tracker::on_delete_buffer(buffers[i]);
	REAL(o_glDeleteBuffers)(n, buffers);
}

// ---------------------------------------------------------------- newer draw calls

GLFN(void, glDrawRangeElements, (GLenum, GLuint, GLuint, GLsizei, GLenum, const void *))
GLFN(void, glDrawElementsInstanced, (GLenum, GLsizei, GLenum, const void *, GLsizei))
GLFN(void, glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))
GLFN(void, glDrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void *, GLint))

static void APIENTRY hk_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
	const void *indices)
{
	HOOK_COST;
	if (geometry::skip_draw() || frame::skip_overlay(0, count, type, indices))
		return;
	frame::before_draw();
	geometry::on_draw(mode, count, type, indices, 0, 1);
	tracker::on_draw("range", count, 1, type, indices);
	REAL(o_glDrawRangeElements)(mode, start, end, count, type, indices);
}

static void APIENTRY hk_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices,
	GLsizei instances)
{
	HOOK_COST;
	if (geometry::skip_draw())
		return;
	frame::before_draw();
	geometry::on_draw(mode, count, type, indices, 0, instances);
	tracker::on_draw("instanced", count, instances, type, indices);
	REAL(o_glDrawElementsInstanced)(mode, count, type, indices, instances);
}

static void APIENTRY hk_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
	HOOK_COST;
	if (geometry::skip_draw())
		return;
	frame::before_draw();
	tracker::on_draw("arrays_instanced", count, instances);
	REAL(o_glDrawArraysInstanced)(mode, first, count, instances);
}

static void APIENTRY hk_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
	GLint base)
{
	HOOK_COST;
	if (geometry::skip_draw())
		return;
	frame::before_draw();
	geometry::on_draw(mode, count, type, indices, base, 1);
	tracker::on_draw("basevertex", count, 1, type, indices);
	REAL(o_glDrawElementsBaseVertex)(mode, count, type, indices, base);
}

// ---------------------------------------------------------------- SDL_GL_GetProcAddress

struct ProcHook {
	const char *name;
	void *detour;
	void **original;
	bool imported; // luanti.exe may also import it from the GL library
};

#define DYN(name) {#name, (void *)&hk_##name, (void **)&o_##name, false}
#define IMP(name) {#name, (void *)&hk_##name, (void **)&o_##name, true}

static ProcHook g_procs[] = {
	IMP(glDrawElements), IMP(glDrawArrays), IMP(glClear), IMP(glViewport), IMP(glBindTexture), IMP(glTexImage2D), IMP(glDeleteTextures),
	IMP(glEnable), IMP(glDisable), IMP(glCullFace), IMP(glFrontFace), IMP(glDrawBuffer), IMP(glReadBuffer),

	DYN(glCreateShader), DYN(glShaderSource), DYN(glCompileShader), DYN(glAttachShader), DYN(glLinkProgram), DYN(glDeleteProgram),
	DYN(glUseProgram), DYN(glGetUniformLocation),

	DYN(glUniform1f), DYN(glUniform2f), DYN(glUniform3f), DYN(glUniform4f),
	DYN(glUniform1i), DYN(glUniform2i), DYN(glUniform3i), DYN(glUniform4i),
	DYN(glUniform1fv), DYN(glUniform2fv), DYN(glUniform3fv), DYN(glUniform4fv),
	DYN(glUniform1iv), DYN(glUniform2iv), DYN(glUniform3iv), DYN(glUniform4iv),
	DYN(glUniformMatrix2fv), DYN(glUniformMatrix3fv), DYN(glUniformMatrix4fv),

	DYN(glBindFramebuffer), DYN(glDeleteFramebuffers), DYN(glFramebufferTexture2D), DYN(glFramebufferTexture), DYN(glFramebufferRenderbuffer),
	DYN(glBindRenderbuffer), DYN(glRenderbufferStorage), DYN(glRenderbufferStorageMultisample),
	DYN(glBlitFramebuffer), DYN(glDrawBuffers), DYN(glActiveTexture), DYN(glTexStorage2D),

	DYN(glBindBuffer), DYN(glBindVertexArray), DYN(glVertexAttribPointer), DYN(glEnableVertexAttribArray),
	DYN(glDisableVertexAttribArray), DYN(glBufferData), DYN(glBufferSubData), DYN(glDeleteBuffers),
	DYN(glVertexAttribIPointer), DYN(glMapBuffer), DYN(glMapBufferRange), DYN(glUnmapBuffer),

	DYN(glDrawRangeElements), DYN(glDrawElementsInstanced), DYN(glDrawArraysInstanced),
	DYN(glDrawElementsBaseVertex),
};

static void *SDLCALL hk_SDL_GL_GetProcAddress(const char *name)
{
	void *real = o_SDL_GL_GetProcAddress(name);
	if (!name)
		return real;
	tracker::on_proc_request(name, real != nullptr);
	if (!real)
		return real;
	for (ProcHook &h : g_procs) {
		if (std::strcmp(h.name, name) != 0)
			continue;
		// For an imported one it is the same function the import table had
		*h.original = real;
		return h.detour;
	}
	return real;
}

// ---------------------------------------------------------------- raw access

void *raw_proc(const char *name)
{
	if (!o_SDL_GL_GetProcAddress)
		return nullptr;
	return o_SDL_GL_GetProcAddress(name);
}

// ---------------------------------------------------------------- install

bool install()
{
	using platform::Library;
	// Older Luanti on Linux (5.10 and before, in some distributions) draws
	// through X11 without SDL: there is nothing to hook into
	if (!platform::library_symbol(Library::sdl, "SDL_GL_GetProcAddress")) {
		log::error("The game does not use SDL2 (Luanti 5.10 or older, built without SDL): Prisma needs a Luanti "
				   "that is newer");
		return false;
	}
	platform::Interpose sdl[] = {
		{Library::sdl, "SDL_GL_GetProcAddress", (void *)&hk_SDL_GL_GetProcAddress, (void **)&o_SDL_GL_GetProcAddress},
		{Library::sdl, "SDL_GL_SwapWindow", (void *)&hk_SDL_GL_SwapWindow, (void **)&o_SDL_GL_SwapWindow},
		{Library::sdl, "SDL_GL_SetAttribute", (void *)&hk_SDL_GL_SetAttribute, (void **)&o_SDL_GL_SetAttribute},
		{Library::sdl, "SDL_GL_CreateContext", (void *)&hk_SDL_GL_CreateContext, (void **)&o_SDL_GL_CreateContext},
	};
	bool ok = platform::interpose(sdl);

	// The GL 1.1 functions luanti.exe imports; the rest come through SDL
	platform::Interpose gl[std::size(g_procs)];
	int count = 0;
	for (ProcHook &h : g_procs)
		if (h.imported)
			gl[count++] = {Library::gl, h.name, h.detour, h.original, false};
	platform::interpose(gl, count);
	return ok;
}

} // namespace prisma::hooks
