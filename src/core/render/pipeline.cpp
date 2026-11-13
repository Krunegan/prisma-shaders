// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/render/pipeline.hpp"
#include "core/dev/developer.hpp"
#include "core/pack/api.hpp"
#include "core/audio/audio.hpp"
#include "core/ui/debugview.hpp"
#include "core/render/frame.hpp"
#include "core/render/gameshaders.hpp"
#include "common/ini.hpp"
#include "core/render/geometry.hpp"
#include "core/hooks/glapi.hpp"
#include "core/render/lighting.hpp"
#include "common/log.hpp"
#include "core/pack/options.hpp"
#include "core/pack/passglsl.hpp"
#include "core/pack/pack.hpp"
#include "core/platform/platform.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <vector>

namespace prisma::pipeline {

struct CompiledPass {
	std::string name;
	GLuint program = 0;
	GLint color_tex = -1, depth_tex = -1, shadow_tex = -1, shadow_tex_prev = -1, gbuffer_tex = -1, target_size = -1;
	GLint sky_tex = -1, gbuffer_light_tex = -1, overhead_tex = -1, gbuffer_material_tex = -1;
	int target = -1; // index into g_buffers; -1 = the scene
	bool compute = false;
	GLint workgroup[3] = {1, 1, 1}; // local size of a compute pass
	std::vector<GLint> buffer_tex;
	api::Locations api{};
	options::Locations opts;
};

// ---------------------------------------------------------------- state

static std::filesystem::path g_packs_dir, g_pack_dir, g_ini;
static std::string g_pack_name;
static bool g_pack_loaded = false; // a version of the pack is in use
static bool g_enabled = true;  // the on/off key
static bool g_bypass = false;  // [debug] blink
static bool g_applied = true;  // what the game shaders currently have
static bool g_rebuild_requested = false;

static pack::Pack g_pack; // the game shaders point at this one
static std::string g_signature;
static double g_next_check = 0;

static bool g_gl_ready = false;
static bool g_gl_failed = false;
static GLuint g_vao = 0, g_vs = 0;
static GLuint g_copy_tex = 0, g_copy_fbo = 0, g_out_fbo = 0;
static GLuint g_out_attached = 0;
static GLsizei g_copy_w = 0, g_copy_h = 0;

static std::vector<CompiledPass> g_passes;

// The pack's buffers: two textures each, so a pass can read what the buffer
// held (earlier this frame, or last frame) while writing its new contents.
struct RuntimeBuffer {
	pack::Buffer def;
	GLuint tex[2] = {}, fbo[2] = {};
	int current = 0; // the texture with the latest contents
	GLsizei width = 0, height = 0;
};
static std::vector<RuntimeBuffer> g_buffers;
static bool g_buffers_dirty = true;
constexpr int k_first_buffer_unit = 5; // 0..4: colour, depth, shadow maps (2), G-buffer
// skyTex goes after the 11 buffers a pack may have: it needs a 17th texture
// unit, which every current GPU has (OpenGL 3.2 only promises 16). Without
// it passes get the scene itself as skyTex, so skyOverlay() is zero.
constexpr int k_sky_unit = k_first_buffer_unit + 11;
static bool g_sky_unit_ok = false;
// gbufferLightTex and overheadTex after it (19 units; current GPUs have 32)
constexpr int k_light_unit = k_sky_unit + 1;
constexpr int k_overhead_unit = k_sky_unit + 2;
static bool g_extra_units_ok = false;
// and gbufferMaterialTex (20 units)
constexpr int k_material_unit = k_sky_unit + 3;
static bool g_material_unit_ok = false;

// The scene as it was when Luanti finished drawing the sky (see capture_sky)
static GLuint g_sky_fbo = 0, g_sky_tex = 0;
static GLsizei g_sky_w = 0, g_sky_h = 0;
static bool g_sky_valid = false; // captured this frame
static bool g_passes_dirty = true;
static bool g_depth_zero_to_one = false;

static void log_block(const std::string &text)
{
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line))
		if (!line.empty())
			log::info("    %s", line.c_str());
}

// ---------------------------------------------------------------- pack files

// Reads the pack from disk. On errors the previous version stays in use.
static bool load_pack()
{
	if (g_pack_name.empty())
		return false; // no pack installed
	g_signature = pack::signature(g_pack_dir);
	log::clear_recent_errors(); // the menu shows the errors of this version
	pack::Pack p = pack::load(g_pack_dir);
	for (const std::string &e : p.errors)
		log::error("%s", e.c_str());
	if (!p.errors.empty()) {
		log::error(g_pack_loaded ? "The pack has errors; the previous version is kept"
							: "The pack has errors and is not used");
		return false;
	}
	for (const std::string &w : p.warnings)
		log::info("Warning: %s", w.c_str());
	g_pack = std::move(p);
	g_pack_loaded = true;
	g_buffers_dirty = true;
	audio::set_pack(g_pack);
	options::set_pack(g_pack, g_ini);
	gameshaders::set_pack(&g_pack, g_applied);
	g_passes_dirty = true;

	std::string game;
	for (auto &[name, gs] : g_pack.game)
		game += (game.empty() ? "" : ", ") + name;
	log::info("Pack %s: %zu pass(es); game shaders: %s", g_pack.name.c_str(), g_pack.passes.size(),
		game.empty() ? "none" : game.c_str());
	return true;
}

