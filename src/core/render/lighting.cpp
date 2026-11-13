// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/render/lighting.hpp"
#include "core/ui/debugview.hpp"
#include "core/render/geometry.hpp"
#include "core/hooks/glapi.hpp"
#include "core/render/lighting_glsl.hpp"
#include "common/log.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace prisma::lighting {

// ---------------------------------------------------------------- shaders

struct Program {
	GLuint id = 0;
	GLint mvp = -1, normal_matrix = -1, texture = -1, alpha_ref = -1, has_normal = -1, has_color = -1, material = -1;
};

// Variants: + ARRAY_BIT for array textures, + SKINNED_BIT for animated meshes
enum { ARRAY_BIT = 1, SKINNED_BIT = 2, CASTER = 0, GBUFFER = 4, PROGRAM_COUNT = 8 };
constexpr GLuint k_joint_binding = 15; // uniform block binding point for the copied poses
static Program g_programs[PROGRAM_COUNT];
static bool g_programs_tried = false, g_programs_ok = false;

static GLuint compile(GLenum type, const std::string &source)
{
	GLuint sh = gl::CreateShader(type);
	const char *src = source.c_str();
	gl::ShaderSource(sh, 1, &src, nullptr);
	gl::CompileShader(sh);
	GLint ok = 0;
	gl::GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char info[2048] = "";
		gl::GetShaderInfoLog(sh, sizeof info, nullptr, info);
		log::error("Prisma: an internal lighting shader does not compile: %s", info);
		gl::DeleteShader(sh);
		return 0;
	}
	return sh;
}

static bool build(Program &p, const char *vs, const char *fs, int variant)
{
	std::string header = std::string("#version 150\n") + (variant & ARRAY_BIT ? "#define ARRAY 1\n" : "") +
		(variant & SKINNED_BIT ? "#define SKINNED 1\n" : "");
	GLuint v = compile(GL_VERTEX_SHADER, header + vs);
	GLuint f = compile(GL_FRAGMENT_SHADER, header + fs);
	if (!v || !f) {
		if (v)
			gl::DeleteShader(v);
		if (f)
			gl::DeleteShader(f);
		return false;
	}
	p.id = gl::CreateProgram();
	gl::AttachShader(p.id, v);
	gl::AttachShader(p.id, f);
	// Luanti's attribute numbers, so the recorded layouts apply as they are
	gl::BindAttribLocation(p.id, 0, "inPosition");
	gl::BindAttribLocation(p.id, 1, "inNormal");
	gl::BindAttribLocation(p.id, 2, "inColor");
	gl::BindAttribLocation(p.id, 3, "inLayer");
	gl::BindAttribLocation(p.id, 4, "inTexCoord");
	gl::BindAttribLocation(p.id, 8, "inWeights");
	gl::BindAttribLocation(p.id, 9, "inJointIDs");
	// The G-buffer's three targets (the casters write none)
	gl::BindFragDataLocation(p.id, 0, "fragData");
	gl::BindFragDataLocation(p.id, 1, "fragLight");
	gl::BindFragDataLocation(p.id, 2, "fragMaterial");
	gl::LinkProgram(p.id);
	gl::DeleteShader(v);
	gl::DeleteShader(f);
	GLint ok = 0;
	gl::GetProgramiv(p.id, GL_LINK_STATUS, &ok);
	if (!ok) {
		char info[2048] = "";
		gl::GetProgramInfoLog(p.id, sizeof info, nullptr, info);
		log::error("Prisma: an internal lighting shader does not link: %s", info);
		gl::DeleteProgram(p.id);
		p.id = 0;
		return false;
	}
	p.mvp = gl::GetUniformLocation(p.id, "uMVP");
	p.normal_matrix = gl::GetUniformLocation(p.id, "uNormalMatrix");
	p.texture = gl::GetUniformLocation(p.id, "uTexture");
	p.alpha_ref = gl::GetUniformLocation(p.id, "uAlphaRef");
	p.has_normal = gl::GetUniformLocation(p.id, "uHasNormal");
	p.has_color = gl::GetUniformLocation(p.id, "uHasColor");
	p.material = gl::GetUniformLocation(p.id, "uMaterial");
	GLuint block = gl::GetUniformBlockIndex(p.id, "JointMatrices");
	if (block != GL_INVALID_INDEX)
		gl::UniformBlockBinding(p.id, block, k_joint_binding);
	return true;
}

static bool programs()
{
	if (g_programs_tried)
		return g_programs_ok;
	g_programs_tried = true;
	g_programs_ok = true;
	for (int v = 0; v < 4; v++) {
		g_programs_ok &= build(g_programs[CASTER + v], glsl::CASTER_VS, glsl::CASTER_FS, v);
		g_programs_ok &= build(g_programs[GBUFFER + v], glsl::GBUFFER_VS, glsl::GBUFFER_FS, v);
	}
	return g_programs_ok;
}

// ---------------------------------------------------------------- targets

