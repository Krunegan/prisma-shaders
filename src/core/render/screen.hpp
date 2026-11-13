// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Without Luanti's post-processing (enable_post_processing = false) the
	world is drawn straight to the window, where there is no depth texture
	and nothing a pass can read. Prisma then gives the game a stand-in for
	the window: while it is on, every bind of framebuffer 0 lands on
	Prisma's own framebuffer (colour + depth/stencil textures), so the scene
	can be processed like with post-processing on; at the end of the frame
	the result is copied to the real window, right before the swap.

	The game never notices: for it (and for the tracker) framebuffer 0 is still
	bound. It is only on while the world is being drawn to framebuffer 0 and
	the pack is active.
*/
#include "core/hooks/gl_defs.hpp"
#include "core/tracking/tracker.hpp"

namespace prisma::screen {

bool active();
// Luanti's window is multisampled (antialiasing = fsaa without
// post-processing): the stand-in can't be used and the pack is not applied
bool msaa_window();

// From the hooks: what to really bind / which buffer to really select.
GLuint map_framebuffer(GLuint fbo);
GLenum map_buffer(GLenum buffer); // GL_BACK & co. -> GL_COLOR_ATTACHMENT0 while active

// The world was drawn this frame into the game's framebuffer `fbo`.
void note_world(GLuint fbo);

// Before the overlay and the swap: copies the stand-in to the window and
// leaves the real framebuffer 0 bound.
void end_frame(void *window);
// Right after the swap: binds the stand-in again for the next frame, if on.
void begin_frame();

// The stand-in's textures, as the scene for the passes.
tracker::SurfaceInfo color();
tracker::SurfaceInfo depth();

} // namespace prisma::screen
