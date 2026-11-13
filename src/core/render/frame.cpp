// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/render/frame.hpp"
#include "common/ini.hpp"
#include "core/audio/audio.hpp"
#include "core/dev/hookcost.hpp"
#include "core/ui/debugview.hpp"
#include "core/render/gameshaders.hpp"
#include "core/render/lighting.hpp"
#include "core/render/geometry.hpp"
#include "core/hooks/glapi.hpp"
#include "core/ui/input.hpp"
#include "common/log.hpp"
#include "core/ui/overlay.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/screen.hpp"
#include "core/hooks/sdl.hpp"
#include "core/tracking/tracker.hpp"
#include "core/tracking/compat.hpp"
#include "core/tracking/state.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace prisma::frame {

static double g_blink_seconds = 0; // 0 = off
static bool g_blink_off = false;
static bool g_open_menu_in_world = false; // [debug] open_menu
static double g_capture_at = 0;           // [debug] capture = N: seconds; 0 = off
static std::filesystem::path g_capture_file;

static unsigned long long g_frame = 0;
static bool g_world_drawn = false; // this frame
static GLuint g_world_fbo = 0;
static GLuint g_world_program = 0;
static GLuint g_last_world_program = 0; // from an earlier frame
static bool g_done = false;
static bool g_sky_captured = false; // this frame (see before_draw)

// Game state, built once per frame the first time someone needs it: a game
// shader that uses the Prisma API, or the passes at the end of the scene.
static GameState g_state;
static bool g_state_valid = false;
static unsigned long long g_state_frame = ~0ull;
static int g_width = 0, g_height = 0;

static bool g_state_from_this_frame = false;
static bool g_scene_readable = false; // this frame's scene had colour and depth textures (compat.cpp)

// Looking at the open sky Luanti draws no map block, but the scene still has
// to be finished (sky, clouds...). The scene framebuffer is remembered from
// earlier frames, and the camera comes from the sky (see run()).
static GLuint g_scene_fbo = 0;          // from the last frame with world
static GLuint g_resolved_fbo = 0;       // this frame: where a multisampled scene was resolved
static double g_scene_seen = -100;       // when world geometry was last drawn
static bool g_scene_touched = false;     // something was drawn into it this frame
static bool g_window_touched = false;    // something was drawn into framebuffer 0 this frame

// Luanti's tint while the camera is in a liquid. ClientMap::renderPostFx
// (MapPostFxStep) draws the node's post_effect_color over the whole screen
// with Irrlicht's untextured 2D renderer, on the screen after the wielded
// item and before the HUD, with or without post-processing (which belongs to
// the 3D stage). (The damage flash is drawn the same way but after the HUD,
// so it is not mistaken for it; menus are told apart by their colour.)
static bool g_hide_underwater = false;
static bool g_hud_started = false;          // a textured 2D draw after the world, this frame
static bool g_liquid_seen = false;          // the tint was drawn this frame
static float g_liquid_frame[4] = {0, 0, 0, 0};
static float g_liquid_last[4] = {0, 0, 0, 0}; // last frame's
// Luanti's pause menu darkens the world with a fullscreen rectangle of
// formspec_fullscreen_bg_color, pure black by default (see skip_overlay)
static bool g_pause_seen = false;

// The tint comes after the passes: they use the one from the previous frame.
static void apply_liquid(GameState &s)
{
	const float *c = g_liquid_seen ? g_liquid_frame : g_liquid_last;
	std::memcpy(s.liquid_color, c, sizeof(s.liquid_color));
}

static bool scene_active()
{
	return g_world_drawn || (g_scene_touched && g_scene_fbo != 0 && uptime() - g_scene_seen < 30.0);
}

static const GameState *state()
{
	// Built from last frame's camera if needed before the world is drawn
	// (e.g. the sky), then once more as soon as this frame's camera is known.
	if (g_state_frame == g_frame && (g_state_from_this_frame || !g_world_drawn))
		return g_state_valid ? &g_state : nullptr;
	GLuint program = g_world_drawn ? g_world_program : g_last_world_program;
	// No block drawn in this world yet (mapgen singlenode: nothing but air),
	// or the last world program belonged to a world since left: the sky's
	// camera stands in, once the sky has been drawn this frame.
	float wv[16], wvp[16];
	bool sky = !g_world_drawn && geometry::sky_camera(wv, wvp);
	if (!program && !sky)
		return nullptr;
	g_state_frame = g_frame;
	g_state_from_this_frame = g_world_drawn;
	g_state_valid = (program && build_state(program, g_width, g_height, g_state)) ||
		(sky && build_state(0, g_width, g_height, g_state));
	apply_liquid(g_state);
	// Game shaders draw before this frame's shadow maps: they get last frame's
	lighting::previous_shadow(g_state);
	return g_state_valid ? &g_state : nullptr;
}