void init(const std::filesystem::path &base_dir)
{
	g_ini = base_dir / "prisma.ini";
	// Where packs live: next to Prisma unless prisma.ini says otherwise.
	std::string packs = ini::get(g_ini, "prisma", "packs");
	g_packs_dir = packs.empty() ? base_dir / "shaderpacks" : utf8_path(packs);
	g_pack_name = ini::get(g_ini, "prisma", "pack");
	// No pack chosen, or the chosen one is not installed: the first one there
	// is, as the launcher does
	std::vector<std::string> installed = available_packs();
	if (std::find(installed.begin(), installed.end(), g_pack_name) == installed.end()) {
		if (!g_pack_name.empty())
			log::info("The pack %s is not installed", g_pack_name.c_str());
		g_pack_name = installed.empty() ? "" : installed.front();
	}
	g_pack_dir = g_packs_dir / utf8_path(g_pack_name);
	if (g_pack_name.empty())
		log::info("No pack installed in %s: the game looks as it does without Prisma",
			to_utf8(g_packs_dir).c_str());
	else
		log::info("Active pack: %s (%s)", g_pack_name.c_str(), to_utf8(g_pack_dir).c_str());
	audio::init(g_ini);
	// Before the game compiles any shader, so the pack applies from the start.
	load_pack();
}

// ---------------------------------------------------------------- GL helpers

static std::string shader_log(GLuint shader)
{
	GLint len = 0;
	gl::GetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
	std::string s(len > 1 ? len : 1, '\0');
	gl::GetShaderInfoLog(shader, (GLsizei)s.size(), nullptr, s.data());
	return s.c_str();
}

static std::string program_log(GLuint program)
{
	GLint len = 0;
	gl::GetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
	std::string s(len > 1 ? len : 1, '\0');
	gl::GetProgramInfoLog(program, (GLsizei)s.size(), nullptr, s.data());
	return s.c_str();
}

static GLuint compile(GLenum type, const std::vector<const char *> &sources, std::string &error)
{
	GLuint sh = gl::CreateShader(type);
	gl::ShaderSource(sh, (GLsizei)sources.size(), sources.data(), nullptr);
	gl::CompileShader(sh);
	GLint ok = 0;
	gl::GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		error = shader_log(sh);
		gl::DeleteShader(sh);
		return 0;
	}
	return sh;
}

static bool setup_gl()
{
	if (g_gl_ready || g_gl_failed)
		return g_gl_ready;
	if (!gl::load()) {
		g_gl_failed = true;
		log::error("OpenGL functions are missing: Prisma will not draw anything");
		return false;
	}
	GLint profile = 0;
	gl::GetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
	gl::GetError(); // the query is invalid on some contexts; don't leave the error behind
	log::info("OpenGL context: %s profile", profile & 1 ? "core" : profile & 2 ? "compatibility" : "unknown");

	GLint units = 0;
	gl::GetIntegerv(0x8872 /* GL_MAX_TEXTURE_IMAGE_UNITS */, &units);
	g_sky_unit_ok = units > k_sky_unit;
	g_extra_units_ok = units > k_overhead_unit;
	g_material_unit_ok = units > k_material_unit;
	if (!g_sky_unit_ok)
		log::info("The graphics card has %d texture units: skyTex will not be available", units);
	if (!g_extra_units_ok)
		log::info("The graphics card has %d texture units: gbufferLightTex and overheadTex will not be available",
			units);
	if (g_extra_units_ok && !g_material_unit_ok)
		log::info("The graphics card has %d texture units: gbufferMaterialTex will not be available", units);

	gl::GenVertexArrays(1, &g_vao);
	gl::GenFramebuffers(1, &g_copy_fbo);
	gl::GenFramebuffers(1, &g_out_fbo);
	std::string error;
	g_vs = compile(GL_VERTEX_SHADER, {passglsl::VERTEX}, error);
	if (!g_vs) {
		g_gl_failed = true;
		log::error("The internal vertex shader does not compile:");
		log_block(error);
		return false;
	}
	g_gl_ready = true;
	return true;
}

