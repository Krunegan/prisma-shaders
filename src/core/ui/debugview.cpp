// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ui/debugview.hpp"
#include "core/hooks/glapi.hpp"
#include "common/log.hpp"
#include "core/pack/passglsl.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>

namespace prisma::debugview {

// ---------------------------------------------------------------- GPU timings

/*
	Two ways to measure, chosen once per run:

	  * GPU timestamps (OpenGL 3.3 or GL_ARB_timer_query): each measured stage
	    gets two (glQueryCounter), one before and one after. A frame's
	    timestamps wait in a queue, with a fence behind them, until the GPU
	    has gone past the fence and every result is available; only then are
	    they read and their query objects used again. Nothing waits for the
	    GPU. A set that never finishes is dropped; if the GPU falls far
	    behind, frames go unmeasured.
	  * Otherwise: glFinish before and after each stage and the CPU clock.
	    Exact, but it makes the CPU wait for the GPU, so the game runs slower
	    while the timings are shown. Also when the queries turn out not to
	    work: the Intel driver offers them on Luanti's OpenGL 3.2 context, but
	    after the first frame they never finish (and reusing one hangs the
	    game). Seen as a set whose fence the GPU has passed while its results
	    still don't come.
*/
struct QuerySet {
	std::vector<GLuint> queries; // pairs: start and end of each stage
	std::vector<std::string> names;
	void *fence = nullptr;       // passed by the GPU once all of them are done
	int age = 0;                 // frames spent waiting
	int age_past_fence = 0;      // of them, after the GPU went past the fence
};
constexpr size_t k_max_pending = 8;
constexpr int k_max_age = 60;
constexpr int k_max_age_past_fence = 10;
static QuerySet g_current;            // this frame
static std::deque<QuerySet> g_pending; // closed, oldest first
static std::vector<GLuint> g_free;
static bool g_profiling = false, g_in_stage = false, g_skip_frame = false;
static std::vector<Timing> g_timings;
static std::map<std::string, double> g_smoothed;

enum Method { METHOD_UNKNOWN, METHOD_NONE, METHOD_QUERIES, METHOD_FINISH };
static Method g_method = METHOD_UNKNOWN;
static std::vector<Timing> g_finish_frame; // METHOD_FINISH: this frame's stages
static std::string g_finish_name;
static double g_finish_start = 0;

void set_profiling(bool on)
{
	g_profiling = on;
}

bool profiling()
{
	return g_profiling;
}

// Needs a current context
static Method choose_method()
{
	bool queries = false;
	if (gl::has_timer_queries() && gl::GetString) {
		const char *version = reinterpret_cast<const char *>(gl::GetString(GL_VERSION));
		int major = 0, minor = 0;
		if (version && std::sscanf(version, "%d.%d", &major, &minor) == 2)
			queries = major > 3 || (major == 3 && minor >= 3);
		GLint count = 0;
		gl::GetIntegerv(GL_NUM_EXTENSIONS, &count);
		for (GLint i = 0; i < count && !queries && gl::GetStringi; i++) {
			const char *ext = reinterpret_cast<const char *>(gl::GetStringi(GL_EXTENSIONS, (GLuint)i));
			queries = ext && std::strcmp(ext, "GL_ARB_timer_query") == 0;
		}
	}
	Method m = queries ? METHOD_QUERIES : gl::Finish ? METHOD_FINISH : METHOD_NONE;
	log::info("GPU timings in the menu: %s", m == METHOD_QUERIES ? "with time queries" :
		m == METHOD_FINISH ? "with glFinish (the context has no time queries; while measuring, the game runs "
		"slower)" : "unavailable");
	return m;
}

bool timings_supported()
{
	if (g_method == METHOD_UNKNOWN && gl::loaded())
		g_method = choose_method();
	return g_method == METHOD_QUERIES || g_method == METHOD_FINISH;
}

bool timings_slow_down()
{
	return g_method == METHOD_FINISH;
}

static void add_timing(std::vector<Timing> &list, const std::string &name, double ms)
{
	if (g_smoothed.size() > 256)
		g_smoothed.clear(); // names of packs no longer loaded
	double &avg = g_smoothed[name];
	avg = avg > 0 ? avg * 0.9 + ms * 0.1 : ms;
	list.push_back({name, avg});
}

static GLuint new_query()
{
	GLuint q = 0;
	if (g_free.empty()) {
		gl::GenQueries(1, &q);
	} else {
		q = g_free.back();
		g_free.pop_back();
	}
	return q;
}

static void drop(QuerySet &set, bool recycle)
{
	if (recycle)
		g_free.insert(g_free.end(), set.queries.begin(), set.queries.end());
	else if (!set.queries.empty())
		gl::DeleteQueries((GLsizei)set.queries.size(), set.queries.data());
	if (set.fence)
		gl::DeleteSync(set.fence);
	set = {};
}

// Asked without waiting: the GPU is past the fence and every result is there
static bool finished(QuerySet &set)
{
	GLenum status = gl::ClientWaitSync(set.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
	if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
		return false;
	set.age_past_fence++;
	for (GLuint q : set.queries) {
		GLint available = 0;
		gl::GetQueryObjectiv(q, GL_QUERY_RESULT_AVAILABLE, &available);
		if (!available)
			return false;
	}
	return true;
}

void frame_end()
{
	if (g_in_stage)
		time_end();
	if (g_method == METHOD_FINISH) {
		if (!g_finish_frame.empty())
			g_timings = std::move(g_finish_frame);
		g_finish_frame.clear();
		return;
	}
	if (!g_current.queries.empty()) {
		g_current.fence = gl::FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		g_pending.push_back(std::move(g_current));
	}
	g_current = {};
}

void frame_begin()
{
	if (!timings_supported())
		return;
	frame_end(); // in case the last frame didn't close its set
	if (!g_profiling)
		g_timings.clear();
	g_skip_frame = false;
	if (g_method != METHOD_QUERIES)
		return;
	while (!g_pending.empty()) {
		QuerySet &set = g_pending.front();
		if (!finished(set)) {
			if (set.age_past_fence > k_max_age_past_fence) {
				// The queries don't work here: never touch them again
				log::info("This driver's time queries never finish: GPU times are measured "
					"with glFinish (the game runs slower while they are shown)");
				for (QuerySet &s : g_pending)
					drop(s, false);
				g_pending.clear();
				g_method = METHOD_FINISH;
				return;
			}
			if (++set.age < k_max_age)
				break;
			drop(set, false); // it won't finish: never use those queries again
			g_pending.pop_front();
			continue;
		}
		std::vector<Timing> list;
		for (size_t i = 0; i + 1 < set.queries.size(); i += 2) {
			unsigned long long t0 = 0, t1 = 0;
			gl::GetQueryObjectui64v(set.queries[i], GL_QUERY_RESULT, &t0);
			gl::GetQueryObjectui64v(set.queries[i + 1], GL_QUERY_RESULT, &t1);
			add_timing(list, set.names[i / 2], t1 > t0 ? (t1 - t0) / 1e6 : 0);
		}
		if (g_profiling)
			g_timings = std::move(list);
		drop(set, true);
		g_pending.pop_front();
	}
	g_skip_frame = g_pending.size() >= k_max_pending;
}

void time_begin(const std::string &name)
{
	if (!g_profiling || g_in_stage || g_skip_frame || !timings_supported())
		return;
	if (g_method == METHOD_FINISH) {
		gl::Finish();
		g_finish_name = name;
		g_finish_start = uptime();
		g_in_stage = true;
		return;
	}
	GLuint q = new_query();
	gl::QueryCounter(q, GL_TIMESTAMP);
	g_current.queries.push_back(q);
	g_current.names.push_back(name);
	g_in_stage = true;
}

void time_end()
{
	if (!g_in_stage)
		return;
	if (g_method == METHOD_FINISH) {
		gl::Finish();
		add_timing(g_finish_frame, g_finish_name, (uptime() - g_finish_start) * 1000);
		g_in_stage = false;
		return;
	}
	GLuint q = new_query();
	gl::QueryCounter(q, GL_TIMESTAMP);
	g_current.queries.push_back(q);
	g_in_stage = false;
}

const std::vector<Timing> &timings()
{
	return g_timings;
}

// ---------------------------------------------------------------- viewer

const char *const VIEWER_FS = R"(#version 150
uniform sampler2D src;
uniform int mode;     // debugview::Kind
uniform int channel;  // 0 RGB, 1 R, 2 G, 3 B, 4 A
uniform float lo, hi;
uniform int lod;
uniform mat4 projInv;
uniform bool depthZeroToOne;
uniform bool probe;   // write the raw texel at probeTexel instead
uniform ivec2 probeTexel;
in vec2 texcoord;
out vec4 fragColor;

// One colour per material id (geometry::Material)
vec3 palette(int id)
{
	if (id == 0) return vec3(0.0);
	if (id == 1) return vec3(0.55);
	if (id == 2) return vec3(0.15, 0.75, 0.2);
	if (id == 3) return vec3(0.75, 0.9, 0.25);
	if (id == 4) return vec3(0.15, 0.45, 1.0);
	if (id == 5) return vec3(1.0, 0.4, 0.05);
	if (id == 6) return vec3(0.95, 0.3, 0.8);
	return vec3(1.0, 0.0, 1.0);
}

void main()
{
	if (probe) {
		fragColor = texelFetch(src, probeTexel, lod);
		return;
	}
	vec4 t = textureLod(src, texcoord, float(lod));
	if (mode == 3) {
		fragColor = vec4(palette(int(t.r * 255.0 + 0.5)), 1.0);
		return;
	}
	vec4 v = t;
	if (mode == 1) {
		// Distance from the camera, in nodes (the sky, very far)
		float z = depthZeroToOne ? t.r : t.r * 2.0 - 1.0;
		vec4 p = projInv * vec4(texcoord * 2.0 - 1.0, z, 1.0);
		v = vec4(t.r >= 1.0 ? 1e9 : length(p.xyz / p.w));
	} else if (mode == 2) {
		v = vec4(t.r);
	}
	float s = 1.0 / max(hi - lo, 1e-9);
	vec3 c = channel == 0 || mode != 0 ? (v.rgb - lo) * s : vec3((v[channel - 1] - lo) * s);
	fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

struct ProgramLocs {
	GLint src, mode, channel, lo, hi, lod, proj_inv, zero_to_one, probe, probe_texel;
};
static GLuint g_program = 0, g_vao = 0;
static bool g_program_failed = false;
static ProgramLocs g_loc{};

static GLuint g_view_fbo = 0, g_view_tex = 0;
static int g_view_w = 0, g_view_h = 0;
static bool g_view_valid = false;

static GLuint g_probe_fbo = 0, g_probe_tex = 0;
constexpr int k_probe_ring = 3;
struct ProbeSlot {
	GLuint pbo = 0;
	bool pending = false;
	Probe meta;          // everything but the values
	float u = 0, v = 0;  // where, 0..1
	Kind kind = KIND_COLOR;
	Mat4 proj_inv;
	bool zero_to_one = false;
};
static ProbeSlot g_probe_ring[k_probe_ring];
static int g_probe_slot = 0;
static Probe g_probe;

static bool g_viewer_open = false;
static Settings g_settings;
static float g_probe_u = -1, g_probe_v = -1;
static std::vector<Source> g_sources;

static GameState g_state;
static bool g_state_valid = false;

void set_viewer_open(bool open)
{
	g_viewer_open = open;
}

Settings &settings()
{
	return g_settings;
}

void set_probe(float u, float v)
{
	g_probe_u = u;
	g_probe_v = v;
}

bool viewer_wanted()
{
	return g_viewer_open || g_settings.fullscreen;
}

bool wants_overlay()
{
	return (g_settings.fullscreen && g_view_valid) || g_settings.hud;
}

void end_frame()
{
	g_view_valid = false;
}

const std::vector<Source> &sources()
{
	return g_sources;
}

GLuint view_texture()
{
	return g_view_valid ? g_view_tex : 0;
}

int view_width()
{
	return g_view_w;
}

int view_height()
{
	return g_view_h;
}

const Probe &probe()
{
	return g_probe;
}

void set_state(const GameState &state)
{
	g_state = state;
	g_state_valid = true;
}

const GameState *state()
{
	return g_state_valid ? &g_state : nullptr;
}

static GLuint compile(GLenum type, const char *source)
{
	GLuint sh = gl::CreateShader(type);
	gl::ShaderSource(sh, 1, &source, nullptr);
	gl::CompileShader(sh);
	GLint ok = 0;
	gl::GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char info[2048] = "";
		gl::GetShaderInfoLog(sh, sizeof info, nullptr, info);
		log::error("The texture viewer shader does not compile: %s", info);
		gl::DeleteShader(sh);
		return 0;
	}
	return sh;
}

static bool program()
{
	if (g_program || g_program_failed)
		return g_program != 0;
	GLuint vs = compile(GL_VERTEX_SHADER, passglsl::VERTEX);
	GLuint fs = compile(GL_FRAGMENT_SHADER, VIEWER_FS);
	if (!vs || !fs) {
		g_program_failed = true;
		return false;
	}
	g_program = gl::CreateProgram();
	gl::AttachShader(g_program, vs);
	gl::AttachShader(g_program, fs);
	gl::LinkProgram(g_program);
	gl::DeleteShader(vs);
	gl::DeleteShader(fs);
	GLint ok = 0;
	gl::GetProgramiv(g_program, GL_LINK_STATUS, &ok);
	if (!ok) {
		log::error("The texture viewer shader does not link");
		gl::DeleteProgram(g_program);
		g_program = 0;
		g_program_failed = true;
		return false;
	}
	g_loc = {gl::GetUniformLocation(g_program, "src"), gl::GetUniformLocation(g_program, "mode"),
		gl::GetUniformLocation(g_program, "channel"), gl::GetUniformLocation(g_program, "lo"),
		gl::GetUniformLocation(g_program, "hi"), gl::GetUniformLocation(g_program, "lod"),
		gl::GetUniformLocation(g_program, "projInv"), gl::GetUniformLocation(g_program, "depthZeroToOne"),
		gl::GetUniformLocation(g_program, "probe"), gl::GetUniformLocation(g_program, "probeTexel")};
	gl::GenVertexArrays(1, &g_vao);
	return true;
}

static bool view_target(int w, int h)
{
	if (g_view_tex && g_view_w == w && g_view_h == h)
		return true;
	if (!g_view_fbo)
		gl::GenFramebuffers(1, &g_view_fbo);
	if (g_view_tex)
		gl::DeleteTextures(1, &g_view_tex);
	gl::GenTextures(1, &g_view_tex);
	gl::ActiveTexture(GL_TEXTURE0);
	gl::BindTexture(GL_TEXTURE_2D, g_view_tex);
	gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	// Nearest: the texels of small buffers stay visible as squares
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_view_fbo);
	gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_view_tex, 0);
	if (gl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		gl::DeleteTextures(1, &g_view_tex);
		g_view_tex = 0;
		return false;
	}
	g_view_w = w;
	g_view_h = h;
	return true;
}

