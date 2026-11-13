// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/tracking/tracker.hpp"
#include "core/dev/trace.hpp"
#include "core/hooks/hooks.hpp"
#include "core/tracking/shader_catalog.hpp"
#include "core/tracking/tracker_internal.hpp"
#include "common/log.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------- records

namespace prisma::tracker::detail {

std::unordered_map<GLuint, Shader> g_shaders;
std::unordered_map<GLuint, Program> g_programs;
std::unordered_map<GLuint, Surface> g_textures, g_renderbuffers;
std::unordered_map<GLuint, std::map<GLenum, Attachment>> g_fbos;
std::unordered_map<GLuint, VertexArray> g_vaos;
std::unordered_map<GLuint, BufferInfo> g_buffers;
Program *g_current = nullptr;
GLuint g_draw_fbo = 0, g_read_fbo = 0, g_vao = 0;
unsigned long long g_frame = 0;

} // namespace prisma::tracker::detail

namespace prisma::tracker {

using namespace detail;

static std::unordered_map<std::string, UniformValue> g_global_uniforms;
// "<program name>|<uniform>": e.g. the cloud colour of cloud_shader
static std::unordered_map<std::string, UniformValue> g_named_uniforms;
// Names of every program linked so far (compat.cpp)
static std::unordered_set<std::string> g_program_names;

static GLuint g_bound_rb = 0;
static GLuint g_active_unit = 0;
static GLuint g_bound_2d[64] = {};
static GLuint g_bound_array[64] = {};
static GLuint g_array_buffer = 0;
static Culling g_culling;
static unsigned long long g_generations = 0;
static long long g_world_frames = -1; // frames since the world first appeared
constexpr GLenum GL_TEXTURE_2D_ARRAY_ = 0x8C1A;

static std::string printf_str(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string printf_str(const char *fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

// ---------------------------------------------------------------- frame

static void log_gl_info()
{
	using PFN_glGetString = const GLubyte *(APIENTRY *)(GLenum);
	auto get = (PFN_glGetString)hooks::raw_proc("glGetString");
	auto str = [&](GLenum e) {
		const GLubyte *s = get ? get(e) : nullptr;
		return s ? (const char *)s : "(unknown)";
	};
	std::string version = host_version(); // only Windows binaries carry it
	log::info("Luanti %s", version.empty() ? to_utf8(host_exe_dir()).c_str() : version.c_str());
	log::info("OpenGL: %s", str(GL_VERSION));
	log::info("GPU: %s / %s", str(GL_VENDOR), str(GL_RENDERER));
	log::info("GLSL: %s", str(GL_SHADING_LANGUAGE_VERSION));
}

void on_frame_end()
{
	if (g_frame == 0)
		log_gl_info();

	trace::frame_ended();

	bool world = false;
	for (auto &[id, p] : g_programs) {
		if (!p.used_this_frame)
			continue;
		world |= p.is_world;
		if (!p.uniforms_reported) {
			p.uniforms_reported = true;
			std::string names;
			for (auto &[loc, n] : p.uniform_names)
				names += (names.empty() ? "" : ", ") + n.name;
			log::info("Program %s#%u in use. Uniforms: %s", p.name.c_str(), p.id,
				names.empty() ? "(none)" : names.c_str());
		}
		p.used_this_frame = false;
	}

	if (world && g_world_frames < 0) {
		g_world_frames = 0;
		log::info("World visible from frame %llu", g_frame);
	}
	if (g_world_frames >= 0)
		g_world_frames++;

	g_frame++;
	trace::frame_started(g_world_frames);
}

// ---------------------------------------------------------------- init / procs

void init(const fs::path &shaders_dir)
{
	catalog::set_dir(shaders_dir);
}

void on_proc_request(const char *name, bool found)
{
	trace::proc_requested(name, found);
}

// ---------------------------------------------------------------- shaders

void on_create_shader(GLuint shader, GLenum type)
{
	g_shaders[shader] = Shader{type};
}

void on_shader_source(GLuint shader, const std::string &source)
{
	Shader &s = g_shaders[shader];
	s.source = source;
	s.identified = false;
}

GLenum shader_type(GLuint shader)
{
	auto it = g_shaders.find(shader);
	return it == g_shaders.end() ? 0 : it->second.type;
}

void on_attach_shader(GLuint program, GLuint shader)
{
	Program &p = g_programs[program];
	p.id = program;
	p.shaders.push_back(shader);
}

static const char *stage_of(GLenum type)
{
	switch (type) {
	case GL_VERTEX_SHADER: return "vertex";
	case GL_FRAGMENT_SHADER: return "fragment";
	case GL_GEOMETRY_SHADER: return "geometry";
	}
	return "";
}

void on_link_program(GLuint program)
{
	Program &p = g_programs[program];
	p.id = program;
	p.uniform_names.clear();
	p.values.clear();
	p.uniforms_reported = false;

	std::string vert, frag, detail;
	for (GLuint id : p.shaders) {
		auto it = g_shaders.find(id);
		if (it == g_shaders.end())
			continue;
		Shader &s = it->second;
		if (!s.identified) {
			s.match = catalog::identify(s.source, stage_of(s.type));
			s.identified = true;
		}
		std::string name = s.match.name.empty() ? "unknown" : s.match.name;
		if (s.type == GL_VERTEX_SHADER)
			vert = s.match.name;
		else if (s.type == GL_FRAGMENT_SHADER)
			frag = s.match.name;
		detail += printf_str("%s%s=%s %.0f%%", detail.empty() ? "" : ", ", stage_of(s.type), name.c_str(),
			s.match.score * 100.f);
	}
	p.name = !frag.empty() ? frag : !vert.empty() ? vert : "unknown";
	g_program_names.insert(p.name);
	p.is_world = p.name == "nodes_shader";
	p.is_2d = p.name.starts_with("Irrlicht/Renderer2D");
	p.geometry = p.is_world || p.name == "object_shader";
	p.array_texture = p.skinned = false;
	p.material_type = -1;
	for (GLuint id : p.shaders) {
		auto it = g_shaders.find(id);
		if (it == g_shaders.end() || it->second.type != GL_VERTEX_SHADER)
			continue;
		const std::string &src = it->second.source;
		auto define = [&](const char *name, int *value) {
			std::string key = std::string("#define ") + name;
			for (size_t at = src.find(key); at != std::string::npos; at = src.find(key, at + 1)) {
				char next = at + key.size() < src.size() ? src[at + key.size()] : '\n';
				if (next != ' ' && next != '\t' && next != '\n')
					continue;
				if (value)
					*value = std::atoi(src.c_str() + at + key.size());
				return true;
			}
			return false;
		};
		p.array_texture = define("USE_ARRAY_TEXTURE", nullptr);
		p.skinned = define("USE_SKINNING", nullptr);
		define("MATERIAL_TYPE", &p.material_type);
	}

	trace::program_linked(program);
	log::info("Program %u = %s  (%s)", program, p.name.c_str(), detail.c_str());

	// Shaders get detached and deleted right after linking; keep the list
	// from growing if the same program object is relinked.
	p.shaders.clear();
}

void on_delete_program(GLuint program)
{
	auto it = g_programs.find(program);
	if (it == g_programs.end())
		return;
	if (g_current == &it->second)
		g_current = nullptr;
	g_programs.erase(it);
}

void on_use_program(GLuint program)
{
	if (program == 0) {
		g_current = nullptr;
		return;
	}
	Program &p = g_programs[program];
	p.id = program;
	g_current = &p;
}

void on_get_uniform_location(GLuint program, const GLchar *name, GLint location)
{
	if (location < 0 || !name)
		return;
	Program &p = g_programs[program];
	p.id = program;
	p.uniform_names[location] = UniformName{name};
	if (std::strcmp(name, "mWorld") == 0)
		p.world_location = location;
	else if (std::strcmp(name, "f_shadow_strength") == 0)
		p.shadow_strength_location = location;
	else if (std::strcmp(name, "volumetricLightStrength") == 0)
		p.volumetric_strength_location = location;
}

// Into a value that may already hold one: the vectors keep their memory
static void store(UniformValue &v, UKind kind, int components, GLsizei count, const void *data)
{
	v.kind = kind;
	v.components = components;
	v.count = count;
	v.frame = g_frame;
	size_t n = size_t(components) * size_t(count);
	if (kind == UKind::Int) {
		v.i.assign((const int *)data, (const int *)data + n);
		v.f.clear();
	} else {
		v.f.assign((const float *)data, (const float *)data + n);
		v.i.clear();
	}
}

void on_uniform(GLint location, UKind kind, int components, GLsizei count, const void *data)
{
	if (!g_current || location < 0 || !data || count <= 0)
		return;
	store(g_current->values[location], kind, components, count, data);
	auto it = g_current->uniform_names.find(location);
	if (it == g_current->uniform_names.end())
		return;
	UniformName &u = it->second;
	if (!u.global) { // the first time: unordered_map entries never move
		u.global = &g_global_uniforms[u.name];
		u.named = &g_named_uniforms[g_current->name + "|" + u.name];
	}
	store(*u.global, kind, components, count, data);
	store(*u.named, kind, components, count, data);
}

// ---------------------------------------------------------------- framebuffers

void on_bind_framebuffer(GLenum target, GLuint fbo)
{
	if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
		bool changed = g_draw_fbo != fbo;
		g_draw_fbo = fbo;
		if (trace::active() && changed)
			trace::framebuffer_bound(fbo);
	}
	if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
		g_read_fbo = fbo;
}

void on_delete_framebuffer(GLuint fbo)
{
	g_fbos.erase(fbo);
}

void on_delete_texture(GLuint texture)
{
	g_textures.erase(texture);
}

void on_framebuffer_texture(GLenum target, GLenum attachment, GLuint texture, GLint level, bool multisample)
{
	GLuint fbo = target == GL_READ_FRAMEBUFFER ? g_read_fbo : g_draw_fbo;
	g_fbos[fbo][attachment] = Attachment{false, texture, multisample};
	if (trace::active())
		trace::attached(fbo, attachment, false, texture, level);
}

void on_framebuffer_renderbuffer(GLenum target, GLenum attachment, GLuint renderbuffer)
{
	GLuint fbo = target == GL_READ_FRAMEBUFFER ? g_read_fbo : g_draw_fbo;
	g_fbos[fbo][attachment] = Attachment{true, renderbuffer};
	if (trace::active())
		trace::attached(fbo, attachment, true, renderbuffer, 0);
}

void on_bind_renderbuffer(GLuint renderbuffer)
{
	g_bound_rb = renderbuffer;
}

void on_renderbuffer_storage(GLenum internal_format, GLsizei width, GLsizei height, GLsizei samples)
{
	g_renderbuffers[g_bound_rb] = Surface{internal_format, width, height, samples};
}

void on_blit_framebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
	GLbitfield mask)
{
	if (trace::active())
		trace::blit(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask);
}

void on_draw_buffers(GLsizei n, const GLenum *bufs)
{
	if (trace::active())
		trace::draw_buffers(n, bufs);
}

// ---------------------------------------------------------------- textures

void on_active_texture(GLenum unit)
{
	GLuint u = unit - GL_TEXTURE0;
	if (u < 64)
		g_active_unit = u;
}

void on_bind_texture(GLenum target, GLuint texture)
{
	if (target == GL_TEXTURE_2D)
		g_bound_2d[g_active_unit] = texture;
	else if (target == GL_TEXTURE_2D_ARRAY_)
		g_bound_array[g_active_unit] = texture;
}

void on_tex_image(GLenum target, GLint level, GLenum internal_format, GLsizei width, GLsizei height)
{
	if (target != GL_TEXTURE_2D || level != 0)
		return;
	GLuint tex = g_bound_2d[g_active_unit];
	if (tex)
		g_textures[tex] = Surface{internal_format, width, height, 0};
}

// ---------------------------------------------------------------- drawing

// The bound vertex array's record, found once per bind rather than on every
// call that reads or changes it (unordered_map entries never move)
static VertexArray *g_vao_record = nullptr;

static VertexArray &bound_vao()
{
	if (!g_vao_record)
		g_vao_record = &g_vaos[g_vao];
	return *g_vao_record;
}

void on_bind_buffer(GLenum target, GLuint buffer)
{
	if (target == 0x8892) // GL_ARRAY_BUFFER
		g_array_buffer = buffer;
	else if (target == 0x8893) // GL_ELEMENT_ARRAY_BUFFER: part of the VAO
		bound_vao().element_buffer = buffer;
}

void on_bind_vertex_array(GLuint vao)
{
	g_vao = vao;
	g_vao_record = &g_vaos[vao];
}

void on_vertex_attrib_pointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
	const void *pointer, bool integer)
{
	if (index >= 16)
		return;
	VertexAttrib &a = bound_vao().attribs[index];
	a.buffer = g_array_buffer;
	a.size = size;
	a.type = type;
	a.normalized = normalized != 0;
	a.integer = integer;
	a.stride = stride;
	a.offset = (uintptr_t)pointer;
}

