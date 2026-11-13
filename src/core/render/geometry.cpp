// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/render/geometry.hpp"
#include "core/render/gameshaders.hpp"
#include "core/hooks/glapi.hpp"
#include "common/log.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace prisma::geometry {

static constexpr double BS = 10.0; // Luanti render units per node
constexpr GLenum GL_TRIANGLES_ = 0x0004;
constexpr GLenum GL_FLOAT_ = 0x1406;
constexpr GLenum GL_UNSIGNED_BYTE_ = 0x1401;
constexpr GLenum GL_UNSIGNED_SHORT_ = 0x1403;
constexpr GLenum GL_UNSIGNED_INT_ = 0x1405;
constexpr GLenum GL_TEXTURE_2D_ARRAY_ = 0x8C1A;

// Luanti's MATERIAL_TYPE values for blended materials
static bool is_transparent(int material_type)
{
	return material_type == 1 || material_type == 2 || material_type == 8 || material_type == 11;
}

// Luanti's MATERIAL_TYPE values for liquids: transparent, waving transparent
// and waving basic (the opaque ones are lava-like and do block the light)
static bool is_liquid(int material_type)
{
	return material_type == 2 || material_type == 7 || material_type == 8;
}

// What the G-buffer calls a node drawn with this MATERIAL_TYPE (Luanti's
// TileMaterialType: 4 waving leaves, 5 waving plants, 3 and 9 opaque liquids)
static int node_material(int material_type)
{
	if (is_liquid(material_type))
		return MATERIAL_LIQUID;
	switch (material_type) {
	case 3:
	case 9: return MATERIAL_LIQUID_OPAQUE;
	case 4: return MATERIAL_LEAVES;
	case 5: return MATERIAL_PLANTS;
	default: return MATERIAL_SOLID;
	}
}

// ---------------------------------------------------------------- state

struct Key {
	GLuint vbo, ibo, texture;
	GLsizei count;
	GLint base_vertex;
	uintptr_t index_offset;
	bool operator==(const Key &o) const = default;
};

struct KeyHash {
	size_t operator()(const Key &k) const
	{
		size_t h = k.vbo;
		h = h * 1000003u ^ k.ibo;
		h = h * 1000003u ^ k.texture;
		h = h * 1000003u ^ (size_t)k.count;
		h = h * 1000003u ^ (size_t)k.base_vertex;
		h = h * 1000003u ^ (size_t)k.index_offset;
		return h;
	}
};

static std::unordered_map<Key, Draw, KeyHash> g_blocks; // persistent
static std::vector<Draw> g_objects;                     // this frame only
static unsigned long long g_frame = 1;

static GLuint g_scene_fbo = 0;
static bool g_scene_known = false; // g_scene_fbo comes from a frame with world
static bool g_world_started = false; // this frame
static bool g_stopped = false;       // this frame: depth cleared after the world, or scene over

static bool g_record = false; // the pack draws shadow maps, a G-buffer or an overhead map
static bool g_hide_clouds = false;
static bool g_hide_shadows = false, g_hide_shadows_sent = false;
static bool g_hide_volumetric = false, g_hide_volumetric_sent = false;

// Joint matrices of this frame's skinned draws, copied on the GPU
static GLuint g_joint_ubo = 0;
static int g_joint_slots_used = 0;
constexpr int k_joint_slots = 64;
static std::unordered_map<GLuint, std::pair<unsigned long long, GLint>> g_block_binding; // program -> (frame, binding)

// Why draws were not recorded, for the log
static unsigned g_skipped_layout = 0, g_skipped_client = 0, g_skipped_skinned = 0, g_skipped_other = 0;
static bool g_reported = false;

// ---------------------------------------------------------------- sky