static GLuint g_vao = 0;
// Per slot: the terrain alone (g_terrain_*, redrawn now and then) and the
// map the passes read (g_shadow_*: that terrain plus this frame's entities)
static GLuint g_shadow_fbo[2] = {}, g_shadow_tex[2] = {};
static GLuint g_terrain_fbo[2] = {}, g_terrain_tex[2] = {};
static int g_slot_prev = 0, g_slot_latest = 1;
static int g_shadow_size = 0;
static GLuint g_gbuffer_fbo = 0, g_gbuffer_tex = 0, g_gbuffer_light_tex = 0, g_gbuffer_material_tex = 0;
static GLuint g_gbuffer_depth = 0;
static int g_gbuffer_w = 0, g_gbuffer_h = 0;
static GLuint g_overhead_fbo = 0, g_overhead_tex = 0;
static int g_overhead_size = 0;
static bool g_shadow_drawn = false, g_gbuffer_drawn = false, g_overhead_drawn = false;
// The shadow fields of the last maps drawn, for previous_shadow()
static GameState g_last_shadow;

static void texture_params(GLenum filter)
{
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// What each map was drawn with: its light space and the camera position at
// that moment (maps are not redrawn every frame, see draw_shadow_maps).
struct MapView {
	DMat4 lv;            // player space (of `cam`) -> light clip space
	double cam[3] = {};
	bool valid = false;
};
static MapView g_map_view[2];

// Two maps: the sun's previous and latest direction (see draw_shadow_maps),
// each with its terrain-only copy.
static bool shadow_target(int size)
{
	if (g_shadow_tex[0] && g_shadow_size == size)
		return true;
	if (!g_shadow_fbo[0]) {
		gl::GenFramebuffers(2, g_shadow_fbo);
		gl::GenFramebuffers(2, g_terrain_fbo);
	}
	if (g_shadow_tex[0]) {
		gl::DeleteTextures(2, g_shadow_tex);
		gl::DeleteTextures(2, g_terrain_tex);
	}
	gl::GenTextures(2, g_shadow_tex);
	gl::GenTextures(2, g_terrain_tex);
	for (int i = 0; i < 4; i++) {
		GLuint fbo = i < 2 ? g_shadow_fbo[i] : g_terrain_fbo[i - 2];
		GLuint tex = i < 2 ? g_shadow_tex[i] : g_terrain_tex[i - 2];
		gl::ActiveTexture(GL_TEXTURE0);
		gl::BindTexture(GL_TEXTURE_2D, tex);
		gl::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,
			nullptr);
		texture_params(GL_NEAREST);
		gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		// Both bindings: glReadBuffer changes the *read* framebuffer, which
		// would otherwise be whatever the game (or the stand-in screen) had.
		gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
		gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
		gl::DrawBuffer(GL_NONE);
		gl::ReadBuffer(GL_NONE);
		if (gl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			log::error("Prisma: the shadow map framebuffer is incomplete");
			gl::DeleteTextures(2, g_shadow_tex);
			gl::DeleteTextures(2, g_terrain_tex);
			g_shadow_tex[0] = g_shadow_tex[1] = g_terrain_tex[0] = g_terrain_tex[1] = 0;
			g_shadow_size = 0;
			return false;
		}
	}
	g_shadow_size = size;
	g_map_view[0].valid = g_map_view[1].valid = false;
	log::info("Own shadow maps: 2 of %dx%d (and their terrain separately)", size, size);
	return true;
}

// The overhead map: the world seen from straight above (see draw_overhead)
static MapView g_overhead_view;

static bool overhead_target(int size)
{
	if (g_overhead_tex && g_overhead_size == size)
		return true;
	if (!g_overhead_fbo)
		gl::GenFramebuffers(1, &g_overhead_fbo);
	if (g_overhead_tex)
		gl::DeleteTextures(1, &g_overhead_tex);
	gl::GenTextures(1, &g_overhead_tex);
	gl::ActiveTexture(GL_TEXTURE0);
	gl::BindTexture(GL_TEXTURE_2D, g_overhead_tex);
	gl::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
	texture_params(GL_NEAREST);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	gl::BindFramebuffer(GL_FRAMEBUFFER, g_overhead_fbo);
	gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_overhead_tex, 0);
	gl::DrawBuffer(GL_NONE);
	gl::ReadBuffer(GL_NONE);
	if (gl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		log::error("Prisma: the overhead map framebuffer is incomplete");
		gl::DeleteTextures(1, &g_overhead_tex);
		g_overhead_tex = 0;
		g_overhead_size = 0;
		return false;
	}
	g_overhead_size = size;
	g_overhead_view.valid = false;
	log::info("Prisma's overhead map: %dx%d", size, size);
	return true;
}