static void run()
{
	hookcost::PassScope cost;
	g_done = true;
	gameshaders::invalidate_shadow_maps();
	geometry::on_scene_end();
	pipeline::Scene scene;
	scene.fbo = screen::active() ? 0 : g_resolved_fbo ? g_resolved_fbo : g_world_drawn ? g_world_fbo : g_scene_fbo;
	if (scene.fbo == 0 && screen::active()) {
		// Drawn "to the window", really into Prisma's stand-in
		scene.color = screen::color();
		scene.depth = screen::depth();
	} else if (scene.fbo != 0) {
		scene.color = tracker::fbo_texture(scene.fbo, GL_COLOR_ATTACHMENT0);
		scene.depth = tracker::fbo_texture(scene.fbo, GL_DEPTH_ATTACHMENT);
		if (!scene.depth.texture)
			scene.depth = tracker::fbo_texture(scene.fbo, GL_DEPTH_STENCIL_ATTACHMENT);
	}
	g_scene_readable = scene.color.texture != 0 && scene.depth.texture != 0 && scene.color.width > 0;
	if (!state())
		return;
	if (!g_world_drawn) {
		// No world this frame: the camera matrices Luanti sent are stale.
		float wv[16], wvp[16];
		if (!geometry::sky_camera(wv, wvp))
			return;
		Vec3 camera, offset;
		bool known = sky_camera_position(camera, offset);
		apply_sky_camera(g_state, wv, wvp, known ? &camera : nullptr);
		if (known)
			g_state.camera_offset = offset;
	}
	apply_liquid(g_state);
	pipeline::render(scene, g_state);
}

void init(const std::filesystem::path &ini)
{
	// [debug] blink = N: switch the pack off and on every N seconds, to
	// compare with and without it.
	g_blink_seconds = ini::get_int(ini, "debug", "blink", 0);
	if (g_blink_seconds > 0)
		log::info("Blink mode: the pack toggles every %.0f s", g_blink_seconds);
	g_open_menu_in_world = ini::get_int(ini, "debug", "open_menu", 0) != 0;
	// [debug] capture = N: N seconds after the world appears, the frame as
	// shown (with Prisma's menu) goes to logs/capture.ppm. For unattended
	// tests where the window can't be captured from outside.
	g_capture_at = ini::get_int(ini, "debug", "capture", 0);
	g_capture_file = ini.parent_path() / "logs" / "capture.ppm";
	// [debug] viewer = <texture>: the texture viewer in full screen from the
	// start (the name as the menu lists it); timings = 1: the GPU timings
	// over the game. For unattended tests.
	std::string name = ini::get(ini, "debug", "viewer");
	name = name.substr(0, name.find(';')); // a comment after the value
	name.erase(name.find_last_not_of(" \t") + 1);
	if (!name.empty()) {
		debugview::settings().source = name;
		debugview::settings().fullscreen = true;
	}
	debugview::settings().hud = ini::get_int(ini, "debug", "timings", 0) != 0;
	// [debug] start_off = 1: the pack starts switched off, as after its key
	if (ini::get_int(ini, "debug", "start_off", 0) != 0)
		pipeline::set_enabled(false);
}

void before_bind_framebuffer(GLenum target, GLuint fbo)
{
	if (target == GL_READ_FRAMEBUFFER || g_done || !scene_active())
		return;
	GLuint scene = g_world_drawn ? g_world_fbo : g_scene_fbo;
	// A multisampled scene can't be read: wait until Luanti resolves it
	if (scene != 0 && tracker::draw_fbo() == scene && fbo != scene && !tracker::fbo_multisampled(scene))
		run();
}

void after_blit(GLuint read_fbo, GLuint draw_fbo, GLbitfield mask)
{
	if (g_done || draw_fbo == 0 || !(mask & GL_COLOR_BUFFER_BIT) || !scene_active())
		return;
	GLuint scene = g_world_drawn ? g_world_fbo : g_scene_fbo;
	if (read_fbo != scene || !tracker::fbo_multisampled(scene))
		return;
	g_resolved_fbo = draw_fbo;
	tracker::SurfaceInfo color = tracker::fbo_texture(draw_fbo, GL_COLOR_ATTACHMENT0);
	if (color.width > 0) {
		g_width = color.width;
		g_height = color.height;
	}
	run();
}

void set_hide_underwater(bool hide)
{
	g_hide_underwater = hide;
}