// Compiles every pass. On any error nothing is replaced, so the previous
// version keeps running while the author fixes it.
static void build_passes(bool depth_zero_to_one)
{
	g_passes_dirty = false;
	g_depth_zero_to_one = depth_zero_to_one;

	const bool compute = gl::has_compute();
	std::string header = passglsl::fragment_header(depth_zero_to_one, g_pack.buffers, compute);
	static bool told = false;
	if (pack::uses_compute(g_pack) && !compute && !told) {
		told = true;
		log::info("This OpenGL context has no compute shaders (4.3 is needed): .comp passes do not "
				  "run, but those marked #prisma fallback do");
	}

	std::vector<CompiledPass> built;
	bool ok = true;
	for (const pack::Pass &pass : g_pack.passes) {
		// A .comp where it can run, its fallback where it can't
		if (pass.compute != compute && (pass.compute || pass.fallback))
			continue;
		std::string error;
		GLuint shader = 0;
		if (pass.compute) {
			std::string comp_header = passglsl::compute_header(depth_zero_to_one, g_pack.buffers, pass);
			shader = compile(GL_COMPUTE_SHADER, {comp_header.c_str(), pass.code.code.c_str()}, error);
		} else {
			shader = compile(GL_FRAGMENT_SHADER, {header.c_str(), pass.code.code.c_str()}, error);
		}
		if (!shader) {
			log::error("%s does not compile:", pass.name.c_str());
			log_block(pack::translate_log(error, pass.code.files));
			ok = false;
			continue;
		}
		GLuint prog = gl::CreateProgram();
		if (!pass.compute)
			gl::AttachShader(prog, g_vs);
		gl::AttachShader(prog, shader);
		gl::LinkProgram(prog);
		gl::DeleteShader(shader);
		GLint linked = 0;
		gl::GetProgramiv(prog, GL_LINK_STATUS, &linked);
		if (!linked) {
			log::error("%s does not link:", pass.name.c_str());
			log_block(pack::translate_log(program_log(prog), pass.code.files));
			gl::DeleteProgram(prog);
			ok = false;
			continue;
		}
		CompiledPass cp;
		cp.name = pass.name;
		cp.program = prog;
		cp.color_tex = gl::GetUniformLocation(prog, "colorTex");
		cp.depth_tex = gl::GetUniformLocation(prog, "depthTex");
		cp.shadow_tex = gl::GetUniformLocation(prog, "shadowTex");
		cp.gbuffer_tex = gl::GetUniformLocation(prog, "gbufferTex");
		cp.shadow_tex_prev = gl::GetUniformLocation(prog, "shadowTexPrev");
		cp.sky_tex = gl::GetUniformLocation(prog, "skyTex");
		cp.gbuffer_light_tex = gl::GetUniformLocation(prog, "gbufferLightTex");
		cp.overhead_tex = gl::GetUniformLocation(prog, "overheadTex");
		cp.gbuffer_material_tex = gl::GetUniformLocation(prog, "gbufferMaterialTex");
		cp.target_size = gl::GetUniformLocation(prog, "targetSize");
		for (const pack::Buffer &b : g_pack.buffers)
			cp.buffer_tex.push_back(gl::GetUniformLocation(prog, b.id.c_str()));
		for (size_t i = 0; i < g_pack.buffers.size(); i++)
			if (g_pack.buffers[i].id == pass.target)
				cp.target = (int)i;
		cp.compute = pass.compute;
		if (cp.compute)
			gl::GetProgramiv(prog, GL_COMPUTE_WORK_GROUP_SIZE, cp.workgroup);
		cp.api.query(prog);
		cp.opts.query(prog);
		built.push_back(cp);
	}

	if (!ok) {
		for (auto &cp : built)
			gl::DeleteProgram(cp.program);
		log::error("The passes have errors; the previous version is kept");
		return;
	}
	for (auto &cp : g_passes)
		gl::DeleteProgram(cp.program);
	g_passes = std::move(built);
	if (!g_passes.empty()) {
		std::string names;
		for (auto &cp : g_passes)
			names += (names.empty() ? "" : ", ") + cp.name;
		log::info("Compiled passes: %s", names.c_str());
	}
}

// ---------------------------------------------------------------- GL state

static const GLenum k_caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST,
	GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_FRAMEBUFFER_SRGB, GL_RASTERIZER_DISCARD};
// colour, depth, shadow maps, G-buffer, the pack's buffers, sky, G-buffer light, overhead map, materials
constexpr int k_units = k_material_unit + 1;
static int saved_units()
{
	return g_material_unit_ok ? k_units : g_extra_units_ok ? k_overhead_unit + 1 : g_sky_unit_ok ? k_sky_unit + 1 :
		k_sky_unit;
}

// Luanti's renderer caches GL state, so everything we touch must be put back
// exactly as it was.
struct SavedState {
	GLint program, vao, draw_fbo, read_fbo, active_texture, array_buffer, renderbuffer, depth_func;
	GLint uniform_buffer, joint_buffer; // generic binding, and binding point 15 (lighting.cpp)
	GLint pixel_pack_buffer;
	GLint cull_face, front_face;
	long long joint_start, joint_size;
	GLint textures[k_units], array_texture;
	GLint viewport[4];
	GLfloat depth_clear, color_clear[4];
	GLboolean depth_mask, color_mask[4];
	GLboolean caps[sizeof(k_caps) / sizeof(k_caps[0])];

