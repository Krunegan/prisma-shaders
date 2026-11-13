// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	The tracker's own records, shared only with the developer tools that
	describe them (core/dev/trace.cpp). The rest of the core uses the queries
	in tracker.hpp.
*/
#include "core/tracking/shader_catalog.hpp"
#include "core/tracking/tracker.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace prisma::tracker::detail {

struct Shader {
	GLenum type = 0;
	std::string source;
	catalog::Match match;
	bool identified = false;
};

// A uniform the game asked the location of. The two copies of its value
// that the queries read by name are found the first time it is sent, so
// sending it again allocates nothing.
struct UniformName {
	std::string name;
	UniformValue *global = nullptr; // in the values by name
	UniformValue *named = nullptr;  // in the values by "<program name>|<name>"
};

struct Program {
	GLuint id = 0;
	std::vector<GLuint> shaders;
	std::string name = "?";
	std::map<GLint, UniformName> uniform_names;
	// Last value ever sent. Luanti only re-sends a uniform when it changes,
	// so most of them are not set every frame.
	std::map<GLint, UniformValue> values;
	bool used_this_frame = false;
	bool uniforms_reported = false;
	bool is_world = false; // draws world geometry: marks that we are in game
	bool is_2d = false;
	bool geometry = false; // nodes_shader or object_shader
	bool array_texture = false, skinned = false;
	int material_type = -1;
	GLint world_location = -1;               // of mWorld
	GLint shadow_strength_location = -1;     // of f_shadow_strength
	GLint volumetric_strength_location = -1; // of volumetricLightStrength
};

struct Surface {
	GLenum format = 0;
	GLsizei width = 0, height = 0, samples = 0;
};

struct Attachment {
	bool renderbuffer = false;
	GLuint id = 0;
	bool multisample = false; // a GL_TEXTURE_2D_MULTISAMPLE texture
};

struct VertexArray {
	GLuint element_buffer = 0;
	VertexAttrib attribs[16];
};

struct BufferInfo {
	GLsizeiptr size = 0;
	GLenum usage = 0;
	unsigned long long generation = 0;
	bool has_bounds = false;
	float min[3], max[3];
};

extern std::unordered_map<GLuint, Shader> g_shaders;
extern std::unordered_map<GLuint, Program> g_programs;
extern std::unordered_map<GLuint, Surface> g_textures, g_renderbuffers;
extern std::unordered_map<GLuint, std::map<GLenum, Attachment>> g_fbos;
extern std::unordered_map<GLuint, VertexArray> g_vaos; // 0 = the default one
extern std::unordered_map<GLuint, BufferInfo> g_buffers;
extern Program *g_current;
extern GLuint g_draw_fbo, g_read_fbo, g_vao;
extern unsigned long long g_frame;

} // namespace prisma::tracker::detail
