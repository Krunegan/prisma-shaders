// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Finds the moment in each frame where the world is complete and the
	game's post-processing has not started yet, and runs the pack there.

	In Luanti's post-processing pipeline the world (sky, nodes, entities,
	clouds) is drawn into one framebuffer; the first time the game switches
	away from it, the scene is done.
*/
#include "core/hooks/gl_defs.hpp"
#include "core/tracking/state.hpp"

#include <filesystem>
#include <string>

namespace prisma::frame {

// Reads the [debug] section of prisma.ini.
void init(const std::filesystem::path &ini);

void before_bind_framebuffer(GLenum target, GLuint fbo);
// After glBlitFramebuffer: Luanti resolving a multisampled scene
// (antialiasing = fsaa) into the textures the passes can read
void after_blit(GLuint read_fbo, GLuint draw_fbo, GLbitfield mask);

// Before a draw call: true if it is Luanti's screen tint for a camera inside
// a liquid and the pack hides it (pack.toml: hide = ["underwater"]). Either
// way its colour becomes prisma.liquidColor. `indices` is null for glDrawArrays.
bool skip_overlay(GLint first, GLsizei count, GLenum index_type, const void *indices);
void set_hide_underwater(bool hide);
void before_draw();
void on_framebuffer_deleted(GLuint fbo);
// Before glClear reaches the driver.
void before_clear(GLbitfield mask);
void on_swap(void *window);

// The game state of the last frame, or null if none yet (for the menu).
const GameState *last_state();

} // namespace prisma::frame
