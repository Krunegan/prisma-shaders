// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/dev/trace.hpp"
#include "core/dev/developer.hpp"
#include "core/tracking/tracker_internal.hpp"
#include "common/log.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace prisma::tracker;
using namespace prisma::tracker::detail;

namespace prisma::trace {

bool g_active = false;

static fs::path g_out_dir;
static std::FILE *g_procs_file = nullptr;
static std::set<std::string> g_procs_seen;

static bool g_requested = false;
static bool g_auto_done = false;
static int g_index = 0;
static std::vector<std::string> g_lines;
static std::set<GLuint> g_fbos_described;
static std::set<GLuint> g_programs_used;
static GLint g_viewport[4] = {-1, -1, -1, -1};
static std::map<GLuint, unsigned> g_uploads; // buffer -> uploads in the traced frame
static unsigned g_upload_count = 0;

// Consecutive draws with the same program, framebuffer and kind, as one line
struct DrawGroup {
	std::string program;
	GLuint fbo = 0;
	const char *kind = nullptr;
	unsigned calls = 0;
	unsigned long long elements = 0;
	std::string first_call; // vertex layout of the first draw of the group
};
static DrawGroup g_group;

// ---------------------------------------------------------------- helpers

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

static const char *format_name(GLenum f)
{
	switch (f) {
	case 0x1903: return "RED";
	case 0x1907: return "RGB";
	case 0x1908: return "RGBA";
	case 0x1902: return "DEPTH_COMPONENT";
	case 0x8051: return "RGB8";
	case 0x8058: return "RGBA8";
	case 0x8059: return "RGB10_A2";
	case 0x8229: return "R8";
	case 0x822B: return "RG8";
	case 0x822D: return "R16F";
	case 0x822E: return "R32F";
	case 0x822F: return "RG16F";
	case 0x8230: return "RG32F";
	case 0x881A: return "RGBA16F";
	case 0x881B: return "RGB16F";
	case 0x8814: return "RGBA32F";
	case 0x8815: return "RGB32F";
	case 0x8C3A: return "R11F_G11F_B10F";
	case 0x8C43: return "SRGB8_ALPHA8";
	case 0x81A5: return "DEPTH16";
	case 0x81A6: return "DEPTH24";
	case 0x81A7: return "DEPTH32";
	case 0x8CAC: return "DEPTH32F";
	case 0x88F0: return "DEPTH24_STENCIL8";
	case 0x8CAD: return "DEPTH32F_STENCIL8";
	case 0x84F9: return "DEPTH_STENCIL";
	}
	static char buf[16];
	std::snprintf(buf, sizeof(buf), "0x%04X", f);
	return buf;
}

static std::string attachment_name(GLenum a)
{
	if (a >= GL_COLOR_ATTACHMENT0 && a < GL_COLOR_ATTACHMENT0 + 16)
		return printf_str("COLOR%u", a - GL_COLOR_ATTACHMENT0);
	switch (a) {
	case GL_DEPTH_ATTACHMENT: return "DEPTH";
	case GL_STENCIL_ATTACHMENT: return "STENCIL";
	case GL_DEPTH_STENCIL_ATTACHMENT: return "DEPTH_STENCIL";
	}
	return printf_str("0x%04X", a);
}

static std::string describe_surface(const Attachment &a)
{
	const auto &table = a.renderbuffer ? g_renderbuffers : g_textures;
	auto it = table.find(a.id);
	std::string s = printf_str("%s %u", a.renderbuffer ? "rb" : "tex", a.id);
	if (it != table.end()) {
		const Surface &sf = it->second;
		s += printf_str(" (%dx%d %s", sf.width, sf.height, format_name(sf.format));
		if (sf.samples > 1)
			s += printf_str(" msaa%d", sf.samples);
		s += ")";
	}
	return s;
}

static std::string describe_fbo(GLuint fbo)
{
	if (fbo == 0)
		return "0 (screen)";
	std::string s = printf_str("%u {", fbo);
	auto it = g_fbos.find(fbo);
	if (it != g_fbos.end()) {
		bool first = true;
		for (auto &[att, surf] : it->second) {
			if (surf.id == 0)
				continue;
			s += printf_str("%s%s: %s", first ? "" : ", ", attachment_name(att).c_str(), describe_surface(surf).c_str());
			first = false;
		}
	}
	return s + "}";
}

static std::string program_label(const Program *p)
{
	if (!p)
		return "<no program>";
	return printf_str("%s#%u", p->name.c_str(), p->id);
}

static const char *usage_name(GLenum u)
{
	switch (u) {
	case 0x88E0: return "stream";
	case 0x88E4: return "static";
	case 0x88E8: return "dynamic";
	}
	return "?";
}

static std::string describe_vertices(GLenum index_type, const void *indices)
{
	const VertexArray &va = g_vaos[g_vao];
	std::string s = printf_str("vao %u", g_vao);
	if (index_type) {
		if (va.element_buffer) {
			const BufferInfo &b = g_buffers[va.element_buffer];
			s += printf_str(" | indices: buffer %u (%s, %lld B) +%llu", va.element_buffer, usage_name(b.usage),
				(long long)b.size, (unsigned long long)(uintptr_t)indices);
		} else {
			s += " | indices: client memory";
		}
	}
	for (int i = 0; i < 16; i++) {
		const VertexAttrib &a = va.attribs[i];
		if (!a.enabled)
			continue;
		if (a.buffer) {
			const BufferInfo &b = g_buffers[a.buffer];
			s += printf_str(" | attr%d: buffer %u (%s) %dx0x%X stride %d +%llu", i, a.buffer, usage_name(b.usage),
				a.size, a.type, a.stride, (unsigned long long)a.offset);
		} else {
			s += printf_str(" | attr%d: client memory %dx0x%X stride %d", i, a.size, a.type, a.stride);
		}
	}
	return s;
}

static std::string sanitize(const std::string &name)
{
	std::string out = name;
	for (char &c : out)
		if (c == '/' || c == '\\' || c == ':' || c == '?')
			c = '_';
	return out;
}

static void write_text(const fs::path &path, const std::string &text)
{
	if (std::FILE *f = open_file(path, "wb")) {
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
	}
}

static std::string format_value(const UniformValue &v)
{
	std::string s;
	if (v.kind == UKind::Matrix) {
		int dim = v.components == 16 ? 4 : v.components == 9 ? 3 : 2;
		for (int r = 0; r < dim; r++) {
			s += r == 0 ? "[" : "\n                                [";
			for (int c = 0; c < dim; c++)
				s += printf_str("%s%10.4f", c ? " " : "", v.f[c * dim + r]); // column-major
			s += "]";
		}
		if (v.count > 1)
			s += printf_str(" (+%d more)", v.count - 1);
		return s;
	}
	size_t n = v.kind == UKind::Int ? v.i.size() : v.f.size();
	size_t shown = std::min<size_t>(n, 16);
	s = n > (size_t)v.components ? "[" : "";
	for (size_t k = 0; k < shown; k++) {
		if (k)
			s += (k % v.components) ? ", " : " | ";
		s += v.kind == UKind::Int ? printf_str("%d", v.i[k]) : printf_str("%g", v.f[k]);
	}
	if (n > shown)
		s += printf_str(" ... (%zu values)", n);
	if (n > (size_t)v.components)
		s += "]";
	return s;
}

// ---------------------------------------------------------------- recording

static void flush_group()
{
	if (g_group.calls == 0)
		return;
	g_lines.push_back(printf_str("    draw x%-5u %-28s %llu elements  -> FBO %u", g_group.calls,
		g_group.program.c_str(), g_group.elements, g_group.fbo));
	if (!g_group.first_call.empty())
		g_lines.push_back("               " + g_group.first_call);
	g_group = {};
}

static void line(std::string text)
{
	flush_group();
	g_lines.push_back(std::move(text));
}

static void start()
{
	g_active = true;
	g_index++;
	g_lines.clear();
	g_fbos_described.clear();
	g_programs_used.clear();
	g_uploads.clear();
	g_upload_count = 0;
	g_viewport[0] = g_viewport[1] = g_viewport[2] = g_viewport[3] = -1;
	g_group = {};
	g_lines.push_back(printf_str("Trace of a frame of Luanti %s  (frame %llu, t=%.2fs)", host_version().c_str(),
		g_frame, uptime()));
	g_lines.push_back("State at the start: draw FBO " + describe_fbo(g_draw_fbo) + ", program " +
		program_label(g_current));
	g_lines.push_back("");
}

static void finish()
{
	flush_group();
	g_lines.push_back(printf_str("Data uploads to buffers during the frame: %u", g_upload_count));
	for (auto &[id, n] : g_uploads) {
		auto b = g_buffers.find(id);
		g_lines.push_back(printf_str("    buffer %u (%s, %lld B): %u upload(s)", id,
			b == g_buffers.end() ? "deleted" : usage_name(b->second.usage),
			b == g_buffers.end() ? 0LL : (long long)b->second.size, n));
	}
	g_lines.push_back("");
	g_lines.push_back("==== Uniforms of each program (current value; * = sent in this frame) ====");
	for (GLuint id : g_programs_used) {
		auto it = g_programs.find(id);
		if (it == g_programs.end())
			continue;
		Program &p = it->second;
		g_lines.push_back("");
		g_lines.push_back(program_label(&p));
		for (auto &[loc, u] : p.uniform_names) {
			auto v = p.values.find(loc);
			if (v == p.values.end())
				g_lines.push_back(printf_str("    %-28s (never sent)", u.name.c_str()));
			else
				g_lines.push_back(printf_str("  %c %-28s = %s", v->second.frame == g_frame ? '*' : ' ', u.name.c_str(),
					format_value(v->second).c_str()));
		}
		for (auto &[loc, v] : p.values)
			if (!p.uniform_names.count(loc))
				g_lines.push_back(printf_str("  %c loc %-24d = %s", v.frame == g_frame ? '*' : ' ', loc,
					format_value(v).c_str()));
	}

	std::string text;
	for (auto &l : g_lines)
		text += l + "\n";
	fs::path path = g_out_dir / printf_str("frame_trace_%02d.txt", g_index).c_str();
	write_text(path, text);
	log::info("Frame trace %llu saved to %s", g_frame, to_utf8(path).c_str());

	g_active = false;
	g_lines.clear();
}

// ---------------------------------------------------------------- entry points

void init(const fs::path &out_dir)
{
	g_out_dir = out_dir;
	if (!developer::enabled())
		return;
	std::error_code ec;
	fs::remove_all(out_dir / "shaders", ec);
	fs::create_directories(out_dir / "shaders", ec);
	for (auto &e : fs::directory_iterator(out_dir, ec))
		if (to_utf8(e.path().filename()).starts_with("frame_trace_"))
			fs::remove(e.path(), ec);
	g_procs_file = open_file(out_dir / "gl_procs.txt", "wb");
}

void request()
{
	if (!developer::enabled())
		return;
	g_requested = true;
	log::info("Capturing the trace of the next frame");
}

void frame_ended()
{
	if (g_active)
		finish();
}

void frame_started(long long world_frames)
{
	if (!developer::enabled())
		return;
	// Automatic trace a few seconds into the game, so a plain run already
	// produces useful data.
	if (!g_auto_done && world_frames == 180) {
		g_auto_done = true;
		g_requested = true;
		log::info("Capturing automatic trace");
	}
	if (g_requested) {
		g_requested = false;
		start();
	}
}

void proc_requested(const char *name, bool found)
{
	if (!g_procs_file || !g_procs_seen.insert(name).second)
		return;
	std::fprintf(g_procs_file, "%s%s\n", name, found ? "" : "   (NULL)");
	std::fflush(g_procs_file);
}

void program_linked(GLuint program)
{
	if (!developer::enabled())
		return;
	auto p = g_programs.find(program);
	if (p == g_programs.end())
		return;
	for (GLuint id : p->second.shaders) {
		auto it = g_shaders.find(id);
		if (it == g_shaders.end())
			continue;
		const char *ext = it->second.type == GL_VERTEX_SHADER ? "vert" :
			it->second.type == GL_FRAGMENT_SHADER ? "frag" : "glsl";
		fs::path path = g_out_dir / "shaders" /
			printf_str("p%03u_%s.%s.glsl", program, sanitize(p->second.name).c_str(), ext).c_str();
		write_text(path, it->second.source);
	}
}

void luanti_source(const std::string &name, GLenum type, const std::string &source)
{
	if (!developer::enabled())
		return;
	static std::set<size_t> seen;
	static std::map<std::string, int> count;
	if (!seen.insert(std::hash<std::string>{}(name + "|" + source)).second)
		return;
	const char *ext = type == GL_VERTEX_SHADER ? "vert" : type == GL_FRAGMENT_SHADER ? "frag" : "glsl";
	std::string key = sanitize(name) + "." + ext;
	fs::path dir = g_out_dir / "shaders" / "luanti";
	std::error_code ec;
	fs::create_directories(dir, ec);
	write_text(dir / printf_str("%s.%02d.glsl", key.c_str(), ++count[key]).c_str(), source);
}

void framebuffer_bound(GLuint fbo)
{
	line("draw FBO -> " + (g_fbos_described.insert(fbo).second ? describe_fbo(fbo) : printf_str("%u", fbo)));
}

void attached(GLuint fbo, GLenum attachment, bool renderbuffer, GLuint id, GLint level)
{
	std::string text = printf_str("  FBO %u: %s = %s", fbo, attachment_name(attachment).c_str(),
		describe_surface({renderbuffer, id}).c_str());
	if (!renderbuffer)
		text += printf_str(" (level %d)", level);
	line(text);
}

void blit(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask)
{
	line(printf_str("blit FBO %u (%d,%d %dx%d) -> FBO %u (%d,%d %dx%d) mask 0x%X", g_read_fbo, sx0, sy0, sx1 - sx0,
		sy1 - sy0, g_draw_fbo, dx0, dy0, dx1 - dx0, dy1 - dy0, mask));
}

void draw_buffers(GLsizei n, const GLenum *bufs)
{
	std::string s = printf_str("drawBuffers FBO %u:", g_draw_fbo);
	for (GLsizei k = 0; k < n; k++)
		s += " " + (bufs[k] ? attachment_name(bufs[k]) : std::string("NONE"));
	line(s);
}

void draw(const char *kind, GLsizei count, GLsizei instances, GLenum index_type, const void *indices)
{
	if (g_current)
		g_programs_used.insert(g_current->id);
	std::string program = program_label(g_current);
	if (g_group.calls && (g_group.program != program || g_group.fbo != g_draw_fbo || g_group.kind != kind))
		flush_group();
	if (g_group.calls == 0)
		g_group.first_call = describe_vertices(index_type, indices);
	g_group.program = program;
	g_group.fbo = g_draw_fbo;
	g_group.kind = kind;
	g_group.calls++;
	g_group.elements += (unsigned long long)count * (unsigned long long)(instances > 0 ? instances : 1);
}

void clear(GLbitfield mask)
{
	std::string what;
	if (mask & GL_COLOR_BUFFER_BIT)
		what += " COLOR";
	if (mask & GL_DEPTH_BUFFER_BIT)
		what += " DEPTH";
	if (mask & GL_STENCIL_BUFFER_BIT)
		what += " STENCIL";
	line(printf_str("clear%s  (FBO %u)", what.c_str(), g_draw_fbo));
}

void viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (g_viewport[0] == x && g_viewport[1] == y && g_viewport[2] == width && g_viewport[3] == height)
		return;
	g_viewport[0] = x;
	g_viewport[1] = y;
	g_viewport[2] = width;
	g_viewport[3] = height;
	line(printf_str("viewport %d,%d %dx%d", x, y, width, height));
}

void buffer_upload(GLuint buffer)
{
	g_uploads[buffer]++;
	g_upload_count++;
}

} // namespace prisma::trace
