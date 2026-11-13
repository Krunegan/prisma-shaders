// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Records the world geometry Luanti draws, so Prisma can draw it again from
	other points of view (from the sun, for its own shadow map) or into other
	targets (its G-buffer), without touching Luanti's shaders.

	Map blocks live in vertex buffers that persist across frames, so a block
	stays recorded while its buffers are unchanged: blocks behind the camera
	still cast shadows. Entities are kept only for the frame in which they
	were drawn (they move, and several of them can share one mesh).

	Also watches the sky: the sun and the moon are drawn as small quads around
	the camera, which gives their direction (and the time of day) even when
	Luanti's dynamic shadows, and with them the uniforms that carry the sun,
	are off.
*/
#include "core/hooks/gl_defs.hpp"
#include "core/render/math.hpp"
#include "core/tracking/tracker.hpp"

#include <vector>

namespace prisma::geometry {

struct Draw {
	bool object = false;        // entity (object_shader) or map block (nodes_shader)
	bool array_texture = false; // texture on unit 0 is a GL_TEXTURE_2D_ARRAY
	bool transparent = false;   // blended material (water, stained glass...)
	bool liquid = false;        // water and other liquids (lets the light through)
	int material = 0;           // MATERIAL_* below: what the G-buffer says it is
	GLuint texture = 0;         // unit 0; 0 = no alpha test possible
	GLuint vbo = 0, ibo = 0;
	unsigned long long vbo_generation = 0, ibo_generation = 0;
	GLenum index_type = 0;
	GLsizei count = 0;
	uintptr_t index_offset = 0;
	GLint base_vertex = 0;
	// Luanti's attributes: 0 position, 1 normal, 2 colour, 3 texture layer, 4 uv.
	// Disabled ones are absent.
	tracker::VertexAttrib position, normal, color, layer, uv;
	// Animated meshes (GPU skinning): 8 weights, 9 joint ids, and a copy of
	// the joint matrices as they were for this draw, in joint_buffer().
	bool skinned = false;
	GLuint skin_vbo = 0;
	unsigned long long skin_generation = 0;
	tracker::VertexAttrib weights, joint_ids;
	GLintptr joint_offset = 0;
	tracker::Culling culling; // as the game drew it
	DMat4 world; // local position -> absolute world position, in nodes
	bool has_bounds = false;
	float bounds_min[3] = {}, bounds_max[3] = {}; // local
	unsigned long long frame = 0; // last frame in which the game drew it
};

// Material ids of gbufferMaterialTex (PRISMA_MAT_* in passglsl.cpp)
enum Material {
	MATERIAL_NONE = 0,
	MATERIAL_SOLID = 1,
	MATERIAL_LEAVES = 2,
	MATERIAL_PLANTS = 3,
	MATERIAL_LIQUID = 4,
	MATERIAL_LIQUID_OPAQUE = 5,
	MATERIAL_ENTITY = 6,
};

// From the draw hooks, before the call reaches the driver.
void on_draw(GLenum mode, GLsizei count, GLenum index_type, const void *indices, GLint base_vertex,
	GLsizei instances);
void on_clear(GLbitfield mask);
// The scene is complete: nothing drawn after this belongs to the world.
void on_scene_end();
void on_frame_end();

// The pack asked to hide Luanti's own clouds (it draws its own).
// Whether to record the world's draws, which only Prisma's shadow maps,
// G-buffer and overhead map replay (on_draw keeps reading the sky and the
// entities' positions either way)
void set_record_draws(bool record);
void set_hide_clouds(bool hide);
// True if the draw being issued must not reach the screen.
bool skip_draw();

// The pack asked to hide Luanti's dynamic shadows (it makes its own): its
// shaders get f_shadow_strength = 0, which skips their whole shadow code.
void set_hide_shadows(bool hide);
// Same for Luanti's volumetric light (volumetricLightStrength = 0).
void set_hide_volumetric_light(bool hide);
// After Prisma relinks game programs: send the overridden values again.
void reapply_overrides();
// From the glUniform1f(v) hooks: the value to really send.
float filter_uniform(GLint location, float value);

// Every recorded draw whose buffers are still intact. With
// `this_frame_only`, just what the game drew in the current frame.
void collect(std::vector<const Draw *> &out, bool this_frame_only);
unsigned long long frame();

// Prisma's uniform buffer with the joint matrices copied from skinned draws
// (std140 block `JointMatrices { mat4 joints[1024]; }`, JOINT_SLOT bytes each).
GLuint joint_buffer();
constexpr GLsizeiptr JOINT_SLOT = 1024 * 64;

// Sun from the sky quads. False if none has been seen lately.
struct Sky {
	Vec3 sun_dir;        // towards the sun, world axes
	float time_of_day;   // 0..1, like Luanti's (0.5 = noon)
};
bool sky(Sky &out);

// Seconds since Luanti last drew its sun or moon; negative if it never has.
double sky_age();

// The camera as the sky saw it this frame (Irrlicht's uWVMatrix and
// uWVPMatrix of the sun/moon quads), for frames without any world geometry:
// looking at the open sky, Luanti draws no map block at all.
bool sky_camera(float wv[16], float wvp[16]);

// Whether the sky's camera was seen this frame, and the framebuffer the sky
// was drawn into (0 = the window). On an empty map (mapgen singlenode) the
// sky is all the world there is.
bool sky_framebuffer(GLuint &fbo);

// The camera's position this frame in render space (nodes, relative to
// Luanti's camera offset), from an entity drawn with the sky's view: the
// player's own model, usually. False if none was.
bool object_camera(Vec3 &render_pos);

} // namespace prisma::geometry
