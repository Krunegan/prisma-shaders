// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Tools for pack authors, shown in the menu's "Debugging" page:

	  * GPU time of every pass and of Prisma's own maps (shadows, G-buffer,
	    overhead), measured with timer queries read a few frames later, so
	    measuring never makes the CPU wait for the GPU.
	  * A texture viewer: any texture a pass can read (the scene, its depth,
	    the G-buffer, the shadow maps, the pack's buffers and their mipmap
	    levels), one channel or all, with a range of values to map to black
	    and white, in the menu or over the whole screen.
	  * A probe: the exact values of the texture under the mouse (or under
	    the crosshair while playing), read back asynchronously.
	  * The `prisma` values of the last frame.
*/
#include "core/hooks/gl_defs.hpp"
#include "core/tracking/state.hpp"

#include <string>
#include <vector>

namespace prisma::debugview {

// ---- GPU timings

// Measuring is on while something shows the timings (the menu page or the HUD).
void set_profiling(bool on);
bool profiling();
// Brackets of one measured stage; not nested. Cheap no-ops while not profiling.
void time_begin(const std::string &name);
void time_end();
// Once per frame, before anything is measured: gathers finished results.
void frame_begin();
// After the last measured stage of the frame (with Prisma's GL state saved).
void frame_end();

struct Timing {
	std::string name;
	double ms = 0; // smoothed over the last frames
};
const std::vector<Timing> &timings(); // in the order they ran
bool timings_supported();
// Measured with glFinish (no timer queries): the game is slower while measuring.
bool timings_slow_down();

// ---- texture viewer

// Its fragment shader (with passglsl::VERTEX), complete with #version
extern const char *const VIEWER_FS;

enum Kind {
	KIND_COLOR,       // shown as stored
	KIND_DEPTH,       // the scene's depth: shown as distance in nodes
	KIND_DEPTH_MAP,   // shadow and overhead maps: depth 0..1 (already linear)
	KIND_MATERIAL,    // G-buffer material ids: a colour per id
};

struct Source {
	std::string name;
	GLuint texture = 0; // 0: not available this frame
	Kind kind = KIND_COLOR;
	int width = 0, height = 0;
	int levels = 1; // mipmap levels
};

struct Settings {
	std::string source = "Escena (colorTex)";
	int channel = 0;     // 0 RGB, 1 R, 2 G, 3 B, 4 A
	float lo = 0, hi = 1; // values mapped to black and white (depth: nodes)
	bool auto_range = true; // next frame: lo, hi and lod as fit the source's kind
	int lod = 0;
	bool fullscreen = false; // over the game, also with the menu closed
	bool hud = false;        // timings over the game with the menu closed
};

// The viewer is on while the menu page shows it, or in full screen.
void set_viewer_open(bool open);
Settings &settings();
// Where to read the probe, 0..1 of the texture (y up); < 0: the crosshair.
void set_probe(float u, float v);

// From pipeline::render, with Prisma's GL state saved: draws the selected
// source into the view texture and queues the probe.
void render(const std::vector<Source> &sources, const GameState &state);
bool viewer_wanted();

// For the menu
const std::vector<Source> &sources(); // as of the last frame
GLuint view_texture();                // 0 if nothing was drawn
int view_width();
int view_height();
struct Probe {
	bool valid = false;
	float raw[4] = {};    // the texel as stored
	float distance = -1;  // KIND_DEPTH: nodes from the camera
	int x = 0, y = 0;     // texel
	std::string source;
};
const Probe &probe();

// The state of the last frame, as the passes received it
void set_state(const GameState &state);
const GameState *state();

// The overlay should draw (full-screen view or HUD) even with the menu closed.
bool wants_overlay();
// After the overlay: what render() drew is shown; the next frame draws anew
// (or nothing, if the pack doesn't run).
void end_frame();

} // namespace prisma::debugview