bool skip_overlay(GLint first, GLsizei count, GLenum index_type, const void *indices)
{
	constexpr GLenum GL_UNSIGNED_BYTE_ = 0x1401, GL_UNSIGNED_SHORT_ = 0x1403, GL_UNSIGNED_INT_ = 0x1405;
	constexpr GLenum GL_FLOAT_ = 0x1406, GL_VIEWPORT_ = 0x0BA2;
	if (!g_world_drawn || !tracker::current_is_2d())
		return false;
	if (std::strcmp(tracker::current_info().name, "Irrlicht/Renderer2D_noTex") != 0) {
		g_hud_started = true;
		return false;
	}
	// After the HUD only the pause menu is looked for (below)
	// Irrlicht's 2D vertices live in client memory: position in pixels, colour as BGRA bytes
	const tracker::VertexAttrib &pos = tracker::attrib(0), &col = tracker::attrib(2);
	if (pos.buffer || !pos.offset || pos.type != GL_FLOAT_ || pos.size < 2 || col.buffer || !col.offset ||
			col.type != GL_UNSIGNED_BYTE_ || col.size != 4 || count < 3 || count > 6 ||
			(indices && tracker::element_buffer()))
		return false;
	GLsizei pos_stride = pos.stride ? pos.stride : pos.size * 4;
	GLsizei col_stride = col.stride ? col.stride : 4;
	float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
	GLint first_vertex = first;
	for (GLsizei i = 0; i < count; i++) {
		GLint v = first + i;
		if (indices) {
			if (index_type == GL_UNSIGNED_SHORT_)
				v = static_cast<const unsigned short *>(indices)[i];
			else if (index_type == GL_UNSIGNED_INT_)
				v = static_cast<GLint>(static_cast<const unsigned *>(indices)[i]);
			else if (index_type == GL_UNSIGNED_BYTE_)
				v = static_cast<const unsigned char *>(indices)[i];
			else
				return false;
		}
		if (i == 0)
			first_vertex = v;
		const float *p = reinterpret_cast<const float *>(pos.offset + static_cast<uintptr_t>(v) * pos_stride);
		for (int k = 0; k < 2; k++) {
			lo[k] = std::min(lo[k], p[k]);
			hi[k] = std::max(hi[k], p[k]);
		}
	}
	GLint vp[4] = {0, 0, 0, 0};
	gl::GetIntegerv(GL_VIEWPORT_, vp);
	if (lo[0] > 1.f || lo[1] > 1.f || hi[0] < vp[2] - 1.f || hi[1] < vp[3] - 1.f)
		return false;
	const unsigned char *c =
		reinterpret_cast<const unsigned char *>(col.offset + static_cast<uintptr_t>(first_vertex) * col_stride);
	// Menus (pause, inventory...) darken the screen with the same kind of
	// rectangle, and the HUD is not drawn under them to tell them apart: their
	// background is black (formspec_fullscreen_bg_color) or nearly so
	// (Minetest Game's inventory: #080808). No liquid tints the view black.
	// Pure black is the pause menu (inventories use their own colour, e.g.
	// Minetest Game's #080808): the pack's sounds stop under it. It may come
	// before or after the HUD.
	if (c[3] != 0 && c[0] == 0 && c[1] == 0 && c[2] == 0)
		g_pause_seen = true;
	if (g_hud_started || c[3] == 0 || std::max({c[0], c[1], c[2]}) < 13)
		return false;
	g_liquid_frame[0] = c[2] / 255.f;
	g_liquid_frame[1] = c[1] / 255.f;
	g_liquid_frame[2] = c[0] / 255.f;
	g_liquid_frame[3] = c[3] / 255.f;
	g_liquid_seen = true;
	static bool logged = false;
	if (!logged) {
		logged = true;
		log::info("Camera inside a liquid: Luanti tints the screen with (%.2f, %.2f, %.2f, %.2f)%s",
			g_liquid_frame[0], g_liquid_frame[1], g_liquid_frame[2], g_liquid_frame[3],
			g_hide_underwater ? "; the pack hides it" : "");
	}
	return g_hide_underwater;
}

void on_framebuffer_deleted(GLuint fbo)
{
	// Leaving the world deletes the scene framebuffer; its number may come back.
	if (fbo != 0 && fbo == g_scene_fbo)
		g_scene_fbo = 0;
}

void before_clear(GLbitfield mask)
{
	// Without post-processing the wielded item is drawn into the same
	// framebuffer as the world, after clearing the depth: the world is
	// complete right now, and its depth is about to go.
	if ((mask & GL_DEPTH_BUFFER_BIT) && !g_done && g_window_touched && screen::active() && tracker::draw_fbo() == 0)
		run();
}

