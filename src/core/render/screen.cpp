// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/render/screen.hpp"
#include "core/hooks/glapi.hpp"
#include "common/log.hpp"
#include "core/render/pipeline.hpp"
#include "core/hooks/sdl.hpp"
#include "common/util.hpp"

#include <string>

namespace prisma::screen {

constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;
constexpr GLenum GL_UNSIGNED_INT_24_8 = 0x84FA;
constexpr GLenum GL_FRONT_ = 0x0404, GL_BACK_ = 0x0405, GL_FRONT_LEFT_ = 0x0400, GL_BACK_LEFT_ = 0x0402;

static bool g_active = false;        // this frame
static bool g_world_to_window = false; // this frame the world went to framebuffer 0
static double g_last_world = -100;
static GLuint g_fbo = 0, g_color = 0, g_depth = 0;
static GLsizei g_w = 0, g_h = 0;
static bool g_logged = false;
static int g_window_samples = -1; // GL_SAMPLE_BUFFERS of the window; -1 = not asked yet

bool active()
{
	return g_active;
}

bool msaa_window()
{
	return g_window_samples > 0;
}

GLuint map_framebuffer(GLuint fbo)
{
	return g_active && fbo == 0 ? g_fbo : fbo;
}

GLenum map_buffer(GLenum buffer)
{
	if (!g_active || tracker::draw_fbo() != 0)
		return buffer;
	if (buffer == GL_BACK_ || buffer == GL_FRONT_ || buffer == GL_BACK_LEFT_ || buffer == GL_FRONT_LEFT_)
		return GL_COLOR_ATTACHMENT0;
	return buffer;
}

void note_world(GLuint fbo)
{
	if (fbo == 0) {
		g_world_to_window = true;
		g_last_world = uptime();
	}
}

// Creates or resizes the stand-in. The caller saves nothing: this happens
// between frames, with Prisma's own bindings restored right after.
static bool prepare(GLsizei w, GLsizei h)
{
	if (g_fbo && g_w == w && g_h == h)
		return true;
	if (!g_fbo)
		gl::GenFramebuffers(1, &g_fbo);
	if (g_color)
		gl::DeleteTextures(1, &g_color);
	if (g_depth)
		gl::DeleteTextures(1, &g_depth);
	GLint unit = 0, tex = 0;
	gl::GetIntegerv(GL_ACTIVE_TEXTURE, &unit);
	gl::ActiveTexture(GL_TEXTURE0);
	gl::GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	GLuint ids[2];
	gl::GenTextures(2, ids);
	g_color = ids[0];
	g_depth = ids[1];
	gl::BindTexture(GL_TEXTURE_2D, g_color);
	gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl::BindTexture(GL_TEXTURE_2D, g_depth);
	gl::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl::BindTexture(GL_TEXTURE_2D, tex);
	gl::ActiveTexture(unit);

	GLint draw = 0;
	gl::GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
	gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
	gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_depth, 0);
	bool ok = gl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
	if (!ok) {
		log::error("Can't create Prisma's intermediate screen");
		return false;
	}
	g_w = w;
	g_h = h;
	if (!g_logged) {
		g_logged = true;
		log::info("Luanti draws without postprocessing: Prisma uses its own intermediate screen (%dx%d)", w, h);
	}
	return true;
}

void end_frame(void *window)
{
	if (g_active) {
		// The frame is finished: show it. A blit honours the scissor test
		// and sRGB conversion, which the GUI may leave on; and it copies from
		// the read buffer selected in our framebuffer, so that is set here
		// too (when it was GL_NONE nothing was copied and the window kept
		// showing old frames).
		GLboolean scissor = gl::IsEnabled(GL_SCISSOR_TEST);
		GLboolean srgb = gl::IsEnabled(GL_FRAMEBUFFER_SRGB);
		gl::Disable(GL_SCISSOR_TEST);
		gl::Disable(GL_FRAMEBUFFER_SRGB);
		for (int i = 0; i < 16 && gl::GetError(); i++) {
		}
		gl::BindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
		gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		// Whatever was selected before, read our colour and write the back buffer.
		gl::ReadBuffer(GL_COLOR_ATTACHMENT0);
		gl::DrawBuffer(0x0405 /* GL_BACK */);
		gl::BlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		gl::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		if (scissor)
			gl::Enable(GL_SCISSOR_TEST);
		if (srgb)
			gl::Enable(GL_FRAMEBUFFER_SRGB);
		static bool reported = false;
		if (GLenum e = gl::GetError(); e && !reported) {
			reported = true;
			log::error("Intermediate screen: copying to the window gave the OpenGL error 0x%X", e);
		}
	}

	// Decide for the next frame. Kept on for a while without world (only
	// sky in view, a menu over the game) so it doesn't flicker on and off.
	bool want = pipeline::active() && uptime() - g_last_world < 5.0 && gl::load();
	// A multisampled window can't receive the copy (glBlitFramebuffer
	// refuses): then Prisma stays out of the way.
	if (want && g_window_samples < 0) {
		GLint draw = 0;
		gl::GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
		gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		g_window_samples = 0;
		gl::GetIntegerv(0x80A8 /* GL_SAMPLE_BUFFERS */, &g_window_samples);
		gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
		if (g_window_samples > 0)
			log::error("Luanti's window uses multisample antialiasing (MSAA) without postprocessing: Prisma cannot "
				"apply the pack. Enable postprocessing or use another antialiasing (FXAA, SSAA).");
	}
	if (g_window_samples > 0)
		want = false;
	g_world_to_window = false;
	GLsizei w = 0, h = 0;
	if (want && (!sdl::drawable_size(window, w, h) || !prepare(w, h)))
		want = false;
	if (want != g_active)
		log::info("Prisma intermediate screen %s", want ? "on" : "off");
	g_active = want;
}

void begin_frame()
{
	// The game believes framebuffer 0 is bound (it is, after the swap).
	if (g_active)
		gl::BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
}

tracker::SurfaceInfo color()
{
	return g_active ? tracker::SurfaceInfo{g_color, GL_RGBA8, g_w, g_h} : tracker::SurfaceInfo{};
}

tracker::SurfaceInfo depth()
{
	return g_active ? tracker::SurfaceInfo{g_depth, GL_DEPTH24_STENCIL8, g_w, g_h} : tracker::SurfaceInfo{};
}

} // namespace prisma::screen