static bool g_sun_this_frame = false, g_moon_this_frame = false;
static Vec3 g_sun_quad, g_moon_quad;           // centres, this frame
static Vec3 g_sun_axis, g_moon_axis;           // orbit normals as seen from each quad
static bool g_have_sky = false;
static bool g_sky_camera = false; // this frame
static float g_sky_wv[16], g_sky_wvp[16];
static GLuint g_sky_fbo = 0;         // where the sky went, with g_sky_camera
static bool g_object_camera = false; // this frame
static bool g_hand_pass = false;     // this frame: the depth was cleared after the sky
static Vec3 g_object_camera_pos;     // render space, nodes
static double g_sky_time = -1;
static Sky g_sky;
static bool g_logged_sky = false;
static float g_sky_tilt = 0;          // orbit tilt of the last reading, radians
// How fast Luanti's time of day runs (per second), measured between readings,
// to work the sun out while the sky is not drawn (sky())
static double g_rate_ref_time = -1;
static float g_rate_ref_tod = 0;
static float g_tod_rate = 0;
static bool g_logged_estimate = false;

static Vec3 cross(const Vec3 &a, const Vec3 &b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static float dot(const Vec3 &a, const Vec3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Luanti's Sky::draw_sky_body builds a quad at distance 1 from the camera
// (corners (±d, ±d, -1)), then place_sky_body rotates it: XZ by ±90° (sun /
// moon), XY by the time of day, YZ by the orbit tilt. The quad's own +Y edge
// stays tangent to the orbit, so cross(centre, edge) is the orbit normal:
// +Z (tilted by less than 90°) for the sun, -Z for the moon.
static void check_sky_quad(GLsizei count, GLenum index_type, const void *indices)
{
	if (count != 6)
		return;
	const tracker::VertexAttrib &a = tracker::attrib(0);
	if (!a.enabled || a.buffer != 0 || a.type != GL_FLOAT_ || a.size < 3 || !a.offset)
		return;
	if (!tracker::element_buffer()) {
		// Client-side indices: they must only use the first four vertices.
		if (!indices)
			return;
		for (int i = 0; i < 6; i++) {
			unsigned idx = index_type == GL_UNSIGNED_SHORT_ ? static_cast<const unsigned short *>(indices)[i] :
				index_type == GL_UNSIGNED_INT_ ? static_cast<const unsigned *>(indices)[i] :
				index_type == GL_UNSIGNED_BYTE_ ? static_cast<const unsigned char *>(indices)[i] : 99;
			if (idx > 3)
				return;
		}
	}
	GLsizei stride = a.stride ? a.stride : a.size * 4;
	Vec3 v[4];
	for (int i = 0; i < 4; i++) {
		float p[3];
		std::memcpy(p, reinterpret_cast<const char *>(a.offset) + i * stride, sizeof p);
		if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
			return;
		v[i] = {p[0], p[1], p[2]};
	}
	Vec3 c = (v[0] + v[1] + v[2] + v[3]) * 0.25f;
	float dist = c.length();
	if (dist < 0.9f || dist > 1.1f)
		return;
	Vec3 ex = v[1] - v[0], ey = v[3] - v[0];
	float lx = ex.length(), ly = ey.length();
	// A small square facing the camera: the sunrise glow and the sky box
	// faces are much bigger.
	if (lx < 1e-4f || ly < 1e-4f || lx > 1.2f || ly > 1.2f || lx / ly < 0.8f || lx / ly > 1.25f)
		return;
	if (std::fabs(dot(ex, c)) > 0.05f * lx || std::fabs(dot(ey, c)) > 0.05f * ly)
		return;
	Vec3 axis = cross(c, ey).normalized();
	if (!g_sky_camera) {
		GLuint program = tracker::current_program();
		g_sky_camera = tracker::uniform(program, "uWVMatrix", g_sky_wv, 16) == 16 &&
			tracker::uniform(program, "uWVPMatrix", g_sky_wvp, 16) == 16;
		g_sky_fbo = tracker::draw_fbo();
	}
	if (axis.z > 0) {
		if (!g_sun_this_frame) {
			g_sun_this_frame = true;
			g_sun_quad = c.normalized();
			g_sun_axis = axis;
		}
	} else if (!g_moon_this_frame) {
		g_moon_this_frame = true;
		g_moon_quad = c.normalized();
		g_moon_axis = axis;
	}
}

// Luanti's getWickedTimeOfDay (sky.cpp): the day stretched so that the
// night is shorter than half of the cycle
static float wicked_time_of_day(float t)
{
	const float wn = 0.415f / 2;
	if (t > wn && t < 1.f - wn)
		return (t - wn) / (1.f - wn * 2) * 0.5f + 0.25f;
	if (t < 0.5f)
		return t / wn * 0.25f;
	return 1.f - ((1.f - t) / wn * 0.25f);
}

// Inverse of Luanti's getWickedTimeOfDay (sky.cpp)
static float unwicked_time_of_day(float w)
{
	const float wn = 0.415f / 2;
	if (w > 0.25f && w < 0.75f)
		return (w - 0.25f) / 0.5f * (1.f - wn * 2) + wn;
	if (w <= 0.25f)
		return w / 0.25f * wn;
	return 1.f - (1.f - w) / 0.25f * wn;
}

static void update_sky()
{
	if (!g_sun_this_frame && !g_moon_this_frame)
		return;
	// The sun is opposite the moon, on the same orbit.
	Vec3 sun = g_sun_this_frame ? g_sun_quad : -g_moon_quad;
	Vec3 axis = g_sun_this_frame ? g_sun_axis : -g_moon_axis;
	// Undo the tilt (rotation around X that took +Z to `axis`) and read the
	// angle along the orbit: Luanti rotates by wicked_time * 360 - 90.
	float tilt = std::atan2(-axis.y, axis.z);
	float cs = std::cos(tilt), sn = std::sin(tilt);
	float y = sun.y * cs + sun.z * sn;
	float angle = std::atan2(y, sun.x) * 57.29578f;
	float wicked = std::fmod((angle + 90.f) / 360.f + 1.f, 1.f);
	g_sky.sun_dir = sun;
	g_sky.time_of_day = unwicked_time_of_day(wicked);
	g_have_sky = true;
	g_sky_time = uptime();
	g_sky_tilt = tilt;
	// The speed of the day, from readings at least 2 s apart. A /time
	// command looks like an absurd speed: start measuring again.
	if (g_rate_ref_time < 0 || g_sky_time - g_rate_ref_time >= 2.0) {
		if (g_rate_ref_time >= 0) {
			float d = g_sky.time_of_day - g_rate_ref_tod;
			if (d < -0.5f)
				d += 1.f;
			else if (d > 0.5f)
				d -= 1.f;
			float rate = d / (float)(g_sky_time - g_rate_ref_time);
			g_tod_rate = std::fabs(rate) < 0.01f ? rate : 0.f;
		}
		g_rate_ref_time = g_sky_time;
		g_rate_ref_tod = g_sky.time_of_day;
	}
	if (!g_logged_sky) {
		g_logged_sky = true;
		log::info("Sun read from the sky: direction (%.2f, %.2f, %.2f), time %.3f, orbit tilt %.1f°%s",
			sun.x, sun.y, sun.z, g_sky.time_of_day, tilt * 57.29578f,
			g_sun_this_frame ? "" : " (from the moon)");
	}
}

bool sky_camera(float wv[16], float wvp[16])
{
	if (!g_sky_camera)
		return false;
	std::memcpy(wv, g_sky_wv, sizeof g_sky_wv);
	std::memcpy(wvp, g_sky_wvp, sizeof g_sky_wvp);
	return true;
}

bool sky_framebuffer(GLuint &fbo)
{
	fbo = g_sky_fbo;
	return g_sky_camera;
}

bool object_camera(Vec3 &render_pos)
{
	render_pos = g_object_camera_pos;
	return g_object_camera;
}

// The camera's position from an entity drawn after the sky: its mWorldView
// times the inverse of its mWorld is the scene's view, translation included
// (the sky's has none). The wielded item has a camera of its own, whose
// rotation is not the sky's, except when both look straight along +Z: it
// comes after a depth clear, and is left out by that. Even in first person
// Luanti draws the player's own model (culled, for its shadow), so there is
// nearly always one.
static void check_object_camera(GLuint program, const float *world_matrix)
{
	if (g_object_camera || !g_sky_camera || g_hand_pass || tracker::draw_fbo() != g_sky_fbo)
		return;
	float wv[16];
	if (tracker::uniform(program, "mWorldView", wv, 16) != 16)
		return;
	Mat4 view = Mat4::from(wv) * Mat4::from(world_matrix).inverse();
	Mat4 sky = Mat4::from(g_sky_wv);
	for (int c = 0; c < 3; c++) {
		float len = std::sqrt(sky.at(0, c) * sky.at(0, c) + sky.at(1, c) * sky.at(1, c) + sky.at(2, c) * sky.at(2, c));
		if (!(len > 0) || !std::isfinite(len))
			return;
		for (int r = 0; r < 3; r++)
			if (!(std::fabs(view.at(r, c) - sky.at(r, c) / len) < 1e-3f))
				return;
	}
	Mat4 inv = view.inverse();
	Vec3 pos{inv.at(0, 3), inv.at(1, 3), inv.at(2, 3)};
	if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
		return;
	g_object_camera = true;
	g_object_camera_pos = pos * float(1.0 / BS);
}

/*
	Luanti does not always draw its sky: not with the camera inside its cloud
	layer (by default 120 to 136 nodes up), clouds hidden by the pack or not.
	Then the sun is worked out from the last one seen: its time of
	day, moved on at the speed the day was running, put on the same orbit
	the way Luanti's place_sky_body does it (the inverse of update_sky()).
*/
double sky_age()
{
	return g_have_sky ? uptime() - g_sky_time : -1.0;
}

bool sky(Sky &out)
{
	if (!g_have_sky)
		return false;
	double age = uptime() - g_sky_time;
	if (age <= 3.0) {
		out = g_sky;
		return true;
	}
	if (!g_logged_estimate) {
		g_logged_estimate = true;
		log::info("Luanti stopped drawing its sky (the camera inside its clouds?): the sun is worked out from "
			"the last one seen, at %.5f days per second", g_tod_rate);
	}
	float tod = g_sky.time_of_day + g_tod_rate * (float)age;
	tod -= std::floor(tod);
	float angle = (wicked_time_of_day(tod) * 360.f - 90.f) / 57.29578f;
	float s = std::sin(angle);
	out.sun_dir = Vec3{std::cos(angle), s * std::cos(g_sky_tilt), s * std::sin(g_sky_tilt)}.normalized();
	out.time_of_day = tod;
	return true;
}

// ---------------------------------------------------------------- recording

// Binding point of the program's JointMatrices block (-1 if none)
static GLint joint_binding(GLuint program)
{
	auto &cached = g_block_binding[program];
	if (cached.first == g_frame)
		return cached.second; // Luanti could change it, but not within a frame
	cached.first = g_frame;
	cached.second = -1;
	GLuint index = gl::GetUniformBlockIndex(program, "JointMatrices");
	if (index != GL_INVALID_INDEX)
		gl::GetActiveUniformBlockiv(program, index, GL_UNIFORM_BLOCK_BINDING, &cached.second);
	return cached.second;
}

// Luanti uploads the pose of each animated mesh to a uniform buffer right
// before drawing it; copy it (GPU to GPU) so the replay draws the same pose.
static bool capture_joints(GLuint program, Draw &d)
{
	if (!gl::loaded() && !gl::load())
		return false;
	if (g_joint_slots_used >= k_joint_slots)
		return false;
	GLint binding = joint_binding(program);
	if (binding < 0)
		return false;
	GLint source = 0;
	long long start = 0, size = 0;
	gl::GetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, binding, &source);
	gl::GetInteger64i_v(GL_UNIFORM_BUFFER_START, binding, &start);
	gl::GetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, binding, &size);
	if (!source)
		return false;

	GLint read = 0, write = 0;
	gl::GetIntegerv(0x8F36 /* GL_COPY_READ_BUFFER_BINDING */, &read);
	gl::GetIntegerv(0x8F37 /* GL_COPY_WRITE_BUFFER_BINDING */, &write);
	if (!g_joint_ubo) {
		gl::GenBuffers(1, &g_joint_ubo);
		gl::BindBuffer(GL_COPY_WRITE_BUFFER, g_joint_ubo);
		gl::BufferData(GL_COPY_WRITE_BUFFER, JOINT_SLOT * k_joint_slots, nullptr, GL_DYNAMIC_COPY);
	}
	gl::BindBuffer(GL_COPY_READ_BUFFER, source);
	if (size <= 0) { // bound whole
		GLint total = 0;
		gl::GetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &total);
		size = total - start;
	}
	bool ok = size > 0;
	if (ok) {
		d.joint_offset = (GLintptr)g_joint_slots_used * JOINT_SLOT;
		gl::BindBuffer(GL_COPY_WRITE_BUFFER, g_joint_ubo);
		gl::CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)start, d.joint_offset,
			(GLsizeiptr)std::min<long long>(size, JOINT_SLOT));
		g_joint_slots_used++;
	}
	gl::BindBuffer(GL_COPY_READ_BUFFER, read);
	gl::BindBuffer(GL_COPY_WRITE_BUFFER, write);
	return ok;
}