void before_draw()
{
	// The sky (Irrlicht's fixed programs and the stars) is drawn first; the
	// first other draw into the scene marks its end: keep a copy of it.
	if (!g_sky_captured && !g_done && !tracker::current_is_2d()) {
		const char *name = tracker::current_info().name;
		bool sky = std::strncmp(name, "Irrlicht/", 9) == 0 || std::strcmp(name, "stars_shader") == 0;
		GLuint fbo = tracker::draw_fbo();
		bool scene = fbo != 0 ? fbo == g_scene_fbo || fbo == g_world_fbo : screen::active();
		if (!sky && scene) {
			g_sky_captured = true;
			GLint vp[4] = {0, 0, 0, 0};
			gl::GetIntegerv(0x0BA2 /* GL_VIEWPORT */, vp);
			pipeline::capture_sky(vp[0], vp[1], vp[2], vp[3]);
		}
	}
	if (g_scene_fbo != 0 && tracker::draw_fbo() == g_scene_fbo)
		g_scene_touched = true;
	if (tracker::draw_fbo() == 0)
		g_window_touched = true;
	if (tracker::current_is_world()) {
		if (!g_world_drawn) {
			g_world_drawn = true;
			g_world_fbo = tracker::draw_fbo();
			screen::note_world(g_world_fbo);
			// The scene size, for viewSize in game shaders
			// (drawn "to the window": Prisma's stand-in screen has that size)
			tracker::SurfaceInfo color = g_world_fbo == 0 && screen::active() ? screen::color() :
				tracker::fbo_texture(g_world_fbo, GL_COLOR_ATTACHMENT0);
			if (color.width > 0) {
				g_width = color.width;
				g_height = color.height;
			}
		}
		g_world_program = tracker::current_program();
	} else if (!g_done && (g_world_drawn ? g_world_fbo == 0 : screen::active()) && tracker::current_is_2d()) {
		// Without post-processing everything goes straight to the screen; the
		// HUD starting is the best boundary we have.
		run();
	}

	GLuint program = tracker::current_program();
	if (gameshaders::uses_api(program))
		if (const GameState *s = state())
			gameshaders::before_draw(program, *s, g_frame);
}

// The window's back buffer, as it is about to be shown, into a PPM file
static void capture_frame(void *window)
{
	int w = 0, h = 0;
	if (!sdl::drawable_size(window, w, h) || !gl::load())
		return;
	std::vector<unsigned char> pixels((size_t)w * h * 3);
	GLint read_fbo = 0, pack = 0;
	gl::GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	gl::GetIntegerv(0x0D05 /* GL_PACK_ALIGNMENT */, &pack);
	gl::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	gl::ReadBuffer(0x0405 /* GL_BACK */);
	gl::PixelStorei(0x0D05, 1);
	gl::ReadPixels(0, 0, w, h, 0x1907 /* GL_RGB */, GL_UNSIGNED_BYTE, pixels.data());
	gl::PixelStorei(0x0D05, pack);
	gl::BindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
	if (std::FILE *f = open_file(g_capture_file, "wb")) {
		std::fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (int y = h - 1; y >= 0; y--) // GL's rows go bottom up
			std::fwrite(pixels.data() + (size_t)y * w * 3, 1, (size_t)w * 3, f);
		std::fclose(f);
		log::info("Frame capture saved to %s", to_utf8(g_capture_file).c_str());
	}
}