	void save()
	{
		gl::GetIntegerv(GL_CURRENT_PROGRAM, &program);
		gl::GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
		gl::GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
		gl::GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
		gl::GetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
		gl::GetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
		gl::GetIntegerv(GL_UNIFORM_BUFFER_BINDING, &uniform_buffer);
		gl::GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pixel_pack_buffer);
		gl::GetIntegerv(GL_CULL_FACE_MODE, &cull_face);
		gl::GetIntegerv(GL_FRONT_FACE, &front_face);
		gl::GetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 15, &joint_buffer);
		gl::GetInteger64i_v(GL_UNIFORM_BUFFER_START, 15, &joint_start);
		gl::GetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, 15, &joint_size);
		gl::GetIntegerv(GL_DEPTH_FUNC, &depth_func);
		gl::GetFloatv(GL_DEPTH_CLEAR_VALUE, &depth_clear);
		gl::GetFloatv(GL_COLOR_CLEAR_VALUE, color_clear);
		gl::GetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
		for (int u = 0; u < saved_units(); u++) {
			gl::ActiveTexture(GL_TEXTURE0 + u);
			gl::GetIntegerv(GL_TEXTURE_BINDING_2D, &textures[u]);
			if (u == 0)
				gl::GetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &array_texture);
		}
		gl::GetIntegerv(GL_VIEWPORT, viewport);
		gl::GetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
		gl::GetBooleanv(GL_COLOR_WRITEMASK, color_mask);
		for (size_t i = 0; i < std::size(k_caps); i++)
			caps[i] = gl::IsEnabled(k_caps[i]);
	}

	void restore()
	{
		for (size_t i = 0; i < std::size(k_caps); i++)
			caps[i] ? gl::Enable(k_caps[i]) : gl::Disable(k_caps[i]);
		gl::ColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
		gl::DepthMask(depth_mask);
		gl::DepthFunc(depth_func);
		gl::ClearDepth(depth_clear);
		gl::ClearColor(color_clear[0], color_clear[1], color_clear[2], color_clear[3]);
		gl::Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		for (int u = 0; u < saved_units(); u++) {
			gl::ActiveTexture(GL_TEXTURE0 + u);
			gl::BindTexture(GL_TEXTURE_2D, textures[u]);
			if (u == 0)
				gl::BindTexture(GL_TEXTURE_2D_ARRAY, array_texture);
		}
		gl::BindBuffer(GL_ARRAY_BUFFER, array_buffer);
		gl::BindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
		gl::CullFace(cull_face);
		gl::FrontFace(front_face);
		if (joint_size > 0)
			gl::BindBufferRange(GL_UNIFORM_BUFFER, 15, joint_buffer, (GLintptr)joint_start, (GLsizeiptr)joint_size);
		else
			gl::BindBufferBase(GL_UNIFORM_BUFFER, 15, joint_buffer);
		gl::BindBuffer(GL_UNIFORM_BUFFER, uniform_buffer);
		gl::BindBuffer(GL_PIXEL_PACK_BUFFER, pixel_pack_buffer);
		gl::ActiveTexture(active_texture);
		gl::BindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
		gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
		gl::BindVertexArray(vao);
		gl::UseProgram(program);
	}
};

// Our copy of the scene colour, and an FBO that writes into the scene's own
// colour texture (without its depth attached, so depth can be sampled).
static bool prepare_targets(const Scene &scene)
{
	const GLsizei w = scene.color.width, h = scene.color.height;
	if (!g_copy_tex || g_copy_w != w || g_copy_h != h) {
		if (g_copy_tex)
			gl::DeleteTextures(1, &g_copy_tex);
		gl::GenTextures(1, &g_copy_tex);
		gl::ActiveTexture(GL_TEXTURE0);
		gl::BindTexture(GL_TEXTURE_2D, g_copy_tex);
		gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
		gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
		gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_copy_tex, 0);
		if (gl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			log::error("The copy framebuffer is incomplete");
			return false;
		}
		g_copy_w = w;
		g_copy_h = h;
		log::info("Prisma textures created at %dx%d", w, h);
	}
	if (g_out_attached != scene.color.texture) {
		gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_out_fbo);
		gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene.color.texture, 0);
		if (gl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			log::error("Can't draw onto the scene texture (tex %u)", scene.color.texture);
			g_out_attached = 0;
			return false;
		}
		g_out_attached = scene.color.texture;
	}
	return true;
}

static GLenum internal_format(const std::string &f)
{
	return f == "rgba8" ? GL_RGBA8 : f == "rgba32f" ? 0x8814 /* GL_RGBA32F */ : GL_RGBA16F;
}

static void free_buffers()
{
	for (RuntimeBuffer &b : g_buffers) {
		gl::DeleteTextures(2, b.tex);
		gl::DeleteFramebuffers(2, b.fbo);
	}
	g_buffers.clear();
}

// (Re)creates the buffers when the pack or the scene size changes. Their
// contents start black.
static bool prepare_buffers(GLsizei scene_w, GLsizei scene_h)
{
	bool ok = true;
	if (g_buffers_dirty) {
		g_buffers_dirty = false;
		free_buffers();
		for (const pack::Buffer &def : g_pack.buffers)
			g_buffers.push_back(RuntimeBuffer{def});
	}
	for (RuntimeBuffer &b : g_buffers) {
		GLsizei w = b.def.width > 0 ? b.def.width : std::max(1, (int)std::lround(scene_w * b.def.scale));
		GLsizei h = b.def.height > 0 ? b.def.height : std::max(1, (int)std::lround(scene_h * b.def.scale));
		if (b.tex[0] && b.width == w && b.height == h)
			continue;
		if (b.tex[0]) {
			gl::DeleteTextures(2, b.tex);
			gl::DeleteFramebuffers(2, b.fbo);
		}
		gl::GenTextures(2, b.tex);
		gl::GenFramebuffers(2, b.fbo);
		GLenum filter = b.def.linear ? GL_LINEAR : GL_NEAREST;
		bool mipmaps = b.def.mipmaps && gl::has_mipmaps();
		GLenum min_filter = !mipmaps ? filter : b.def.linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
		for (int k = 0; k < 2; k++) {
			gl::ActiveTexture(GL_TEXTURE0);
			gl::BindTexture(GL_TEXTURE_2D, b.tex[k]);
			gl::TexImage2D(GL_TEXTURE_2D, 0, internal_format(b.def.format), w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
			gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
			gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
			gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, b.fbo[k]);
			gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.tex[k], 0);
			if (gl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				log::error("The pack's buffer %s can't be created", b.def.id.c_str());
				ok = false;
			}
			gl::ColorMask(1, 1, 1, 1);
			gl::ClearColor(0, 0, 0, 0);
			gl::Clear(GL_COLOR_BUFFER_BIT);
			if (mipmaps)
				gl::GenerateMipmap(GL_TEXTURE_2D); // the smaller levels must exist to be sampled
		}
		b.width = w;
		b.height = h;
		b.current = 0;
	}
	return ok;
}

