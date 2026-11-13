// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/render/gameshaders.hpp"
#include "core/render/shader_compose.hpp"
#include "core/render/lighting.hpp"
#include "core/dev/trace.hpp"
#include "core/pack/api.hpp"
#include "core/hooks/glapi.hpp"
#include "common/log.hpp"
#include "core/pack/options.hpp"
#include "core/tracking/shader_catalog.hpp"
#include "core/tracking/tracker.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <cstring>
#include <regex>
#include <string_view>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace prisma::gameshaders {

// Virtual uniform locations handed to Luanti start here.
constexpr GLint VBASE = 0x40000000;
bool g_virtual_locations = false;

static const pack::Pack *g_pack = nullptr;
static bool g_enabled = true;

struct Stage {
	bool present = false;
	GLenum type = 0;
	std::string name;     // Luanti shader name, "" if not one of Luanti's files
	std::string luanti;   // the source Luanti asked for
	std::string compiled; // what is compiled right now
	std::vector<std::string> files;
	bool modified = false;
	std::string what; // "extended", "replaced"...
};

using compose::Composed;

struct Program {
	std::vector<GLuint> attached; // between attach and link
	Stage vertex, fragment;
	bool managed = false; // one of Luanti's programs: virtual locations apply
	std::vector<std::string> vnames;
	std::unordered_map<std::string, int> vindex;
	std::vector<GLint> real;
	api::Locations api{};
	options::Locations opts;
	bool uses_prisma = false; // api or options
	bool uses_shadows = false; // samples shadowTex or shadowTexPrev
	unsigned long long stamp = ~0ull;

	void query(GLuint program)
	{
		api.query(program);
		opts.query(program);
		uses_prisma = api.any || std::any_of(opts.loc.begin(), opts.loc.end(), [](GLint l) { return l >= 0; });
		// The shadow samplers point at fixed units (bind_shadow_maps)
		GLint shadow = gl::GetUniformLocation(program, "shadowTex");
		GLint prev = gl::GetUniformLocation(program, "shadowTexPrev");
		uses_shadows = shadow >= 0 || prev >= 0;
		if (uses_shadows) {
			GLint current = 0;
			gl::GetIntegerv(GL_CURRENT_PROGRAM, &current);
			gl::UseProgram(program);
			if (shadow >= 0)
				gl::Uniform1i(shadow, api::SHADOW_UNIT);
			if (prev >= 0)
				gl::Uniform1i(prev, api::SHADOW_PREV_UNIT);
			gl::UseProgram(current);
		}
		stamp = ~0ull;
	}
};

static std::unordered_map<GLuint, Stage> g_shaders;
static std::unordered_map<GLuint, Program> g_programs;

static const char *stage_name(GLenum type)
{
	return type == GL_VERTEX_SHADER ? "vertex" : type == GL_FRAGMENT_SHADER ? "fragment" : "";
}

// The pack to apply, if it is on
static const pack::Pack *active_pack()
{
	return g_enabled ? g_pack : nullptr;
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

static void log_block(const std::string &text)
{
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line))
		if (!line.empty())
			log::info("    %s", line.c_str());
}