void on_swap(void *window)
{
	if (scene_active() && !g_done && (g_world_drawn ? g_world_fbo : g_scene_fbo) != 0)
		run();
	else if (g_world_drawn && !g_done)
		run();

	if (g_state_frame == g_frame && g_state_valid)
		end_state_frame(g_state);
	compat::on_frame(g_world_drawn, g_scene_readable, g_state_frame == g_frame && g_state_valid ? &g_state : nullptr);
	g_scene_readable = false;
	// For measuring startup times; the tracker logs when the
	// world appears
	static bool first_frame = true;
	if (first_frame) {
		first_frame = false;
		log::info("First frame at %.2f s", uptime());
	}
	if (g_world_drawn) {
		g_last_world_program = g_world_program;
		g_scene_seen = uptime();
		if (g_world_fbo != 0)
			g_scene_fbo = g_world_fbo;
		// Where Luanti's camera offset stands, to follow it through frames
		// that show only sky
		if (g_state_valid && g_state_from_this_frame)
			track_world_camera(g_state);
	}
	g_scene_touched = false;
	g_window_touched = false;
	g_resolved_fbo = 0;
	if (g_liquid_seen)
		std::memcpy(g_liquid_last, g_liquid_frame, sizeof(g_liquid_last));
	else
		std::fill(std::begin(g_liquid_last), std::end(g_liquid_last), 0.f);
	g_liquid_seen = false;
	g_hud_started = false;
	// Only while the world is drawn: without it the sounds stop on their own
	audio::set_paused(g_world_drawn && g_pause_seen);
	g_pause_seen = false;
	if (g_world_drawn && g_open_menu_in_world) {
		g_open_menu_in_world = false;
		input::set_menu_open(true);
	}
	g_frame++;

	// Largest change of the light direction between two frames: the sun
	// should move smoothly, any jump shows up here.
	static Vec3 last_light;
	static bool have_light = false;
	static float max_step = 0;
	if (g_state_valid && g_state.sun_available) {
		if (have_light) {
			Vec3 a = last_light, b = g_state.light_dir;
			// atan2 of |a x b| and a . b: acos loses tiny angles to float rounding
			Vec3 x{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
			float angle = std::atan2(x.length(), a.x * b.x + a.y * b.y + a.z * b.z);
			max_step = std::max(max_step, angle * 57.29578f);
		}
		last_light = g_state.light_dir;
		have_light = true;
	}

	// Frame rate in the log every few seconds: shows what a pack costs.
	static double fps_start = uptime();
	static int fps_frames = 0;
	fps_frames++;
	double now = uptime();
	if (now - fps_start >= 5.0) {
		if (g_state_valid) {
			// Forward direction of the camera: third row of the view rotation
			const Mat4 &v = g_state.view;
			log::info("%.1f FPS  camera (%.1f, %.1f, %.1f) looking at (%.2f, %.2f, %.2f)  sun (%.3f, %.3f, %.3f), "
				"max jump %.4f°", fps_frames / (now - fps_start), g_state.camera_pos.x, g_state.camera_pos.y,
				g_state.camera_pos.z, v.at(2, 0), v.at(2, 1), v.at(2, 2), g_state.light_dir.x, g_state.light_dir.y,
				g_state.light_dir.z, max_step);
			max_step = 0;
		} else {
			log::info("%.1f FPS", fps_frames / (now - fps_start));
		}
		std::string top = hookcost::report_top(fps_frames, 6);
		std::string cost = hookcost::report(fps_frames);
		if (!cost.empty()) {
			log::info("  %s", cost.c_str());
			log::info("  %s", top.c_str());
		}
		if (debugview::profiling() && !debugview::timings().empty()) {
			std::string gpu;
			for (const debugview::Timing &t : debugview::timings())
				gpu += (gpu.empty() ? "" : ", ") + t.name + " " + std::to_string(t.ms).substr(0, 5) + " ms";
			log::info("  GPU: %s", gpu.c_str());
		}
		fps_start = now;
		fps_frames = 0;
	}

	if (g_blink_seconds > 0) {
		bool off = int(uptime() / g_blink_seconds) % 2 == 1;
		if (off != g_blink_off) {
			g_blink_off = off;
			pipeline::set_bypass(off);
			log::info("Blink: pack %s", off ? "off" : "on");
		}
	}
	// Only sky this frame. On an empty map (mapgen singlenode) Luanti never
	// draws a block, and above an island the open sky can fill the view for
	// long: the sky stands in for the world, so the pack keeps running.
	GLuint sky_fbo = 0;
	if (!g_world_drawn && geometry::sky_framebuffer(sky_fbo)) {
		g_scene_seen = uptime();
		tracker::SurfaceInfo color;
		if (sky_fbo != 0) {
			g_scene_fbo = sky_fbo;
			color = tracker::fbo_texture(sky_fbo, GL_COLOR_ATTACHMENT0);
		} else {
			screen::note_world(0);
			color = screen::color();
		}
		if (color.width > 0) {
			g_width = color.width;
			g_height = color.height;
		}
	}
	g_world_drawn = false;
	g_done = false;
	g_sky_captured = false;
	geometry::on_frame_end();
	pipeline::on_frame_end();
	screen::end_frame(window);
	overlay::render(window);
	static double world_since = -1;
	if (g_capture_at > 0 && g_state_valid && world_since < 0)
		world_since = uptime();
	if (g_capture_at > 0 && world_since >= 0 && uptime() - world_since >= g_capture_at) {
		g_capture_at = 0;
		capture_frame(window);
	}
}

const GameState *last_state()
{
	return g_state_valid ? &g_state : nullptr;
}

} // namespace prisma::frame
