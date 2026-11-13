// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	The text of a Luanti shader with a pack's game/ changes applied (see
	gameshaders.hpp for the layout). Pure text, no GL, so the unit tests can
	check it against Luanti's real shader sources.
*/
#include "core/hooks/gl_defs.hpp"
#include "core/pack/pack.hpp"

#include <string>
#include <vector>

namespace prisma::compose {

struct Composed {
	bool modified = false; // false: compile Luanti's source as it is
	std::string code;
	std::vector<std::string> files; // source string names, for compiler logs
	std::string what;               // "extended", "replaced and patched"...
	std::vector<std::string> missing_anchors; // #prisma before/after lines not found in Luanti's file
};

// Luanti's source `luanti` of its shader `name` (stage `type`) with the
// changes of `pack` applied. With no pack (null), only what Prisma always
// needs: nodes_shader's vertex stage keeps its camera uniforms alive.
Composed compose(GLenum type, const std::string &name, const std::string &luanti, const pack::Pack *pack);

} // namespace prisma::compose
