// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	What the core knows of the game's OpenGL state, recorded from the hooks:

	  * every shader program, which of Luanti's shader files it came from,
	    and the uniforms it asks for, with the last value the game sent;
	  * framebuffers and their attachments, bound textures, vertex arrays
	    and buffers, culling: what Prisma needs to replay or extend the game's
	    drawing.

	The developer tools that describe these records (frame traces, shader
	dumps) are in core/dev/trace.hpp. All entry points are called from the
	hooks on the GL thread.
*/
#include "core/hooks/gl_defs.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace prisma::tracker {

enum class UKind { Float, Int, Matrix };

struct UniformValue {
	UKind kind = UKind::Float;
	int components = 1; // per element: 1..4, or 4/9/16 for matrices
	GLsizei count = 1;
	std::vector<float> f;
	std::vector<int> i;
	unsigned long long frame = 0; // frame in which it was last set
};

// `shaders_dir`: Luanti's client/shaders, to recognise its shader files.
void init(const std::filesystem::path &shaders_dir);

void on_proc_request(const char *name, bool found);
void on_frame_end();

void on_create_shader(GLuint shader, GLenum type);
void on_shader_source(GLuint shader, const std::string &source); // as compiled (after patching)
void on_attach_shader(GLuint program, GLuint shader);
void on_link_program(GLuint program);
void on_delete_program(GLuint program);
void on_use_program(GLuint program);
void on_get_uniform_location(GLuint program, const GLchar *name, GLint location);
void on_uniform(GLint location, UKind kind, int components, GLsizei count, const void *data);

void on_bind_framebuffer(GLenum target, GLuint fbo);
void on_delete_framebuffer(GLuint fbo);
void on_delete_texture(GLuint texture);
void on_framebuffer_texture(GLenum target, GLenum attachment, GLuint texture, GLint level, bool multisample = false);
void on_framebuffer_renderbuffer(GLenum target, GLenum attachment, GLuint renderbuffer);
void on_bind_renderbuffer(GLuint renderbuffer);
void on_renderbuffer_storage(GLenum internal_format, GLsizei width, GLsizei height, GLsizei samples);
void on_blit_framebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
	GLbitfield mask);
void on_draw_buffers(GLsizei n, const GLenum *bufs);

void on_active_texture(GLenum unit);
void on_bind_texture(GLenum target, GLuint texture);
void on_tex_image(GLenum target, GLint level, GLenum internal_format, GLsizei width, GLsizei height);

// `index_type`/`indices`: as passed to glDrawElements (0/null for arrays)
void on_draw(const char *kind, GLsizei count, GLsizei instances, GLenum index_type = 0,
	const void *indices = nullptr);

// Vertex data: which buffers and layouts each draw uses
void on_bind_buffer(GLenum target, GLuint buffer);
void on_bind_vertex_array(GLuint vao);
void on_vertex_attrib_pointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
	const void *pointer, bool integer = false);
void on_enable_vertex_attrib(GLuint index, bool enabled);
// `data` may be null. `partial`: glBufferSubData (and mapping for writing).
void on_buffer_data(GLenum target, GLsizeiptr size, const void *data, GLenum usage, bool partial);
void on_delete_buffer(GLuint buffer);
void on_clear(GLbitfield mask);
void on_enable(GLenum cap, bool enabled);
void on_cull_face(GLenum mode);
void on_front_face(GLenum mode);
void on_viewport(GLint x, GLint y, GLsizei width, GLsizei height);

// ---- queries used by the rest of the core

struct SurfaceInfo {
	GLuint texture = 0; // 0 if nothing (or a renderbuffer) is attached
	GLenum format = 0;
	GLsizei width = 0, height = 0;
};

GLenum shader_type(GLuint shader);
GLuint draw_fbo();
GLuint read_fbo();
// Its colour is a multisampled texture: to be resolved (blitted) before it
// can be read, as Luanti does with antialiasing = fsaa
bool fbo_multisampled(GLuint fbo);
GLuint current_program();
bool current_is_world(); // a nodes_shader variant: world geometry
bool current_is_2d();    // Irrlicht's 2D renderer: GUI and HUD
SurfaceInfo fbo_texture(GLuint fbo, GLenum attachment);

// Copies the last value sent for uniform `name` of `program` (ints are
// converted). Returns how many floats were copied, 0 if it was never sent.
int uniform(GLuint program, const char *name, float *out, int max);
// Same, but the last value sent under that name to any program.
int global_uniform(const char *name, float *out, int max);
// Same, but only to programs built from Luanti's shader `program_name`.
int named_uniform(const char *program_name, const char *name, float *out, int max);
// A program built from Luanti's shader `name` was linked at some point.
bool has_program(const char *name);

// ---- vertex data of the draw being issued (for Prisma's own replays)

struct VertexAttrib {
	bool enabled = false;
	bool integer = false; // glVertexAttribIPointer
	bool normalized = false;
	GLuint buffer = 0; // 0: client memory, `offset` is then a pointer
	GLint size = 0;
	GLenum type = 0;
	GLsizei stride = 0;
	uintptr_t offset = 0;
};

const VertexAttrib &attrib(unsigned index); // of the bound vertex array
GLuint element_buffer();                    // of the bound vertex array

// Changes every time the buffer's contents may change (upload, mapping,
// deletion). 0 = unknown buffer. Lets a recorded draw tell whether the data
// it pointed at is still there.
unsigned long long buffer_generation(GLuint buffer);
// Bounding box of the positions, if the buffer was uploaded whole with
// Luanti's usual vertex layout (40-byte vertices, position first).
bool buffer_bounds(GLuint buffer, float min[3], float max[3]);

GLuint bound_texture(unsigned unit, GLenum target); // GL_TEXTURE_2D or GL_TEXTURE_2D_ARRAY

// Face culling as the game left it (Irrlicht sets it per material)
struct Culling {
	bool enabled = false;
	GLenum face = 0x0405;  // GL_BACK
	GLenum front = 0x0901; // GL_CCW
};
Culling culling();

// What Prisma needs to know about the program in use to redraw its geometry.
struct ProgramInfo {
	const char *name = "";   // Luanti shader, e.g. "nodes_shader"
	bool world = false;      // nodes_shader or object_shader
	bool array_texture = false; // USE_ARRAY_TEXTURE
	bool skinned = false;    // USE_SKINNING (joints in a uniform buffer)
	int material_type = -1;  // MATERIAL_TYPE
	const float *world_matrix = nullptr; // last mWorld sent, if any
};
ProgramInfo current_info();

// Location (as the game sees it) of Luanti's f_shadow_strength in the
// current program, -1 if it has none.
GLint current_shadow_strength_location();
// Same for volumetric_light's volumetricLightStrength.
GLint current_volumetric_strength_location();
// Every program that has uniform `name`, with the game's location and the
// last value sent (null if never sent).
void for_each_program_with(const char *name,
	void (*fn)(void *ctx, GLuint program, GLint location, const UniformValue *value), void *ctx);

// Raw access to the same data. `location` is the one the game used.
const UniformValue *uniform_value(GLuint program, GLint location);
// Null until the uniform is first sent; from then on the same pointer, with
// the latest value.
const UniformValue *global_uniform_value(const std::string &name);

// The number of the frame being drawn (UniformValue::frame says when a value
// was last set).
unsigned long long current_frame();

} // namespace prisma::tracker
