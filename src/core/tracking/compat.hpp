// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	The compatibility report in the game: once the
	world has been on screen for a few seconds, checks that the running Luanti
	gives Prisma what the baseline of the supported version says it should
	(common/luanti_baseline.hpp): the programs and uniforms marked required, a
	sun direction, and a scene framebuffer the passes can read. The result goes
	to the log once; problems are logged as errors, so they also show in the
	menu's Errors page.

	What prisma_check --luanti checks in the files, this checks in the running
	game: the uniforms Luanti's C++ sends and the framebuffers it makes.
*/
#include "core/tracking/state.hpp"

namespace prisma::compat {

// Once per frame, before the swap. `world_drawn`: Luanti drew map blocks this
// frame. `scene_readable`: the scene the passes would read had colour and
// depth textures. `state`: this frame's game state, null if none was built
// (no pack uses it).
void on_frame(bool world_drawn, bool scene_readable, const GameState *state);

} // namespace prisma::compat