GLuint joint_buffer()
{
	return g_joint_ubo;
}

static bool same_buffer(const tracker::VertexAttrib &a, GLuint vbo)
{
	return a.enabled && a.buffer == vbo;
}

// Luanti's render origin (cameraOffset, BS units). The tracker's value never
// moves once it exists, so it is looked up by name only until then.
static const float *camera_offset()
{
	static const float zero[3] = {0, 0, 0};
	static const tracker::UniformValue *value = nullptr;
	if (!value)
		value = tracker::global_uniform_value("cameraOffset");
	return value && value->f.size() >= 3 ? value->f.data() : zero;
}

void on_draw(GLenum mode, GLsizei count, GLenum index_type, const void *indices, GLint base_vertex,
	GLsizei instances)
{
	if (g_stopped)
		return;
	tracker::ProgramInfo info = tracker::current_info();
	if (!info.world) {
		if (!g_world_started)
			check_sky_quad(count, index_type, indices);
		return;
	}
	GLuint fbo = tracker::draw_fbo();
	bool block = std::strcmp(info.name, "nodes_shader") == 0;
	if (!block && info.world_matrix && (!g_scene_known || fbo == g_scene_fbo))
		check_object_camera(tracker::current_program(), info.world_matrix);
	if (!g_world_started) {
		// Entities drawn before the first block: accept them in last frame's scene.
		if (!block && (!g_scene_known || fbo != g_scene_fbo))
			return;
		if (block) {
			g_world_started = true;
			g_scene_fbo = fbo;
			g_scene_known = true;
		}
	} else if (fbo != g_scene_fbo) {
		return;
	}
	if (!g_record)
		return;

	const tracker::VertexAttrib &pos = tracker::attrib(0);
	GLuint ibo = tracker::element_buffer();
	if (!pos.buffer || !ibo) {
		g_skipped_client++; // vertices or indices in client memory: gone after the call
		return;
	}
	if (mode != GL_TRIANGLES_ || instances != 1 || !index_type || !info.world_matrix || !pos.enabled ||
			pos.type != GL_FLOAT_ || pos.size != 3 || pos.integer) {
		g_skipped_layout++;
		return;
	}

	Draw d;
	d.object = !block;
	d.array_texture = info.array_texture;
	d.transparent = is_transparent(info.material_type);
	d.liquid = block && is_liquid(info.material_type);
	d.material = block ? node_material(info.material_type) : MATERIAL_ENTITY;
	d.vbo = pos.buffer;
	d.ibo = ibo;
	d.vbo_generation = tracker::buffer_generation(d.vbo);
	d.ibo_generation = tracker::buffer_generation(d.ibo);
	if (!d.vbo_generation || !d.ibo_generation) {
		g_skipped_other++;
		return;
	}
	d.index_type = index_type;
	d.count = count;
	d.index_offset = reinterpret_cast<uintptr_t>(indices);
	d.base_vertex = base_vertex;
	d.position = pos;
	d.culling = tracker::culling();
	// Everything must come from the same vertex buffer (Luanti interleaves).
	const tracker::VertexAttrib &n = tracker::attrib(1), &c = tracker::attrib(2), &l = tracker::attrib(3), &t = tracker::attrib(4);
	if (same_buffer(n, d.vbo) && n.type == GL_FLOAT_ && n.size == 3)
		d.normal = n;
	if (same_buffer(c, d.vbo) && c.type == GL_UNSIGNED_BYTE_ && c.size == 4)
		d.color = c;
	if (same_buffer(t, d.vbo) && t.type == GL_FLOAT_ && t.size >= 2)
		d.uv = t;
	if (same_buffer(l, d.vbo) && l.integer)
		d.layer = l;
	d.texture = tracker::bound_texture(0, d.array_texture ? GL_TEXTURE_2D_ARRAY_ : 0x0DE1);
	if (d.array_texture && !d.layer.enabled)
		d.texture = 0; // can't tell which layer: no alpha test
	if (!d.uv.enabled)
		d.texture = 0;

	// Luanti draws relative to the camera offset (render space, BS units):
	// world = scale(1 / BS) * translation(offset) * mWorld, written out
	const float *offset = camera_offset();
	const float *m = info.world_matrix;
	const double s = 1.0 / BS;
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 3; row++)
			d.world.m[col * 4 + row] = s * m[col * 4 + row] + (s * offset[row]) * m[col * 4 + 3];
		d.world.m[col * 4 + 3] = m[col * 4 + 3];
	}
	if (pos.stride == 40 && pos.offset == 0)
		d.has_bounds = tracker::buffer_bounds(d.vbo, d.bounds_min, d.bounds_max);
	d.frame = g_frame;

	// A skinned program also draws static meshes (no weights): those are
	// recorded like any other.
	const tracker::VertexAttrib &w = tracker::attrib(8), &j = tracker::attrib(9);
	if (info.skinned && w.enabled) {
		if (!d.object || !w.buffer || w.buffer != j.buffer || !j.enabled || !j.integer ||
				!capture_joints(tracker::current_program(), d)) {
			g_skipped_skinned++;
			return;
		}
		d.skinned = true;
		d.skin_vbo = w.buffer;
		d.skin_generation = tracker::buffer_generation(w.buffer);
		d.weights = w;
		d.joint_ids = j;
		d.has_bounds = false; // the pose moves vertices away from the rest bounds
	}

	if (d.object) {
		g_objects.push_back(d);
	} else {
		Key k{d.vbo, d.ibo, d.texture, d.count, d.base_vertex, d.index_offset};
		g_blocks[k] = d;
	}
}