void on_texture_deleted(GLuint texture)
{
	// The scene texture goes away when the player leaves the world (and on
	// resizes). Its number may come back for the next one, so drop the
	// attachment now; otherwise Prisma would keep drawing into a dead texture.
	if (texture != 0 && texture == g_out_attached)
		g_out_attached = 0;
}

// ---------------------------------------------------------------- render

// A number from pack.toml, or the current value of the option it names.
static double setting(const pack::Setting &s)
{
	if (s.option.empty())
		return s.number;
	const auto &defs = options::defs();
	for (size_t i = 0; i < defs.size(); i++) {
		if (defs[i].id != s.option)
			continue;
		double v = options::value(i);
		if (defs[i].type == pack::OptionDef::Choice) {
			// The number the pack gives the chosen value, or its text read as one ("2048")
			size_t k = (size_t)v;
			if (k < defs[i].numbers.size())
				return defs[i].numbers[k];
			return k < defs[i].values.size() ? std::atof(defs[i].values[k].c_str()) : 0;
		}
		return v;
	}
	return s.number;
}

// ---------------------------------------------------------------- sound

// The pack's sound buffer (audio.hpp) goes to the CPU through a ring of
// pixel buffers and is used a couple of frames later, so reading it never
// makes the CPU wait for the GPU.
constexpr int k_sound_ring = 3;
static GLuint g_sound_pbo[k_sound_ring] = {};
static int g_sound_bytes[k_sound_ring] = {}; // what each holds (0 = nothing yet)
static int g_sound_w[k_sound_ring] = {}, g_sound_h[k_sound_ring] = {};
static int g_sound_slot = 0;
static std::vector<float> g_sound_texels;

static void read_sound_buffer()
{
	if (g_pack.sounds.empty())
		return;
	const RuntimeBuffer *b = nullptr;
	for (const RuntimeBuffer &rb : g_buffers)
		if (rb.def.id == g_pack.sound_buffer)
			b = &rb;
	if (!b || b->width <= 0 || b->height <= 0)
		return;
	if (!g_sound_pbo[0])
		gl::GenBuffers(k_sound_ring, g_sound_pbo);

	// This frame's copy, queued on the GPU
	const int bytes = b->width * b->height * 4 * (int)sizeof(float);
	int slot = g_sound_slot;
	gl::BindBuffer(GL_PIXEL_PACK_BUFFER, g_sound_pbo[slot]);
	if (g_sound_bytes[slot] != bytes)
		gl::BufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
	gl::BindFramebuffer(GL_READ_FRAMEBUFFER, b->fbo[b->current]);
	gl::ReadPixels(0, 0, b->width, b->height, GL_RGBA, GL_FLOAT, nullptr);
	g_sound_bytes[slot] = bytes;
	g_sound_w[slot] = b->width;
	g_sound_h[slot] = b->height;

	// The oldest one, finished by now
	int oldest = (slot + 1) % k_sound_ring;
	g_sound_slot = oldest;
	if (g_sound_bytes[oldest] > 0) {
		gl::BindBuffer(GL_PIXEL_PACK_BUFFER, g_sound_pbo[oldest]);
		if (const void *data = gl::MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, g_sound_bytes[oldest], GL_MAP_READ_BIT)) {
			const float *f = static_cast<const float *>(data);
			g_sound_texels.assign(f, f + g_sound_bytes[oldest] / sizeof(float));
			gl::UnmapBuffer(GL_PIXEL_PACK_BUFFER);
			audio::update(g_sound_texels.data(), g_sound_w[oldest], g_sound_h[oldest]);
		}
	}
	gl::BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