void on_enable_vertex_attrib(GLuint index, bool enabled)
{
	if (index < 16)
		bound_vao().attribs[index].enabled = enabled;
}

void on_buffer_data(GLenum target, GLsizeiptr size, const void *data, GLenum usage, bool partial)
{
	GLuint buffer = target == 0x8892 ? g_array_buffer : target == 0x8893 ? bound_vao().element_buffer : 0;
	if (!buffer)
		return;
	BufferInfo &b = g_buffers[buffer];
	b.generation = ++g_generations;
	if (!partial) {
		b.size = size;
		b.usage = usage;
		// Bounds for culling Prisma's replays, assuming Luanti's vertex layout;
		// whoever uses them checks that the draw really has that layout.
		b.has_bounds = false;
		constexpr GLsizeiptr k_stride = 40;
		if (target == 0x8892 && data && size >= k_stride && size % k_stride == 0) {
			const unsigned char *p = static_cast<const unsigned char *>(data);
			float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
			bool finite = true;
			for (GLsizeiptr at = 0; at < size; at += k_stride) {
				float v[3];
				std::memcpy(v, p + at, sizeof v);
				for (int k = 0; k < 3; k++) {
					finite &= std::isfinite(v[k]);
					mn[k] = std::min(mn[k], v[k]);
					mx[k] = std::max(mx[k], v[k]);
				}
			}
			if (finite) {
				b.has_bounds = true;
				std::memcpy(b.min, mn, sizeof mn);
				std::memcpy(b.max, mx, sizeof mx);
			}
		}
	} else {
		b.has_bounds = false;
	}
	if (trace::active())
		trace::buffer_upload(buffer);
}