void on_clear(GLbitfield mask)
{
	// The wielded item is drawn last, after clearing the depth buffer. It
	// sits at the camera and casts no shadow in Luanti.
	if ((mask & GL_DEPTH_BUFFER_BIT) && g_world_started && tracker::draw_fbo() == g_scene_fbo)
		g_stopped = true;
	if ((mask & GL_DEPTH_BUFFER_BIT) && g_sky_camera && tracker::draw_fbo() == g_sky_fbo)
		g_hand_pass = true;
}

void on_scene_end()
{
	if (g_world_started)
		g_stopped = true;
}

static void send_shadow_strength();

void on_frame_end()
{
	send_shadow_strength();
	update_sky();
	g_sun_this_frame = g_moon_this_frame = false;

	if (g_world_started && !g_reported && g_frame > 300) {
		g_reported = true;
		log::info("Geometry registered: %zu block draws, %zu entity draws in the last frame; "
			"discarded: %u in client memory, %u by format, %u with skeleton, %u other", g_blocks.size(),
			g_objects.size(), g_skipped_client, g_skipped_layout, g_skipped_skinned, g_skipped_other);
	}
	g_objects.clear();
	g_sky_camera = false;
	g_object_camera = false;
	g_hand_pass = false;
	g_joint_slots_used = 0;
	g_world_started = false;
	g_stopped = false;
	g_frame++;
}