// Every texture a pass can read, for the texture viewer (debugview.hpp).
// Unavailable ones are listed too, with texture 0.
static std::vector<debugview::Source> debug_sources(const Scene &scene, const GameState &s)
{
	using debugview::Source;
	std::vector<Source> list;
	auto add = [&](const std::string &name, GLuint texture, debugview::Kind kind, int w, int h, int levels = 1) {
		list.push_back(Source{name, w > 0 && h > 0 ? texture : 0, kind, w, h, levels});
	};
	const int w = scene.color.width, h = scene.color.height;
	bool sky = g_sky_valid && g_sky_w == w && g_sky_h == h;
	add("Scene (colorTex)", scene.color.texture, debugview::KIND_COLOR, w, h);
	add("Depth (depthTex)", scene.depth.texture, debugview::KIND_DEPTH, w, h);
	add("Sky (skyTex)", sky ? g_sky_tex : 0, debugview::KIND_COLOR, w, h);
	add("G-buffer: normal and sun (gbufferTex)", lighting::gbuffer_texture(), debugview::KIND_COLOR, s.width, s.height);
	add("G-buffer: light (gbufferLightTex)", lighting::gbuffer_light_texture(), debugview::KIND_COLOR, s.width,
		s.height);
	add("G-buffer: material (gbufferMaterialTex)", lighting::gbuffer_material_texture(), debugview::KIND_MATERIAL,
		s.width, s.height);
	add("Shadows (shadowTex)", lighting::shadow_texture(), debugview::KIND_DEPTH_MAP, s.shadow_size, s.shadow_size);
	add("Previous shadows (shadowTexPrev)", lighting::shadow_texture_prev(), debugview::KIND_DEPTH_MAP,
		s.shadow_size, s.shadow_size);
	add("Overhead map (overheadTex)", lighting::overhead_texture(), debugview::KIND_DEPTH_MAP, s.overhead_size,
		s.overhead_size);
	for (const RuntimeBuffer &b : g_buffers) {
		int levels = 1;
		if (b.def.mipmaps && gl::has_mipmaps())
			while ((std::max(b.width, b.height) >> levels) > 0)
				levels++;
		add("Buffer " + b.def.id, b.tex[b.current], debugview::KIND_COLOR, b.width, b.height, levels);
	}
	return list;
}