static bool probe_target()
{
	if (g_probe_tex)
		return true;
	gl::GenFramebuffers(1, &g_probe_fbo);
	gl::GenTextures(1, &g_probe_tex);
	gl::ActiveTexture(GL_TEXTURE0);
	gl::BindTexture(GL_TEXTURE_2D, g_probe_tex);
	gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, nullptr);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_probe_fbo);
	gl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_probe_tex, 0);
	if (gl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		gl::DeleteTextures(1, &g_probe_tex);
		g_probe_tex = 0;
		return false;
	}
	for (ProbeSlot &s : g_probe_ring) {
		gl::GenBuffers(1, &s.pbo);
		gl::BindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
		gl::BufferData(GL_PIXEL_PACK_BUFFER, 4 * sizeof(float), nullptr, GL_STREAM_READ);
	}
	gl::BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	return true;
}

// Point (u, v, depth) of the screen -> distance from the camera, in nodes
static float distance_at(const Mat4 &inv, float u, float v, float depth, bool zero_to_one)
{
	if (depth >= 1.f)
		return 1e9f;
	const float ndc[4] = {u * 2 - 1, v * 2 - 1, zero_to_one ? depth : depth * 2 - 1, 1};
	float p[4] = {};
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			p[r] += inv.m[c * 4 + r] * ndc[c];
	if (std::fabs(p[3]) < 1e-12f)
		return -1;
	return std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) / std::fabs(p[3]);
}