void set_record_draws(bool record)
{
	if (record == g_record)
		return;
	g_record = record;
	if (!record) {
		g_blocks.clear();
		g_objects.clear();
	}
}

void set_hide_clouds(bool hide)
{
	g_hide_clouds = hide;
}

void set_hide_shadows(bool hide)
{
	g_hide_shadows = hide;
}

void set_hide_volumetric_light(bool hide)
{
	g_hide_volumetric = hide;
}

void reapply_overrides()
{
	// Relinking resets uniforms to Luanti's values: pretend nothing was sent.
	g_hide_shadows_sent = !g_hide_shadows ? g_hide_shadows_sent : false;
	g_hide_volumetric_sent = !g_hide_volumetric ? g_hide_volumetric_sent : false;
}

float filter_uniform(GLint location, float value)
{
	if (location < 0)
		return value;
	if (g_hide_shadows && location == tracker::current_shadow_strength_location())
		return 0.f;
	if (g_hide_volumetric && location == tracker::current_volumetric_strength_location())
		return 0.f;
	return value;
}

// Luanti only sends these uniforms when they change, so switching an
// override on or off has to set them in every program right away.
static void send_override(const char *uniform, bool hide, bool &sent)
{
	if (hide == sent || !gl::loaded())
		return;
	sent = hide;
	GLint previous = 0;
	gl::GetIntegerv(GL_CURRENT_PROGRAM, &previous);
	tracker::for_each_program_with(uniform, [](void *ctx, GLuint program, GLint location, const tracker::UniformValue *v) {
		bool hidden = *static_cast<bool *>(ctx);
		float value = hidden || !v || v->f.empty() ? 0.f : v->f[0];
		gl::UseProgram(program);
		gl::Uniform1f(gameshaders::real_location(program, location), value);
	}, &hide);
	gl::UseProgram(previous);
}