void render(const Scene &scene, const GameState &game_state)
{
	if (!g_enabled || g_bypass)
		return;
	static bool warned_scene = false;
	if (!scene.color.texture || !scene.depth.texture || scene.color.width <= 0) {
		// Drawn straight to the window: from the next frame on Prisma's
		// stand-in screen (screen.cpp) takes its place, nothing to report.
		if (!warned_scene && scene.fbo != 0) {
			warned_scene = true;
			log::error("The scene is not drawn to textures (FBO %u): Prisma can't apply the pack.", scene.fbo);
		}
		return;
	}
	if (!setup_gl())
		return;
	if (g_passes_dirty || game_state.depth_zero_to_one != g_depth_zero_to_one)
		build_passes(game_state.depth_zero_to_one);
	if (g_passes.empty())
		return;
	debugview::frame_begin();

	SavedState saved;
	saved.save();

	// Prisma's own shadow map and G-buffer, drawn before the passes
	GameState state = game_state;
	lighting::Request request;
	request.shadows = g_pack.shadows;
	request.resolution = std::clamp((int)setting(g_pack.shadow_resolution), 256, 8192);
	request.distance = (float)std::clamp(setting(g_pack.shadow_distance), 8.0, 1000.0);
	request.gbuffer = g_pack.gbuffer;
	request.gbuffer_distance = (float)std::max(setting(g_pack.gbuffer_distance), 0.0);
	request.overhead = g_pack.overhead;
	request.overhead_resolution = std::clamp((int)setting(g_pack.overhead_resolution), 128, 4096);
	request.overhead_distance = (float)std::clamp(setting(g_pack.overhead_distance), 8.0, 512.0);
	// Under a sky the game hides there is no sun; a pack can bring its own
	// light ([shadows] hidden_sky_light): the shadow maps are drawn
	// from it and the passes get it as lightDirection
	if (state.sky_hidden && g_pack.shadows && g_pack.hidden_sky_light) {
		Vec3 L = Vec3{(float)g_pack.hidden_sky_dir[0], (float)g_pack.hidden_sky_dir[1],
			(float)g_pack.hidden_sky_dir[2]}.normalized();
		state.light_dir = state.light_body = state.sun_dir = L;
		state.moon_dir = -L;
		request.hidden_light = true;
	}
	// From a known state: Luanti may leave polygon offset (its own shadow
	// pass), alpha-to-coverage, colour masks... enabled.
	for (size_t i = 0; i < std::size(k_caps); i++)
		gl::Disable(k_caps[i]);
	gl::ColorMask(1, 1, 1, 1);
	lighting::render(request, state);
	debugview::set_state(state);

	if (prepare_targets(scene) && prepare_buffers(scene.color.width, scene.color.height)) {
		const GLsizei w = scene.color.width, h = scene.color.height;
		for (size_t i = 0; i < std::size(k_caps); i++)
			gl::Disable(k_caps[i]);
		gl::DepthMask(0);
		gl::BindVertexArray(g_vao);

		bool copy_valid = false; // g_copy_tex holds the scene as it is now
		for (const CompiledPass &p : g_passes) {
			debugview::time_begin(p.name);
			if (!copy_valid) {
				gl::BindFramebuffer(GL_READ_FRAMEBUFFER, g_out_fbo);
				gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
				gl::BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
				copy_valid = true;
			}

			RuntimeBuffer *target = p.target >= 0 && p.target < (int)g_buffers.size() ? &g_buffers[p.target] : nullptr;
			if (p.compute && target) {
				// In place: the image is the buffer's latest texture
				gl::BindImageTexture(0, target->tex[target->current], 0, 0, 0, GL_READ_WRITE,
					internal_format(target->def.format));
			} else if (target) {
				gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, target->fbo[1 - target->current]);
				gl::Viewport(0, 0, target->width, target->height);
				gl::ColorMask(1, 1, 1, 1);
			} else {
				gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_out_fbo);
				gl::Viewport(0, 0, w, h);
				// Alpha is left alone: Luanti's later passes may rely on it.
				gl::ColorMask(1, 1, 1, 0);
			}
			gl::UseProgram(p.program);
			for (size_t i = 0; i < g_buffers.size() && i < p.buffer_tex.size(); i++) {
				gl::ActiveTexture(GL_TEXTURE0 + k_first_buffer_unit + (GLenum)i);
				gl::BindTexture(GL_TEXTURE_2D, g_buffers[i].tex[g_buffers[i].current]);
				if (p.buffer_tex[i] >= 0)
					gl::Uniform1i(p.buffer_tex[i], k_first_buffer_unit + (GLint)i);
			}
			if (p.target_size >= 0)
				gl::Uniform2f(p.target_size, (float)(target ? target->width : w), (float)(target ? target->height : h));
			gl::ActiveTexture(GL_TEXTURE0);
			gl::BindTexture(GL_TEXTURE_2D, g_copy_tex);
			gl::ActiveTexture(GL_TEXTURE0 + 1);
			gl::BindTexture(GL_TEXTURE_2D, scene.depth.texture);
			gl::ActiveTexture(GL_TEXTURE0 + 2);
			gl::BindTexture(GL_TEXTURE_2D, lighting::shadow_texture());
			gl::ActiveTexture(GL_TEXTURE0 + 3);
			gl::BindTexture(GL_TEXTURE_2D, lighting::gbuffer_texture());
			gl::ActiveTexture(GL_TEXTURE0 + 4);
			gl::BindTexture(GL_TEXTURE_2D, lighting::shadow_texture_prev());
			if (p.shadow_tex_prev >= 0)
				gl::Uniform1i(p.shadow_tex_prev, 4);
			if (g_sky_unit_ok) {
				gl::ActiveTexture(GL_TEXTURE0 + k_sky_unit);
				// Without a capture of the right size, the scene itself: no overlay
				bool sky = g_sky_valid && g_sky_w == w && g_sky_h == h;
				gl::BindTexture(GL_TEXTURE_2D, sky ? g_sky_tex : g_copy_tex);
			}
			if (p.sky_tex >= 0)
				gl::Uniform1i(p.sky_tex, g_sky_unit_ok ? k_sky_unit : 0);
			if (g_extra_units_ok) {
				gl::ActiveTexture(GL_TEXTURE0 + k_light_unit);
				gl::BindTexture(GL_TEXTURE_2D, lighting::gbuffer_light_texture());
				gl::ActiveTexture(GL_TEXTURE0 + k_overhead_unit);
				gl::BindTexture(GL_TEXTURE_2D, lighting::overhead_texture());
			}
			// Without the units, unit 4 (a depth texture or nothing): reads as "no data"
			if (p.gbuffer_light_tex >= 0)
				gl::Uniform1i(p.gbuffer_light_tex, g_extra_units_ok ? k_light_unit : 4);
			if (p.overhead_tex >= 0)
				gl::Uniform1i(p.overhead_tex, g_extra_units_ok ? k_overhead_unit : 4);
			if (g_material_unit_ok) {
				gl::ActiveTexture(GL_TEXTURE0 + k_material_unit);
				gl::BindTexture(GL_TEXTURE_2D, lighting::gbuffer_material_texture());
			}
			if (p.gbuffer_material_tex >= 0)
				gl::Uniform1i(p.gbuffer_material_tex, g_material_unit_ok ? k_material_unit : 4);
			if (p.color_tex >= 0)
				gl::Uniform1i(p.color_tex, 0);
			if (p.depth_tex >= 0)
				gl::Uniform1i(p.depth_tex, 1);
			if (p.shadow_tex >= 0)
				gl::Uniform1i(p.shadow_tex, 2);
			if (p.gbuffer_tex >= 0)
				gl::Uniform1i(p.gbuffer_tex, 3);
			api::upload(p.api, state);
			options::upload(p.opts);
			if (p.compute && target) {
				auto groups = [](GLsizei size, GLint local) { return (GLuint)((size + local - 1) / std::max(local, 1)); };
				gl::DispatchCompute(groups(target->width, p.workgroup[0]), groups(target->height, p.workgroup[1]), 1);
				// What it wrote, visible to every later read (samplers, images, read-backs)
				gl::MemoryBarrier(GL_ALL_BARRIER_BITS);
				gl::BindImageTexture(0, 0, 0, 0, 0, GL_READ_WRITE, GL_RGBA16F);
			} else {
				gl::DrawArrays(GL_TRIANGLES, 0, 3);
			}
			if (target) {
				if (!p.compute)
					target->current = 1 - target->current;
				if (target->def.mipmaps && gl::has_mipmaps()) {
					gl::ActiveTexture(GL_TEXTURE0);
					gl::BindTexture(GL_TEXTURE_2D, target->tex[target->current]);
					gl::GenerateMipmap(GL_TEXTURE_2D);
				}
			} else {
				copy_valid = false;
			}
			debugview::time_end(); // the pass's cost includes its mipmaps
		}
		debugview::frame_end();
		read_sound_buffer();
		if (debugview::viewer_wanted())
			debugview::render(debug_sources(scene, state), state);
	}

	saved.restore();
}