// Three targets: gbufferTex (normal, share of sunlight), gbufferLightTex
// (light banks, liquid surfaces) and gbufferMaterialTex (what each pixel
// is), see lighting_glsl.hpp.
static bool gbuffer_target(int w, int h)
{
	if (g_gbuffer_tex && g_gbuffer_w == w && g_gbuffer_h == h)
		return true;
	if (!g_gbuffer_fbo)
		gl::GenFramebuffers(1, &g_gbuffer_fbo);
	if (!g_gbuffer_depth)
		gl::GenRenderbuffers(1, &g_gbuffer_depth);
	if (g_gbuffer_tex)
		gl::DeleteTextures(1, &g_gbuffer_tex);
	if (g_gbuffer_light_tex)
		gl::DeleteTextures(1, &g_gbuffer_light_tex);
	if (g_gbuffer_material_tex)
		gl::DeleteTextures(1, &g_gbuffer_material_tex);
	gl::GenTextures(1, &g_gbuffer_tex);
	gl::GenTextures(1, &g_gbuffer_light_tex);
	gl::GenTextures(1, &g_gbuffer_material_tex);
	gl::ActiveTexture(GL_TEXTURE0);
	for (GLuint tex : {g_gbuffer_tex, g_gbuffer_light_tex, g_gbuffer_material_tex}) {
		bool material = tex == g_gbuffer_material_tex;
		gl::BindTexture(GL_TEXTURE_2D, tex);
		gl::TexImage2D(GL_TEXTURE_2D, 0, material ? GL_R8 : GL_RGBA8, w, h, 0, material ? GL_RED : GL_RGBA,
			GL_UNSIGNED_BYTE, nullptr);
		texture_params(GL_NEAREST);
	}
	gl::BindRenderbuffer(GL_RENDERBUFFER, g_gbuffer_depth);
	gl::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gbuffer_fbo);
	gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gbuffer_tex, 0);
	gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_gbuffer_light_tex, 0);
	gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, g_gbuffer_material_tex, 0);
	gl::FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_gbuffer_depth);
	const GLenum all[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
	gl::DrawBuffers(3, all);
	if (gl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		log::error("Prisma: the G-buffer framebuffer is incomplete");
		gl::DeleteTextures(1, &g_gbuffer_tex);
		gl::DeleteTextures(1, &g_gbuffer_light_tex);
		gl::DeleteTextures(1, &g_gbuffer_material_tex);
		g_gbuffer_tex = g_gbuffer_light_tex = g_gbuffer_material_tex = 0;
		return false;
	}
	g_gbuffer_w = w;
	g_gbuffer_h = h;
	return true;
}

// ---------------------------------------------------------------- replay

static std::vector<const geometry::Draw *> g_draws;

// Draws the recorded geometry with program `base` (+ the draw's variant).
// `matrix(d, mvp, normal3x3)` returns false to skip a draw.
template <class MatrixFn>
static unsigned replay(int base, float alpha_ref, bool use_normals, MatrixFn matrix)
{
	// Only the camera view culls faces like the game did (otherwise the
	// inside of the player's own head shows up); casters keep both sides.
	const bool cull = use_normals;
	// Fewer state changes: group by texture kind, texture and buffer
	std::sort(g_draws.begin(), g_draws.end(), [](const geometry::Draw *a, const geometry::Draw *b) {
		if (a->array_texture != b->array_texture)
			return a->array_texture < b->array_texture;
		if (a->skinned != b->skinned)
			return a->skinned < b->skinned;
		if (a->texture != b->texture)
			return a->texture < b->texture;
		return a->vbo < b->vbo;
	});

	gl::BindVertexArray(g_vao);
	gl::ActiveTexture(GL_TEXTURE0);
	const Program *current = nullptr;
	GLuint texture = ~0u, vbo = 0, ibo = 0;
	bool array = false;
	unsigned drawn = 0;
	tracker::Culling culling;
	culling.enabled = false;
	culling.face = culling.front = 0;
	gl::Disable(GL_CULL_FACE);
	float mvp[16], normal[9];
	auto attrib = [](GLuint index, const tracker::VertexAttrib &a) {
		if (!a.enabled) {
			gl::DisableVertexAttribArray(index);
			return;
		}
		gl::EnableVertexAttribArray(index);
		if (a.integer)
			gl::VertexAttribIPointer(index, a.size, a.type, a.stride, reinterpret_cast<const void *>(a.offset));
		else
			gl::VertexAttribPointer(index, a.size, a.type, a.normalized, a.stride,
				reinterpret_cast<const void *>(a.offset));
	};

	for (const geometry::Draw *d : g_draws) {
		if (!matrix(*d, mvp, normal))
			continue;
		const Program &p = g_programs[base + (d->array_texture ? ARRAY_BIT : 0) + (d->skinned ? SKINNED_BIT : 0)];
		if (&p != current) {
			current = &p;
			gl::UseProgram(p.id);
			if (p.texture >= 0)
				gl::Uniform1i(p.texture, 0);
			if (p.alpha_ref >= 0)
				gl::Uniform1f(p.alpha_ref, alpha_ref);
			texture = ~0u;
			vbo = 0; // attribute 3 differs between variants
		}
		if (d->texture != texture || d->array_texture != array) {
			// Unbind the other kind so a texture-less draw samples nothing
			gl::BindTexture(d->array_texture ? GL_TEXTURE_2D : GL_TEXTURE_2D_ARRAY, 0);
			gl::BindTexture(d->array_texture ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D, d->texture);
			texture = d->texture;
			array = d->array_texture;
		}
		if (cull && (d->culling.enabled != culling.enabled || d->culling.face != culling.face ||
				d->culling.front != culling.front)) {
			culling = d->culling;
			culling.enabled ? gl::Enable(GL_CULL_FACE) : gl::Disable(GL_CULL_FACE);
			gl::CullFace(culling.face);
			gl::FrontFace(culling.front);
		}
		if (d->vbo != vbo) {
			vbo = d->vbo;
			gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
		}
		// Layouts can differ between draws of one buffer: always set them.
		attrib(0, d->position);
		attrib(4, d->uv);
		attrib(3, d->array_texture ? d->layer : tracker::VertexAttrib{});
		if (use_normals) {
			attrib(1, d->normal);
			attrib(2, d->color);
			gl::Uniform1i(p.has_normal, d->normal.enabled ? 1 : 0);
			gl::Uniform1i(p.has_color, d->color.enabled ? 1 : 0);
			gl::Uniform1i(p.material, d->material);
			gl::UniformMatrix3fv(p.normal_matrix, 1, 0, normal);
		}
		if (d->skinned) {
			gl::BindBuffer(GL_ARRAY_BUFFER, d->skin_vbo);
			attrib(8, d->weights);
			attrib(9, d->joint_ids);
			vbo = d->skin_vbo;
			gl::BindBufferRange(GL_UNIFORM_BUFFER, k_joint_binding, geometry::joint_buffer(), d->joint_offset,
				geometry::JOINT_SLOT);
		}
		if (d->ibo != ibo) {
			ibo = d->ibo;
			gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
		}
		gl::UniformMatrix4fv(p.mvp, 1, 0, mvp);
		const void *indices = reinterpret_cast<const void *>(d->index_offset);
		if (d->base_vertex)
			gl::DrawElementsBaseVertex(GL_TRIANGLES, d->count, d->index_type, indices, d->base_vertex);
		else
			gl::DrawElements(GL_TRIANGLES, d->count, d->index_type, indices);
		drawn++;
	}
	for (GLuint i = 0; i < 10; i++)
		gl::DisableVertexAttribArray(i);
	gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); // part of our VAO only
	return drawn;
}