void on_delete_buffer(GLuint buffer)
{
	g_buffers.erase(buffer);
}

const VertexAttrib &attrib(unsigned index)
{
	static const VertexAttrib none;
	return index < 16 ? bound_vao().attribs[index] : none;
}

GLuint element_buffer()
{
	return bound_vao().element_buffer;
}

unsigned long long buffer_generation(GLuint buffer)
{
	auto it = g_buffers.find(buffer);
	return it == g_buffers.end() ? 0 : it->second.generation;
}

bool buffer_bounds(GLuint buffer, float min[3], float max[3])
{
	auto it = g_buffers.find(buffer);
	if (it == g_buffers.end() || !it->second.has_bounds)
		return false;
	std::memcpy(min, it->second.min, sizeof it->second.min);
	std::memcpy(max, it->second.max, sizeof it->second.max);
	return true;
}

void on_enable(GLenum cap, bool enabled)
{
	if (cap == 0x0B44) // GL_CULL_FACE
		g_culling.enabled = enabled;
}

void on_cull_face(GLenum mode)
{
	g_culling.face = mode;
}

void on_front_face(GLenum mode)
{
	g_culling.front = mode;
}

Culling culling()
{
	return g_culling;
}

GLuint bound_texture(unsigned unit, GLenum target)
{
	if (unit >= 64)
		return 0;
	return target == GL_TEXTURE_2D_ARRAY_ ? g_bound_array[unit] : g_bound_2d[unit];
}