static GLuint compile_raw(GLenum type, const std::string &code, std::string &error)
{
	GLuint sh = gl::CreateShader(type);
	const GLchar *src = code.c_str();
	gl::ShaderSource(sh, 1, &src, nullptr);
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

// Compiles the best version of a stage that works: the pack's, then
// Prisma's minimal patch, then Luanti's untouched source.
static GLuint compile_stage(const Stage &base, bool use_pack, Stage &out)
{
	std::vector<Composed> tries;
	if (use_pack)
		tries.push_back(compose::compose(base.type, base.name, base.luanti, active_pack()));
	tries.push_back(compose::compose(base.type, base.name, base.luanti, nullptr));
	tries.push_back(Composed{});
	for (const Composed &c : tries) {
		const std::string &code = c.modified ? c.code : base.luanti;
		std::string error;
		if (GLuint id = compile_raw(base.type, code, error)) {
			out = base;
			out.compiled = code;
			out.files = c.files;
			out.modified = c.modified;
			out.what = c.what;
			return id;
		}
		log::error("%s (%s) does not compile:", base.name.c_str(), stage_name(base.type));
		log_block(pack::translate_log(error, c.files));
	}
	return 0;
}

static void upload_value(GLint loc, const tracker::UniformValue &v)
{
	GLsizei n = v.count;
	if (v.kind == tracker::UKind::Int) {
		const GLint *d = v.i.data();
		switch (v.components) {
		case 1: gl::Uniform1iv(loc, n, d); break;
		case 2: gl::Uniform2iv(loc, n, d); break;
		case 3: gl::Uniform3iv(loc, n, d); break;
		case 4: gl::Uniform4iv(loc, n, d); break;
		}
		return;
	}
	const GLfloat *d = v.f.data();
	if (v.kind == tracker::UKind::Matrix) {
		switch (v.components) {
		case 4: gl::UniformMatrix2fv(loc, n, 0, d); break;
		case 9: gl::UniformMatrix3fv(loc, n, 0, d); break;
		case 16: gl::UniformMatrix4fv(loc, n, 0, d); break;
		}
		return;
	}
	switch (v.components) {
	case 1: gl::Uniform1fv(loc, n, d); break;
	case 2: gl::Uniform2fv(loc, n, d); break;
	case 3: gl::Uniform3fv(loc, n, d); break;
	case 4: gl::Uniform4fv(loc, n, d); break;
	}
}

// After a relink every uniform is back to zero: give each one the last value
// Luanti sent, to this program or (for globals like dayLight) to any program.
static void restore_values(GLuint program, Program &p)
{
	GLint current = 0;
	gl::GetIntegerv(GL_CURRENT_PROGRAM, &current);
	gl::UseProgram(program);

	std::set<std::string> done;
	for (size_t i = 0; i < p.vnames.size(); i++) {
		done.insert(p.vnames[i]);
		if (p.real[i] < 0)
			continue;
		const tracker::UniformValue *v = tracker::uniform_value(program, VBASE + (GLint)i);
		if (!v)
			v = tracker::global_uniform_value(p.vnames[i]);
		if (v)
			upload_value(p.real[i], *v);
	}
	// Uniforms the new code uses that Luanti never asked about
	GLint count = 0;
	gl::GetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
	for (GLint k = 0; k < count; k++) {
		char name[256];
		GLsizei len = 0;
		GLint size = 0;
		GLenum type = 0;
		gl::GetActiveUniform(program, k, sizeof(name), &len, &size, &type, name);
		std::string n(name, len);
		if (size_t b = n.find('['); b != std::string::npos)
			n.resize(b);
		if (done.count(n) || n.starts_with("prisma."))
			continue;
		if (const tracker::UniformValue *v = tracker::global_uniform_value(n))
			upload_value(gl::GetUniformLocation(program, n.c_str()), *v);
	}
	gl::UseProgram(current);
}

static std::string describe(const Program &p)
{
	std::string s;
	for (const Stage *st : {&p.vertex, &p.fragment})
		if (st->present && !st->what.empty())
			s += std::string(s.empty() ? "" : ", ") + stage_name(st->type) + " " + st->what;
	return s;
}

static const std::string &program_name(const Program &p)
{
	return !p.fragment.name.empty() ? p.fragment.name : p.vertex.name;
}

static bool rebuild(GLuint program, Program &p, bool use_pack)
{
	Stage nv, nf;
	GLuint vs = p.vertex.present ? compile_stage(p.vertex, use_pack, nv) : 0;
	GLuint fs = p.fragment.present ? compile_stage(p.fragment, use_pack, nf) : 0;
	if ((p.vertex.present && !vs) || (p.fragment.present && !fs)) {
		if (vs)
			gl::DeleteShader(vs);
		if (fs)
			gl::DeleteShader(fs);
		return false;
	}

	GLuint attached[16];
	GLsizei n = 0;
	gl::GetAttachedShaders(program, 16, &n, attached);
	for (GLsizei i = 0; i < n; i++)
		gl::DetachShader(program, attached[i]);
	if (vs)
		gl::AttachShader(program, vs);
	if (fs)
		gl::AttachShader(program, fs);
	gl::LinkProgram(program);
	// Flagged for deletion; they go away when detached by the next rebuild.
	if (vs)
		gl::DeleteShader(vs);
	if (fs)
		gl::DeleteShader(fs);

	GLint linked = 0;
	gl::GetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
	log::error("%s#%u does not link:", program_name(p).c_str(), program);
		log_block(program_log(program));
		if (use_pack)
			return rebuild(program, p, false);
		return false;
	}

	if (p.vertex.present)
		p.vertex = nv;
	if (p.fragment.present)
		p.fragment = nf;
	for (size_t i = 0; i < p.vnames.size(); i++)
		p.real[i] = gl::GetUniformLocation(program, p.vnames[i].c_str());
	restore_values(program, p);
	p.query(program);
	return true;
}