// Reads the oldest probe, finished by now, and queues this frame's.
static void read_probe(const Source *src, int level_w, int level_h, const GameState &state)
{
	if (!probe_target())
		return;
	int oldest = (g_probe_slot + 1) % k_probe_ring;
	ProbeSlot &old = g_probe_ring[oldest];
	if (old.pending) {
		gl::BindBuffer(GL_PIXEL_PACK_BUFFER, old.pbo);
		if (const void *data = gl::MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4 * sizeof(float), GL_MAP_READ_BIT)) {
			g_probe = old.meta;
			const float *f = static_cast<const float *>(data);
			std::copy(f, f + 4, g_probe.raw);
			gl::UnmapBuffer(GL_PIXEL_PACK_BUFFER);
			g_probe.distance = old.kind == KIND_DEPTH ?
				distance_at(old.proj_inv, old.u, old.v, g_probe.raw[0], old.zero_to_one) : -1;
			g_probe.valid = true;
		}
		old.pending = false;
	}
	if (!src) {
		gl::BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		return;
	}

	float u = g_probe_u < 0 ? 0.5f : std::clamp(g_probe_u, 0.f, 1.f);
	float v = g_probe_v < 0 ? 0.5f : std::clamp(g_probe_v, 0.f, 1.f);
	int x = std::clamp((int)(u * level_w), 0, level_w - 1);
	int y = std::clamp((int)(v * level_h), 0, level_h - 1);
	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_probe_fbo);
	gl::Viewport(0, 0, 1, 1);
	gl::Uniform1i(g_loc.probe, 1);
	const GLint texel[2] = {x, y};
	gl::Uniform2iv(g_loc.probe_texel, 1, texel);
	gl::DrawArrays(GL_TRIANGLES, 0, 3);
	gl::Uniform1i(g_loc.probe, 0);

	ProbeSlot &s = g_probe_ring[g_probe_slot];
	gl::BindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
	gl::BindFramebuffer(GL_READ_FRAMEBUFFER, g_probe_fbo);
	gl::ReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, nullptr);
	gl::BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	s.pending = true;
	s.meta = Probe{};
	s.meta.x = x;
	s.meta.y = y;
	s.meta.source = src->name;
	s.u = u;
	s.v = v;
	s.kind = src->kind;
	s.proj_inv = state.proj_inv;
	s.zero_to_one = state.depth_zero_to_one;
	g_probe_slot = oldest;
}