// ---------------------------------------------------------------- control

static bool g_switch_requested = false;

static void rebuild_game_shaders()
{
	gameshaders::rebuild_all();
	geometry::reapply_overrides();
}

// The game shaders are rebuilt whenever what should be applied changes.
static void sync_game_shaders()
{
	bool want = g_enabled && !g_bypass;
	geometry::set_record_draws(want && (g_pack.shadows || g_pack.gbuffer || g_pack.overhead));
	geometry::set_hide_clouds(want && g_pack.hide_clouds);
	geometry::set_hide_shadows(want && g_pack.hide_shadows);
	geometry::set_hide_volumetric_light(want && g_pack.hide_volumetric_light);
	frame::set_hide_underwater(want && g_pack.hide_underwater);
	if (want == g_applied)
		return;
	g_applied = want;
	gameshaders::set_pack(&g_pack, g_applied);
	rebuild_game_shaders();
}

bool active()
{
	return g_enabled && !g_bypass;
}

bool enabled()
{
	return g_enabled;
}

void set_enabled(bool on)
{
	if (on == g_enabled)
		return;
	g_enabled = on;
	log::info("Prisma %s", g_enabled ? "on" : "off");
}

void set_bypass(bool bypass)
{
	g_bypass = bypass;
}

const pack::Pack &current_pack()
{
	return g_pack;
}

const std::string &selected_pack()
{
	return g_pack_name;
}

std::vector<std::string> available_packs()
{
	namespace fs = std::filesystem;
	std::vector<std::string> names;
	std::error_code ec;
	for (auto &e : fs::directory_iterator(g_packs_dir, ec)) {
		if (!e.is_directory())
			continue;
		const fs::path &p = e.path();
		if (fs::exists(p / "pack.toml") || fs::is_directory(p / "passes") || fs::is_directory(p / "game"))
			names.push_back(to_utf8(p.filename()));
	}
	std::sort(names.begin(), names.end());
	return names;
}

void select_pack(const std::string &name)
{
	if (name == g_pack_name)
		return;
	g_pack_name = name;
	g_pack_dir = g_packs_dir / utf8_path(name);
	ini::set(g_ini, "prisma", "pack", name);
	log::info("Active pack: %s", name.c_str());
	g_switch_requested = true; // loaded at the end of the frame
}

void options_changed(bool recompile)
{
	if (recompile)
		g_rebuild_requested = true;
}

void open_pack_folder()
{
	platform::open_folder(g_pack_dir);
}

void capture_sky(GLint x, GLint y, GLsizei w, GLsizei h)
{
	if (!g_enabled || g_bypass || w <= 0 || h <= 0 || !setup_gl() || !g_sky_unit_ok)
		return;
	GLint draw = 0, read = 0, texture = 0;
	gl::GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
	gl::GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
	if (draw == 0)
		return; // the real window: nothing to read back reliably
	if (!g_sky_tex || g_sky_w != w || g_sky_h != h) {
		gl::GetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
		if (!g_sky_tex) {
			gl::GenTextures(1, &g_sky_tex);
			gl::GenFramebuffers(1, &g_sky_fbo);
		}
		gl::BindTexture(GL_TEXTURE_2D, g_sky_tex);
		gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
		gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl::BindTexture(GL_TEXTURE_2D, texture);
		gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sky_fbo);
		gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sky_tex, 0);
		g_sky_w = w;
		g_sky_h = h;
	}
	// A blit honours the scissor test
	GLboolean scissor = gl::IsEnabled(GL_SCISSOR_TEST);
	gl::Disable(GL_SCISSOR_TEST);
	gl::BindFramebuffer(GL_READ_FRAMEBUFFER, draw);
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sky_fbo);
	gl::BlitFramebuffer(x, y, x + w, y + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
	gl::BindFramebuffer(GL_READ_FRAMEBUFFER, read);
	if (scissor)
		gl::Enable(GL_SCISSOR_TEST);
	g_sky_valid = true;
}

void on_frame_end()
{
	g_sky_valid = false;
	audio::on_frame_end();
	sync_game_shaders();

	if (g_switch_requested) {
		g_switch_requested = false;
		if (load_pack())
			rebuild_game_shaders();
	}
	if (g_rebuild_requested) {
		// A #define option changed: everything that includes the options
		g_rebuild_requested = false;
		g_passes_dirty = true;
		rebuild_game_shaders();
	}

	// Reload on save, in developer mode only: it
	// reads the pack's folder twice a second on this thread.
	if (developer::enabled() && !g_pack_name.empty() && uptime() >= g_next_check) {
		g_next_check = uptime() + 0.5;
		if (pack::signature(g_pack_dir) != g_signature) {
			log::info("Pack changed, reloading");
			if (load_pack())
				rebuild_game_shaders();
		}
	}
}

} // namespace prisma::pipeline