GLint current_shadow_strength_location()
{
	return g_current ? g_current->shadow_strength_location : -1;
}

GLint current_volumetric_strength_location()
{
	return g_current ? g_current->volumetric_strength_location : -1;
}

void for_each_program_with(const char *name,
	void (*fn)(void *ctx, GLuint program, GLint location, const UniformValue *value), void *ctx)
{
	for (auto &[id, p] : g_programs)
		for (auto &[loc, n] : p.uniform_names)
			if (n.name == name) {
				auto v = p.values.find(loc);
				fn(ctx, id, loc, v == p.values.end() ? nullptr : &v->second);
			}
}

ProgramInfo current_info()
{
	ProgramInfo info;
	if (!g_current)
		return info;
	const Program &p = *g_current;
	info.name = p.name.c_str();
	info.world = p.geometry;
	info.array_texture = p.array_texture;
	info.skinned = p.skinned;
	info.material_type = p.material_type;
	if (p.world_location >= 0) {
		auto v = p.values.find(p.world_location);
		if (v != p.values.end() && v->second.f.size() >= 16)
			info.world_matrix = v->second.f.data();
	}
	return info;
}

void on_draw(const char *kind, GLsizei count, GLsizei instances, GLenum index_type, const void *indices)
{
	if (g_current)
		g_current->used_this_frame = true;
	if (trace::active())
		trace::draw(kind, count, instances, index_type, indices);
}