void render(const std::vector<Source> &list, const GameState &state)
{
	g_sources = list;
	if (!viewer_wanted() || !program())
		return;
	const Source *src = nullptr;
	for (const Source &s : g_sources)
		if (s.name == g_settings.source && s.texture)
			src = &s;
	if (!src) {
		read_probe(nullptr, 0, 0, state);
		return;
	}
	if (g_settings.auto_range) {
		g_settings.auto_range = false;
		g_settings.lo = 0;
		g_settings.hi = src->kind != KIND_DEPTH ? 1.f : state.far_plane > 0 ? state.far_plane : 200.f;
		g_settings.lod = 0;
	}
	int lod = std::clamp(g_settings.lod, 0, std::max(0, src->levels - 1));
	int w = std::max(1, src->width >> lod), h = std::max(1, src->height >> lod);
	// Big maps are shown smaller; the probe still reads the exact texel.
	float shrink = std::min(1.f, 2048.f / std::max(w, h));
	if (!view_target(std::max(1, (int)(w * shrink)), std::max(1, (int)(h * shrink))))
		return;

	gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_view_fbo);
	gl::Viewport(0, 0, g_view_w, g_view_h);
	gl::ColorMask(1, 1, 1, 1);
	gl::UseProgram(g_program);
	gl::BindVertexArray(g_vao);
	gl::ActiveTexture(GL_TEXTURE0);
	gl::BindTexture(GL_TEXTURE_2D, src->texture);
	gl::Uniform1i(g_loc.src, 0);
	gl::Uniform1i(g_loc.mode, (int)src->kind);
	gl::Uniform1i(g_loc.channel, std::clamp(g_settings.channel, 0, 4));
	gl::Uniform1f(g_loc.lo, g_settings.lo);
	gl::Uniform1f(g_loc.hi, g_settings.hi);
	gl::Uniform1i(g_loc.lod, lod);
	gl::UniformMatrix4fv(g_loc.proj_inv, 1, 0, state.proj_inv.m);
	gl::Uniform1i(g_loc.zero_to_one, state.depth_zero_to_one ? 1 : 0);
	gl::Uniform1i(g_loc.probe, 0);
	gl::DrawArrays(GL_TRIANGLES, 0, 3);
	g_view_valid = true;

	read_probe(src, w, h, state);
}

} // namespace prisma::debugview