static void to_float(const DMat4 &m, float out[16])
{
	for (int i = 0; i < 16; i++)
		out[i] = (float)m.m[i];
}

static void normal_matrix(const DMat4 &w, float out[9])
{
	for (int c = 0; c < 3; c++)
		for (int r = 0; r < 3; r++)
			out[c * 3 + r] = (float)w.at(r, c);
}

// Is the box (local bounds of `d`, through `m` into clip space with w = 1)
// entirely outside the unit cube?
static bool outside(const geometry::Draw &d, const DMat4 &m, double margin)
{
	if (!d.has_bounds) {
		// Map blocks are at most a few mesh chunks wide; entities are small.
		const double origin[3] = {0, 0, 0};
		double o[3];
		m.apply(origin, o);
		for (int k = 0; k < 3; k++)
			if (o[k] < -1 - margin || o[k] > 1 + margin)
				return true;
		return false;
	}
	double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
	for (int i = 0; i < 8; i++) {
		double c[3] = {(i & 1) ? d.bounds_max[0] : d.bounds_min[0], (i & 2) ? d.bounds_max[1] : d.bounds_min[1],
			(i & 4) ? d.bounds_max[2] : d.bounds_min[2]};
		double o[3];
		m.apply(c, o);
		for (int k = 0; k < 3; k++) {
			lo[k] = std::min(lo[k], o[k]);
			hi[k] = std::max(hi[k], o[k]);
		}
	}
	for (int k = 0; k < 3; k++)
		if (hi[k] < -1 || lo[k] > 1)
			return true;
	return false;
}

// ---------------------------------------------------------------- shadow map

