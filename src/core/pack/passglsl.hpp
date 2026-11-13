// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	GLSL shared by every pass of a pack: the fullscreen vertex shader and the
	header put before each pass (API, options, inputs and helpers). Pure
	strings, so the pack checker (prisma_check) builds exactly the same code.
	Keep in sync with docs/shaderpacks.md.
*/
#include "core/pack/pack.hpp"

#include <string>
#include <vector>

namespace prisma::passglsl {

extern const char *const VERTEX;

// Everything before the pass's own code. Each of the pack's buffers is a
// sampler2D with its own name. `compute`: the context runs compute shaders
// (PRISMA_COMPUTE, so a .frag can tell).
std::string fragment_header(bool depth_zero_to_one, const std::vector<pack::Buffer> &buffers, bool compute);

// The same for a compute pass (.comp, GLSL 4.30): no texcoord input nor
// fragColor output, but prismaImage (its target buffer, read and written in
// place), prismaPixel and a texcoord macro.
std::string compute_header(bool depth_zero_to_one, const std::vector<pack::Buffer> &buffers, const pack::Pass &pass);

// The same declarations as a file for code editors (prisma_check --stub):
// all inside #ifndef PRISMA, so including it changes nothing in the game.
// Uses the options as options::set_pack() left them.
std::string editor_stub(const pack::Pack &pack);

} // namespace prisma::passglsl