// ---------------------------------------------------------------- public

void set_pack(const pack::Pack *pack, bool enabled)
{
	g_pack = pack;
	g_enabled = enabled;
}

void rebuild_all()
{
	if (!gl::load())
		return;
	int rebuilt = 0, modified = 0;
	for (auto &[id, p] : g_programs) {
		if (!p.managed)
			continue;
		bool changed = false;
		for (const Stage *st : {&p.vertex, &p.fragment}) {
			if (!st->present || st->name.empty())
				continue;
			Composed c = compose::compose(st->type, st->name, st->luanti, active_pack());
			if ((c.modified ? c.code : st->luanti) != st->compiled)
				changed = true;
		}
		if (changed && rebuild(id, p, true))
			rebuilt++;
		if (!describe(p).empty())
			modified++;
	}
	log::info("Game shaders: %d program(s) rebuilt, %d modified by the pack", rebuilt, modified);
}

std::string source_for(GLuint shader, GLenum type, const std::string &luanti_source)
{
	Stage s;
	s.present = true;
	s.type = type;
	s.luanti = luanti_source;
	s.compiled = luanti_source;
	if (type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER)
		s.name = catalog::identify(luanti_source, stage_name(type)).name;
	if (!s.name.empty())
		trace::luanti_source(s.name, type, luanti_source);
	std::string result;
	if (!s.name.empty()) {
		Composed c = compose::compose(type, s.name, luanti_source, active_pack());
		if (c.modified) {
			s.compiled = c.code;
			s.files = c.files;
			s.modified = true;
			s.what = c.what;
			result = c.code;
		}
	}
	g_shaders[shader] = std::move(s);
	return result;
}

std::string after_compile(GLuint shader)
{
	auto it = g_shaders.find(shader);
	if (it == g_shaders.end() || !it->second.modified || !gl::load())
		return {};
	GLint ok = 0;
	gl::GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok)
		return {};

	Stage &s = it->second;
	log::error("%s (%s) does not compile with the pack:", s.name.c_str(), stage_name(s.type));
	log_block(pack::translate_log(shader_log(shader), s.files));
	log::error("Using Luanti's original shader");

	for (const Composed &c : {compose::compose(s.type, s.name, s.luanti, nullptr), Composed{}}) {
		const std::string &code = c.modified ? c.code : s.luanti;
		const GLchar *src = code.c_str();
		gl::ShaderSource(shader, 1, &src, nullptr);
		gl::CompileShader(shader);
		gl::GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
		s.compiled = code;
		s.files = c.files;
		s.modified = c.modified;
		s.what = c.what;
		if (ok)
			break;
	}
	return s.compiled;
}