static double angle_between(const Vec3 &a, const Vec3 &b)
{
	Vec3 x{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
	return std::atan2(x.length(), a.x * b.x + a.y * b.y + a.z * b.z);
}


// Player space of the camera now -> [0, 1] texture coordinates and depth of map `m`.
static Mat4 map_matrix(const MapView &m, const GameState &s)
{
	DMat4 bias = DMat4::scale(0.5);
	bias.at(0, 3) = bias.at(1, 3) = bias.at(2, 3) = 0.5;
	DMat4 shift = DMat4::translation(s.camera_pos.x - m.cam[0], s.camera_pos.y - m.cam[1], s.camera_pos.z - m.cam[2]);
	return (bias * m.lv * shift).to_float();
}

// Nodes between depth 0 and depth 1 of a map reaching `R` nodes from its centre
static double depth_range(double R)
{
	return 2.0 * std::max(2.0 * R, 256.0);
}

// Draws g_draws into the depth map of `fbo` as seen by `view` (`resolution`
// texels, `R` nodes from the centre to the edge), over what it holds unless
// `clear`. Returns how many draws fell in it.
static unsigned draw_into(GLuint fbo, int resolution, double R, const MapView &view, bool clear)
{
	// Absolute world -> light clip space
	DMat4 world_to_light = view.lv * DMat4::translation(-view.cam[0], -view.cam[1], -view.cam[2]);

	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	gl::Viewport(0, 0, resolution, resolution);
	gl::Enable(GL_DEPTH_TEST);
	gl::DepthFunc(GL_LESS);
	gl::DepthMask(1);
	if (clear) {
		gl::ClearDepth(1.0);
		gl::Clear(GL_DEPTH_BUFFER_BIT);
	}
	const double margin = 48.0 / R;
	return replay(CASTER, 0.70f, false, [&](const geometry::Draw &d, float *mvp, float *) {
		DMat4 m = world_to_light * d.world;
		if (outside(d, m, margin))
			return false;
		to_float(m, mvp);
		return true;
	});
}

// Draws g_draws into the depth map of `fbo` (`resolution` texels, `R`
// nodes from the centre to the edge) looking along -dir, and records how
// in `view`. Returns how many draws fell in it.
static unsigned draw_depth_map(GLuint fbo, int resolution, double R, const Vec3 &dir, const GameState &s,
	MapView &view)
{
	const double Dz = depth_range(R) / 2; // casters far above still count
	const double texel = 2.0 * R / resolution;

	// Light space: looking along -dir.
	double f[3] = {-dir.x, -dir.y, -dir.z};
	double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	for (double &v : f)
		v /= fl;
	double hint[3] = {0, 1, 0};
	if (std::fabs(f[1]) > 0.9) {
		hint[1] = 0;
		hint[2] = 1;
	}
	double rt[3] = {hint[1] * f[2] - hint[2] * f[1], hint[2] * f[0] - hint[0] * f[2], hint[0] * f[1] - hint[1] * f[0]};
	double rl = std::sqrt(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
	for (double &v : rt)
		v /= rl;
	double up[3] = {f[1] * rt[2] - f[2] * rt[1], f[2] * rt[0] - f[0] * rt[2], f[0] * rt[1] - f[1] * rt[0]};

	// Keep the texel grid fixed to the world, so shadows don't crawl as the
	// camera moves. It is anchored to a point near the camera, not to the
	// world origin, which can be thousands of nodes away.
	const double cam[3] = {s.camera_pos.x, s.camera_pos.y, s.camera_pos.z};
	const double cell = 32.0;
	double rel[3];
	for (int k = 0; k < 3; k++)
		rel[k] = cam[k] - std::floor(cam[k] / cell) * cell;
	double cx = rt[0] * rel[0] + rt[1] * rel[1] + rt[2] * rel[2];
	double cy = up[0] * rel[0] + up[1] * rel[1] + up[2] * rel[2];
	double tx = cx - std::floor(cx / texel) * texel;
	double ty = cy - std::floor(cy / texel) * texel;

	// Player space (origin at the camera) -> light clip space
	DMat4 lv;
	for (int c = 0; c < 3; c++) {
		lv.at(0, c) = rt[c] / R;
		lv.at(1, c) = up[c] / R;
		lv.at(2, c) = f[c] / Dz;
	}
	lv.at(0, 3) = tx / R;
	lv.at(1, 3) = ty / R;
	lv.at(2, 3) = 0;
	view.lv = lv;
	for (int k = 0; k < 3; k++)
		view.cam[k] = cam[k];
	view.valid = true;
	return draw_into(fbo, resolution, R, view, true);
}

// A rectangle of texels of a map, [x0, x1) x [y0, y1)
struct TexelRect {
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool empty() const { return x0 >= x1 || y0 >= y1; }
	TexelRect join(const TexelRect &o) const
	{
		if (empty())
			return o;
		if (o.empty())
			return *this;
		return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
	}
};

// The texels of the map seen by `view` that the draws in g_draws (the
// entities) can cover. Without bounds (animated meshes: the pose moves
// their vertices) a generous box around the entity's origin.
static TexelRect entity_rect(const MapView &view, int resolution, double R)
{
	if (g_draws.empty() || !view.valid)
		return {};
	const DMat4 world_to_light = view.lv * DMat4::translation(-view.cam[0], -view.cam[1], -view.cam[2]);
	const double unbounded = 8.0 / R; // nodes, in clip units
	double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
	auto add = [&](const double o[3], double pad) {
		for (int k = 0; k < 2; k++) {
			lo[k] = std::min(lo[k], o[k] - pad);
			hi[k] = std::max(hi[k], o[k] + pad);
		}
	};
	for (const geometry::Draw *d : g_draws) {
		const DMat4 m = world_to_light * d->world;
		if (d->has_bounds) {
			for (int i = 0; i < 8; i++) {
				double c[3] = {(i & 1) ? d->bounds_max[0] : d->bounds_min[0],
					(i & 2) ? d->bounds_max[1] : d->bounds_min[1], (i & 4) ? d->bounds_max[2] : d->bounds_min[2]};
				double o[3];
				m.apply(c, o);
				add(o, 0);
			}
		} else {
			const double origin[3] = {0, 0, 0};
			double o[3];
			m.apply(origin, o);
			add(o, unbounded);
		}
	}
	// Clip space to texels, with a texel to spare on each side
	auto texel = [&](double v, bool up) {
		double t = (v * 0.5 + 0.5) * resolution;
		return std::clamp((int)(up ? std::ceil(t) + 1 : std::floor(t) - 1), 0, resolution);
	};
	return {texel(lo[0], false), texel(lo[1], false), texel(hi[0], true), texel(hi[1], true)};
}

/*
	Following the sun with a single map makes shadow edges shimmer: every
	time the map turns, each edge is rounded to the texel grid again, and
	that rounding can go back as well as forward (by about a texel), so an
	edge moving smoothly as a whole bobs up and down.

	Instead there are two maps that stay still: one at the sun's previous
	"key" direction (A) and one at the latest (B). The passes blend from A to
	B as the sun moves on; when it has moved one more step, A <- B and B <-
	the sun, exactly when the blend has reached B, so nothing jumps. The
	direction shown lags the sun by about one step (a fraction of a degree).

	The terrain of each map is drawn into a map of its own, and only now and
	then: when the keys advance (the old B map simply becomes A) and every 8
	frames. Entities can't wait a frame:
	moving fast, their shadow would trail behind. So every frame each map the
	passes read gets its terrain back where the entities were and then this
	frame's entities drawn on top (like Luanti, which keeps them in a map of
	their own every frame and merges it with its slower terrain map). Only
	the texels around the entities are copied, not the whole map.
*/
static void draw_shadow_maps(const Request &r, GameState &s)
{
	if (!(s.sun_available || r.hidden_light) || !shadow_target(r.resolution))
		return;
	const double R = r.distance;
	const double texel = 2.0 * R / r.resolution;
	// One step moves a shadow cast from 16 nodes up by about one texel.
	const double step = std::clamp(texel / 16.0, 0.05 / 57.29578, 1.0 / 57.29578);

	static Vec3 key[2]; // A, B
	static bool have_keys = false;
	static int slot[2] = {0, 1}; // which texture holds A and B
	static unsigned turn = 0;
	static int resolution = 0;
	bool redraw[2] = {false, false};
	// The body itself, not s.light_dir: before this, previous_shadow() set that
	// to the direction last frame's maps showed, so the keys never moved and
	// the sun stayed where it was when the game started
	const Vec3 L = s.light_body;
	float len = L.length();
	if (!(len > 0.5f) || !std::isfinite(len))
		return; // no usable light this frame
	double ahead = have_keys ? angle_between(key[1], L) : 1e9;
	if (!std::isfinite(ahead))
		ahead = 1e9;
	if (ahead > step * 8 || resolution != r.resolution) { // first time, sun/moon swap, /time, new size
		key[0] = key[1] = L;
		ahead = 0;
		redraw[0] = redraw[1] = true;
	} else if (ahead >= step) {
		key[0] = key[1];
		key[1] = L;
		std::swap(slot[0], slot[1]); // the old B map is the new A
		ahead = 0;
		redraw[1] = true;
	}
	have_keys = true;
	resolution = r.resolution;
	// The terrain stands still: each map is redrawn every 8 frames (as
	// Luanti's shadow_update_frames), enough to follow the camera and the
	// nodes that are dug or placed. They take turns, so a frame redraws one
	// at most.
	static unsigned since = 0;
	if (redraw[0] || redraw[1])
		since = 0;
	else if (++since >= 4) {
		redraw[turn++ & 1] = true;
		since = 0;
	}
	float blend = (float)std::clamp(ahead / step, 0.0, 1.0);

	geometry::collect(g_draws, false);
	static std::vector<const geometry::Draw *> entities;
	entities.clear();
	for (const geometry::Draw *d : g_draws)
		if (d->object)
			entities.push_back(d);
	// Water lets the sunlight through: with it in the map, everything under
	// a lake would be in its shadow (and the shadows of trees on the bottom lost).
	g_draws.erase(std::remove_if(g_draws.begin(), g_draws.end(),
					  [](const geometry::Draw *d) { return d->object || d->liquid; }),
		g_draws.end());
	// The terrain each final map was last copied from in full, and the
	// texels the entities drawn on it since then cover.
	static unsigned long long terrain_generation[2] = {1, 1}, copied[2] = {0, 0};
	static TexelRect stamped[2];
	unsigned drawn = 0;
	for (int k = 0; k < 2; k++) {
		int i = slot[k];
		if (redraw[k] || !g_map_view[i].valid) {
			drawn = draw_depth_map(g_terrain_fbo[i], r.resolution, R, key[k], s, g_map_view[i]);
			terrain_generation[i]++;
		}
	}
	const size_t terrain_draws = g_draws.size();
	g_draws.swap(entities);
	gl::DepthMask(1);
	for (int i = 0; i < 2; i++) {
		const TexelRect now = entity_rect(g_map_view[i], r.resolution, R);
		// Back to the bare terrain where the entities were and are now:
		// the whole map only after the terrain was redrawn.
		TexelRect restore = stamped[i].join(now);
		if (copied[i] != terrain_generation[i]) {
			restore = {0, 0, r.resolution, r.resolution};
			copied[i] = terrain_generation[i];
		}
		if (!restore.empty()) {
			gl::BindFramebuffer(GL_READ_FRAMEBUFFER, g_terrain_fbo[i]);
			gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_shadow_fbo[i]);
			gl::BlitFramebuffer(restore.x0, restore.y0, restore.x1, restore.y1, restore.x0, restore.y0, restore.x1,
				restore.y1, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
		}
		if (!now.empty())
			draw_into(g_shadow_fbo[i], r.resolution, R, g_map_view[i], false);
		stamped[i] = now;
	}
	g_slot_prev = slot[0];
	g_slot_latest = slot[1];
	s.shadow_matrix_prev = map_matrix(g_map_view[slot[0]], s);
	s.shadow_matrix = map_matrix(g_map_view[slot[1]], s);
	s.shadow_blend = blend;
	// The passes light the world from the direction the maps show.
	Vec3 shown = key[0] * (1.f - blend) + key[1] * blend;
	s.light_dir = shown.normalized();
	s.shadow_available = true;
	s.shadow_distance = (float)R;
	s.shadow_depth_range = (float)depth_range(R);
	s.shadow_size = r.resolution;
	g_shadow_drawn = true;
	g_last_shadow = s;

	static bool logged = false;
	if (!logged && drawn > 0) {
		logged = true;
		log::info("Own shadows: %u of %zu terrain draws fall in the shadow map, plus %zu entities",
			drawn, terrain_draws, g_draws.size());
	}
}

// ---------------------------------------------------------------- G-buffer

// Is any part of map block `d` within `distance` nodes of `cam`? Entities
// and blocks without bounds count as near.
static bool within(const geometry::Draw &d, const double cam[3], double distance)
{
	if (d.object || !d.has_bounds)
		return true;
	double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
	for (int i = 0; i < 8; i++) {
		double c[3] = {(i & 1) ? d.bounds_max[0] : d.bounds_min[0], (i & 2) ? d.bounds_max[1] : d.bounds_min[1],
			(i & 4) ? d.bounds_max[2] : d.bounds_min[2]};
		double o[3];
		d.world.apply(c, o);
		for (int k = 0; k < 3; k++) {
			lo[k] = std::min(lo[k], o[k]);
			hi[k] = std::max(hi[k], o[k]);
		}
	}
	double sq = 0;
	for (int k = 0; k < 3; k++) {
		double e = std::max({lo[k] - cam[k], 0.0, cam[k] - hi[k]});
		sq += e * e;
	}
	return sq <= distance * distance;
}

static void draw_gbuffer(const Request &r, GameState &s)
{
	if (s.width <= 0 || s.height <= 0 || !gbuffer_target(s.width, s.height))
		return;
	const double cam[3] = {s.camera_pos.x, s.camera_pos.y, s.camera_pos.z};
	DMat4 view_proj = DMat4::from(s.proj * s.view);
	DMat4 world_to_clip_rel = DMat4::translation(-cam[0], -cam[1], -cam[2]);

	geometry::collect(g_draws, true);
	// Blended surfaces (water, stained glass) don't hide what is behind them
	// in Luanti's depth buffer either. Liquids are only marked in
	// gbufferLightTex, afterwards.
	std::vector<const geometry::Draw *> liquids;
	for (const geometry::Draw *d : g_draws)
		if (d->liquid)
			liquids.push_back(d);
	g_draws.erase(std::remove_if(g_draws.begin(), g_draws.end(), [](const geometry::Draw *d) { return d->transparent; }),
		g_draws.end());
	// gbuffer_distance: the G-buffer costs as much as the map Luanti draws,
	// so a pack can leave out what is far (and hidden in its fog anyway).
	// Liquids are kept at any distance: they are few, and a pack finds
	// water by them.
	if (r.gbuffer_distance > 0)
		g_draws.erase(std::remove_if(g_draws.begin(), g_draws.end(),
						  [&](const geometry::Draw *d) { return !d->liquid && !within(*d, cam, r.gbuffer_distance); }),
			g_draws.end());

	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gbuffer_fbo);
	gl::Viewport(0, 0, s.width, s.height);
	gl::Enable(GL_DEPTH_TEST);
	gl::DepthFunc(GL_LESS);
	gl::DepthMask(1);
	gl::ColorMask(1, 1, 1, 1);
	gl::ClearDepth(1.0);
	// Each target with its own "nothing here"
	const GLenum first[3] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE};
	const GLenum others[3] = {GL_NONE, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
	const GLenum all[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
	gl::DrawBuffers(3, first);
	gl::ClearColor(0.5f, 0.5f, 0.5f, 1.f);
	gl::Clear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
	gl::DrawBuffers(3, others);
	gl::ClearColor(0.f, 0.f, 0.f, 0.f);
	gl::Clear(GL_COLOR_BUFFER_BIT);
	gl::DrawBuffers(3, all);
	auto matrix = [&](const geometry::Draw &d, float *mvp, float *normal) {
		// Camera-relative first (in double), then Luanti's projection
		DMat4 rel = world_to_clip_rel * d.world;
		to_float(view_proj * rel, mvp);
		normal_matrix(d.world, normal);
		return true;
	};
	replay(GBUFFER, 0.5f, true, matrix);

	// The liquid surfaces in front of that, in gbufferLightTex and
	// gbufferMaterialTex only: Luanti writes their depth, so a pass finds
	// them in depthTex too.
	if (!liquids.empty()) {
		g_draws.swap(liquids);
		gl::DepthMask(0);
		gl::ColorMaski(0, 0, 0, 0, 0);
		replay(GBUFFER, -1.f, true, matrix);
		gl::ColorMask(1, 1, 1, 1);
		gl::DepthMask(1);
	}
	g_gbuffer_drawn = true;
}

// ---------------------------------------------------------------- overhead map

/*
	The world seen from straight above: for every column around the camera,
	the depth of the highest thing in it (blocks, plants and leaves).
	Entities move and liquids wave (and the rain falls on them anyway), so
	they are left out. Rain, snow and puddles use it to know what is under
	a roof.

	It changes only when the world does, so it is redrawn when the camera
	has moved a quarter of its reach and, to catch dug and placed nodes,
	every 30 frames.
*/
static void draw_overhead(const Request &r, GameState &s)
{
	if (!overhead_target(r.overhead_resolution))
		return;
	const double R = r.overhead_distance;
	static unsigned age = 0;
	static double drawn_distance = 0;
	MapView &v = g_overhead_view;
	double dx = s.camera_pos.x - v.cam[0], dy = s.camera_pos.y - v.cam[1], dz = s.camera_pos.z - v.cam[2];
	bool redraw = !v.valid || drawn_distance != R || ++age >= 30 || std::sqrt(dx * dx + dz * dz) > R * 0.25 ||
		std::fabs(dy) > 32.0;
	if (redraw) {
		geometry::collect(g_draws, false);
		g_draws.erase(std::remove_if(g_draws.begin(), g_draws.end(),
						  [](const geometry::Draw *d) { return d->object || d->liquid; }),
			g_draws.end());
		draw_depth_map(g_overhead_fbo, r.overhead_resolution, R, Vec3{0, 1, 0}, s, v);
		drawn_distance = R;
		age = 0;
	}
	s.overhead_available = true;
	s.overhead_matrix = map_matrix(v, s);
	s.overhead_distance = (float)R;
	s.overhead_depth_range = (float)depth_range(R);
	s.overhead_size = r.overhead_resolution;
	g_overhead_drawn = true;
}

// ---------------------------------------------------------------- entry

void render(const Request &r, GameState &s)
{
	g_shadow_drawn = g_gbuffer_drawn = g_overhead_drawn = false;
	s.shadow_available = false;
	s.overhead_available = false;
	if (!r.shadows && !r.gbuffer && !r.overhead)
		return;
	if (!programs())
		return;
	if (!g_vao)
		gl::GenVertexArrays(1, &g_vao);
	gl::Disable(GL_BLEND);
	gl::Disable(GL_CULL_FACE);
	gl::Disable(GL_SCISSOR_TEST);
	gl::Disable(GL_STENCIL_TEST);
	if (r.shadows) {
		debugview::time_begin("Prisma: shadow maps");
		draw_shadow_maps(r, s);
		debugview::time_end();
	}
	if (r.overhead) {
		debugview::time_begin("Prisma: overhead map");
		draw_overhead(r, s);
		debugview::time_end();
	}
	if (r.gbuffer) {
		debugview::time_begin("Prisma: G-buffer");
		draw_gbuffer(r, s);
		debugview::time_end();
	}
}

void previous_shadow(GameState &s)
{
	if (!g_shadow_drawn)
		return;
	s.shadow_available = true;
	s.shadow_matrix_prev = map_matrix(g_map_view[g_slot_prev], s);
	s.shadow_matrix = map_matrix(g_map_view[g_slot_latest], s);
	s.shadow_blend = g_last_shadow.shadow_blend;
	s.light_dir = g_last_shadow.light_dir;
	s.shadow_distance = g_last_shadow.shadow_distance;
	s.shadow_depth_range = g_last_shadow.shadow_depth_range;
	s.shadow_size = g_last_shadow.shadow_size;
}

GLuint shadow_texture()
{
	return g_shadow_drawn ? g_shadow_tex[g_slot_latest] : 0;
}

GLuint shadow_texture_prev()
{
	return g_shadow_drawn ? g_shadow_tex[g_slot_prev] : 0;
}

GLuint gbuffer_texture()
{
	return g_gbuffer_drawn ? g_gbuffer_tex : 0;
}

GLuint gbuffer_light_texture()
{
	return g_gbuffer_drawn ? g_gbuffer_light_tex : 0;
}

GLuint gbuffer_material_texture()
{
	return g_gbuffer_drawn ? g_gbuffer_material_tex : 0;
}

GLuint overhead_texture()
{
	return g_overhead_drawn ? g_overhead_tex : 0;
}

} // namespace prisma::lighting
