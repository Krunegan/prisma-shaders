// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Values of the active pack's options.

	In GLSL each option is either a member of a uniform struct, `options.<id>`
	(changes apply instantly), or, with `define = true` and for every
	`choice`, a #define named after the id in upper case (changing it
	recompiles the pack). Values are saved in prisma.ini, section
	[options:<pack>].
*/
#include "core/hooks/gl_defs.hpp"
#include "core/pack/pack.hpp"

#include <string>
#include <vector>

namespace prisma::options {

// Loads the saved values for `pack` (falls back to the defaults).
void set_pack(const pack::Pack &pack, const std::filesystem::path &ini);

const std::vector<pack::OptionDef> &defs();
double value(size_t index);
// Clamps, stores and saves. Returns true if it changed a #define (the pack
// must be recompiled).
bool set(size_t index, double value);
void reset_all();

// Declarations to put in every shader of the pack.
const std::string &glsl();

struct Locations {
	std::vector<GLint> loc;
	void query(GLuint program); // Prisma's raw GL; needs a current context
};
// Uploads the uniform options. `program` must be the current one.
void upload(const Locations &l);

} // namespace prisma::options
