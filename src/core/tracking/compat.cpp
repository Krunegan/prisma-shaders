// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/tracking/compat.hpp"
#include "common/log.hpp"
#include "common/luanti_baseline.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/screen.hpp"
#include "core/tracking/tracker.hpp"

#include <string>
#include <vector>

namespace prisma::compat {

// Frames with the world on screen before the report: long enough for every
// required uniform to have been sent at least once
static constexpr int k_world_frames = 300;
// A sky the game hides is only told apart from a missing sun after about
// 300 frames (state.cpp, SKY_GONE_FRAMES): the sun waits up to this long
static constexpr int k_sun_frames = 900;

static int g_world_frames = 0;
static bool g_done = false;
static bool g_scene_seen = false;   // a frame had a readable scene
static bool g_scene_checked = false; // a pack was active on a frame with the world
static bool g_sun_seen = false;
static bool g_sky_hidden = false; // the game hid the sky: no sun to find, in any Luanti
static bool g_state_seen = false;

static void report()
{
	const luanti::Baseline &b = luanti::baseline();
	std::vector<std::string> problems, missing_optional;
	if (!b.error.empty())
		problems.push_back("the embedded baseline cannot be read: " + b.error);

	int programs = 0;
	for (const luanti::Program &p : b.programs) {
		if (tracker::has_program(p.name.c_str()))
			programs++;
		else if (p.required)
			problems.push_back("no " + p.name + " program");
		else
			missing_optional.push_back(p.name);
	}
	int uniforms = 0;
	for (const luanti::Uniform &u : b.uniforms) {
		if (tracker::global_uniform_value(u.name))
			uniforms++;
		else if (u.required)
			problems.push_back("uniform " + u.name + " was never sent");
		else
			missing_optional.push_back(u.name);
	}
	// With a multisampled window the pack can't be applied, and screen.cpp
	// already said why: a player's setting, not a different Luanti
	if (screen::msaa_window())
		g_scene_checked = false;
	if (g_scene_checked && !g_scene_seen)
		problems.push_back("no scene framebuffer with colour and depth textures");
	if (g_state_seen && !g_sun_seen && !g_sky_hidden)
		problems.push_back("no sun direction (neither Luanti's dynamic shadows nor the sky)");

	log::info("Compatibility with Luanti %s: %d of %zu programs, %d of %zu uniforms, scene %s, sun %s",
		b.version.c_str(), programs, b.programs.size(), uniforms, b.uniforms.size(),
		screen::msaa_window() ? "not checked (multisampled window)" : !g_scene_checked ? "not checked (no pack)" :
		g_scene_seen ? "readable" : "NOT readable",
		!g_state_seen ? "not checked (no pack)" : g_sun_seen ? "found" :
		g_sky_hidden ? "not checked (the game hid the sky)" : "NOT found");
	if (!missing_optional.empty()) {
		std::string list;
		for (const std::string &n : missing_optional)
			list += (list.empty() ? "" : ", ") + n;
		log::info("  Not seen, which depends on Luanti's settings and the scene: %s", list.c_str());
	}
	if (!problems.empty()) {
		log::error("This Luanti does not behave like Luanti %s: shaders may look wrong. "
			"Run prisma_check --luanti on its folder.", b.version.c_str());
		for (const std::string &p : problems)
			log::error("  %s", p.c_str());
	}
}

void on_frame(bool world_drawn, bool scene_readable, const GameState *state)
{
	if (g_done || !world_drawn)
		return;
	if (pipeline::active()) {
		g_scene_checked = true;
		g_scene_seen |= scene_readable;
	}
	if (state) {
		g_state_seen = true;
		g_sun_seen |= state->sun_available;
		g_sky_hidden |= state->sky_hidden;
	}
	if (++g_world_frames < k_world_frames)
		return;
	if (g_state_seen && !g_sun_seen && !g_sky_hidden && g_world_frames < k_sun_frames)
		return;
	g_done = true;
	report();
}

} // namespace prisma::compat