static void send_shadow_strength()
{
	bool before = g_hide_shadows_sent;
	send_override("f_shadow_strength", g_hide_shadows, g_hide_shadows_sent);
	if (before != g_hide_shadows_sent)
		log::info("Luanti dynamic shadows %s", g_hide_shadows ? "hidden (the pack draws them)" : "restored");
	send_override("volumetricLightStrength", g_hide_volumetric, g_hide_volumetric_sent);
}

bool skip_draw()
{
	if (!g_hide_clouds)
		return false;
	return std::strcmp(tracker::current_info().name, "cloud_shader") == 0;
}

// ---------------------------------------------------------------- access

static bool intact(const Draw &d)
{
	return tracker::buffer_generation(d.vbo) == d.vbo_generation && tracker::buffer_generation(d.ibo) == d.ibo_generation &&
		(!d.skinned || tracker::buffer_generation(d.skin_vbo) == d.skin_generation);
}

void collect(std::vector<const Draw *> &out, bool this_frame_only)
{
	out.clear();
	for (auto it = g_blocks.begin(); it != g_blocks.end();) {
		if (!intact(it->second)) {
			it = g_blocks.erase(it);
			continue;
		}
		if (!this_frame_only || it->second.frame == g_frame)
			out.push_back(&it->second);
		++it;
	}
	for (const Draw &d : g_objects)
		if (intact(d))
			out.push_back(&d);
}

unsigned long long frame()
{
	return g_frame;
}

} // namespace prisma::geometry