void on_attach(GLuint program, GLuint shader)
{
	g_programs[program].attached.push_back(shader);
}

void after_link(GLuint program)
{
	Program &p = g_programs[program];
	for (GLuint id : p.attached) {
		auto it = g_shaders.find(id);
		if (it == g_shaders.end())
			continue;
		if (it->second.type == GL_VERTEX_SHADER)
			p.vertex = it->second;
		else if (it->second.type == GL_FRAGMENT_SHADER)
			p.fragment = it->second;
	}
	p.attached.clear();
	if (program_name(p).empty() || !gl::load())
		return;
	p.managed = true;
	g_virtual_locations = true;

	GLint linked = 0;
	gl::GetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked && (p.vertex.modified || p.fragment.modified)) {
		log::error("%s#%u does not link with the pack:", program_name(p).c_str(), program);
		log_block(program_log(program));
		log::error("Using Luanti's original shader");
		rebuild(program, p, false);
	} else {
		for (size_t i = 0; i < p.vnames.size(); i++)
			p.real[i] = gl::GetUniformLocation(program, p.vnames[i].c_str());
		p.query(program);
	}
	std::string what = describe(p);
	if (!what.empty())
		log::info("Pack applied to %s#%u: %s", program_name(p).c_str(), program, what.c_str());
}

void on_delete_program(GLuint program)
{
	g_programs.erase(program);
}

GLint on_get_uniform_location(GLuint program, const GLchar *name, GLint real)
{
	auto it = g_programs.find(program);
	if (it == g_programs.end() || !it->second.managed || !name)
		return real;
	Program &p = it->second;
	auto v = p.vindex.find(name);
	int idx;
	if (v == p.vindex.end()) {
		idx = (int)p.vnames.size();
		p.vnames.push_back(name);
		p.vindex[name] = idx;
		p.real.push_back(real);
	} else {
		idx = v->second;
		p.real[idx] = real;
	}
	return VBASE + idx;
}

GLint real_location(GLuint program, GLint location)
{
	if (location < VBASE)
		return location;
	auto it = g_programs.find(program);
	if (it == g_programs.end())
		return -1;
	size_t idx = size_t(location - VBASE);
	return idx < it->second.real.size() ? it->second.real[idx] : -1;
}

// The shadow maps on their units for the game shaders: once per frame, and
// again after Prisma's own drawing (invalidate_shadow_maps), which may use
// those units. Luanti never does.
static unsigned long long g_shadow_maps_frame = ~0ull;

static void bind_shadow_maps(unsigned long long frame)
{
	if (g_shadow_maps_frame == frame)
		return;
	g_shadow_maps_frame = frame;
	GLint active = 0;
	gl::GetIntegerv(GL_ACTIVE_TEXTURE, &active);
	gl::ActiveTexture(GL_TEXTURE0 + api::SHADOW_UNIT);
	gl::BindTexture(GL_TEXTURE_2D, lighting::shadow_texture());
	gl::ActiveTexture(GL_TEXTURE0 + api::SHADOW_PREV_UNIT);
	gl::BindTexture(GL_TEXTURE_2D, lighting::shadow_texture_prev());
	gl::ActiveTexture((GLenum)active);
}

void invalidate_shadow_maps()
{
	g_shadow_maps_frame = ~0ull;
}

void before_draw(GLuint program, const GameState &state, unsigned long long frame)
{
	auto it = g_programs.find(program);
	if (it == g_programs.end() || !it->second.uses_prisma)
		return;
	if (it->second.uses_shadows)
		bind_shadow_maps(frame);
	if (it->second.stamp == frame)
		return;
	api::upload(it->second.api, state);
	options::upload(it->second.opts);
	it->second.stamp = frame;
}

bool uses_api(GLuint program)
{
	auto it = g_programs.find(program);
	return it != g_programs.end() && it->second.uses_prisma;
}

} // namespace prisma::gameshaders
