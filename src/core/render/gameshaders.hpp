// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Applies a pack's game/ folder to Luanti's own shaders.

	Each shader Luanti compiles is recognised (see shader_catalog), and if the
	pack replaces or extends it, the source is rewritten before compilation:

	    <Luanti's generated header>
	    <Prisma API block>                       only when the pack touches it
	    #define main prisma_luanti_main          only for extensions
	    <Luanti's file, or the pack's replacement>
	    #undef main
	    <the pack's extension>
	    void main() { prisma_luanti_main(); prisma_vertex()/prisma_fragment(...); }

	If the pack's version does not compile or link, the original is used and
	the error goes to prisma.log, so the game never breaks.

	Programs can be rebuilt while the game runs (hot reload, on/off key).
	Relinking may move uniform locations, which Luanti caches; so for these
	programs Luanti only ever sees stable "virtual" locations that Prisma
	translates to the real ones on every glUniform call.
*/
#include "core/hooks/gl_defs.hpp"
#include "core/pack/pack.hpp"
#include "core/tracking/state.hpp"

#include <string>

namespace prisma::gameshaders {

// What to apply to shaders compiled from now on. The pack must outlive its use.
void set_pack(const pack::Pack *pack, bool enabled);

// Re-applies the current pack to every program Luanti has already built.
void rebuild_all();

// ---- hook entry points

// Returns the source to compile instead, or "" to keep Luanti's.
std::string source_for(GLuint shader, GLenum type, const std::string &luanti_source);
// After glCompileShader. If our version failed, compiles Luanti's original
// instead and returns its source (for the tracker), otherwise "".
std::string after_compile(GLuint shader);
void on_attach(GLuint program, GLuint shader);
void after_link(GLuint program);
void on_delete_program(GLuint program);

// Location translation (virtual <-> real), see the comment above.
GLint on_get_uniform_location(GLuint program, const GLchar *name, GLint real);
GLint real_location(GLuint program, GLint location);
extern bool g_virtual_locations; // fast path: false until the first managed program

// Uploads the Prisma API uniforms once per frame for programs that use them.
// `program` must be the current one.
void before_draw(GLuint program, const GameState &state, unsigned long long frame);
// Prisma drew (the passes, the maps): the shadow maps are bound again for
// the game shaders that still draw this frame
void invalidate_shadow_maps();
bool uses_api(GLuint program);

} // namespace prisma::gameshaders