void on_clear(GLbitfield mask)
{
	if (trace::active())
		trace::clear(mask);
}

void on_viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (trace::active())
		trace::viewport(x, y, width, height);
}

// ---------------------------------------------------------------- queries

GLuint draw_fbo()
{
	return g_draw_fbo;
}

GLuint read_fbo()
{
	return g_read_fbo;
}

bool fbo_multisampled(GLuint fbo)
{
	auto f = g_fbos.find(fbo);
	if (f == g_fbos.end())
		return false;
	auto a = f->second.find(GL_COLOR_ATTACHMENT0);
	return a != f->second.end() && a->second.multisample;
}

GLuint current_program()
{
	return g_current ? g_current->id : 0;
}

bool current_is_world()
{
	return g_current && g_current->is_world;
}

bool current_is_2d()
{
	return g_current && g_current->is_2d;
}

SurfaceInfo fbo_texture(GLuint fbo, GLenum attachment)
{
	auto f = g_fbos.find(fbo);
	if (f == g_fbos.end())
		return {};
	auto a = f->second.find(attachment);
	if (a == f->second.end() || a->second.renderbuffer || a->second.id == 0)
		return {};
	SurfaceInfo info;
	info.texture = a->second.id;
	auto t = g_textures.find(info.texture);
	if (t != g_textures.end()) {
		info.format = t->second.format;
		info.width = t->second.width;
		info.height = t->second.height;
	}
	return info;
}

static int copy_value(const UniformValue &v, float *out, int max)
{
	int n = 0;
	if (v.kind == UKind::Int)
		for (; n < max && n < (int)v.i.size(); n++)
			out[n] = (float)v.i[n];
	else
		for (; n < max && n < (int)v.f.size(); n++)
			out[n] = v.f[n];
	return n;
}

int uniform(GLuint program, const char *name, float *out, int max)
{
	auto p = g_programs.find(program);
	if (p == g_programs.end())
		return 0;
	for (auto &[loc, n] : p->second.uniform_names) {
		if (n.name != name)
			continue;
		auto v = p->second.values.find(loc);
		return v == p->second.values.end() ? 0 : copy_value(v->second, out, max);
	}
	return 0;
}

const UniformValue *uniform_value(GLuint program, GLint location)
{
	auto p = g_programs.find(program);
	if (p == g_programs.end())
		return nullptr;
	auto v = p->second.values.find(location);
	return v == p->second.values.end() ? nullptr : &v->second;
}

const UniformValue *global_uniform_value(const std::string &name)
{
	auto v = g_global_uniforms.find(name);
	return v == g_global_uniforms.end() ? nullptr : &v->second;
}

unsigned long long current_frame()
{
	return g_frame;
}

int named_uniform(const char *program_name, const char *name, float *out, int max)
{
	auto v = g_named_uniforms.find(std::string(program_name) + "|" + name);
	return v == g_named_uniforms.end() ? 0 : copy_value(v->second, out, max);
}

bool has_program(const char *name)
{
	return g_program_names.count(name) > 0;
}

int global_uniform(const char *name, float *out, int max)
{
	auto v = g_global_uniforms.find(name);
	return v == g_global_uniforms.end() ? 0 : copy_value(v->second, out, max);
}

} // namespace prisma::tracker
