// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Runs the active shader pack. Each pass is a fullscreen fragment shader
	that reads the scene (colour + depth) and writes the new colour back into
	Luanti's scene texture, so the game's own post-processing (bloom, volumetric
	light, exposure, FXAA) is applied on top of it.
*/
#include "core/hooks/gl_defs.hpp"
#include "core/pack/pack.hpp"
#include "core/tracking/tracker.hpp"
#include "core/tracking/state.hpp"

#include <string>
#include <vector>

namespace prisma::pipeline {

struct Scene {
	GLuint fbo = 0;
	tracker::SurfaceInfo color, depth;
};

// Reads prisma.ini next to the core and loads the pack's files. Called once
// at startup, before the game compiles any shader (no GL yet).
void init(const std::filesystem::path &base_dir);

// Called at the moment the world has been fully drawn into `scene`.
void render(const Scene &scene, const GameState &state);

// Once per frame, right before the buffer swap: keys and hot reload.
void on_frame_end();

// Luanti has finished drawing the sky into the bound framebuffer (the area
// x, y, w, h): keeps a copy for the passes (skyTex), so they can tell what
// was drawn on top of the sky without depth (particles, translucent entities).
void capture_sky(GLint x, GLint y, GLsizei w, GLsizei h);

// The game deleted a texture: forget it if Prisma was drawing into it.
void on_texture_deleted(GLuint texture);

// Skips the pack without changing the user's on/off state (comparisons).
void set_bypass(bool bypass);

// True while the pack is applied (not switched off, not bypassed).
bool active();

// ---- used by the menu and the keys
bool enabled();
void set_enabled(bool on);
const pack::Pack &current_pack();
const std::string &selected_pack();
std::vector<std::string> available_packs();
void select_pack(const std::string &name); // takes effect at the end of the frame
void options_changed(bool recompile);
void open_pack_folder();

} // namespace prisma::pipeline
