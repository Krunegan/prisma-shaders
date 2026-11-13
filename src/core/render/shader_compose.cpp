// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/render/shader_compose.hpp"
#include "core/pack/api.hpp"
#include "core/pack/options.hpp"
#include "common/log.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

namespace prisma::compose {

// Warnings about a pack's game/ files are logged once per run
static std::set<std::string> g_warned;

// Plain scanning instead of std::regex: these run over every shader Luanti
// compiles (hundreds of lines each) and std::regex made loading visibly slower.

static bool is_ident(char c)
{
	return std::isalnum((unsigned char)c) || c == '_';
}

// Calls f(position) for each occurrence of `word` as a whole identifier.
template <typename F>
static bool any_word(const std::string &code, const char *word, F &&f)
{
	const size_t n = std::strlen(word);
	for (size_t pos = code.find(word); pos != std::string::npos; pos = code.find(word, pos + 1)) {
		if ((pos > 0 && is_ident(code[pos - 1])) || (pos + n < code.size() && is_ident(code[pos + n])))
			continue;
		if (f(pos, pos + n))
			return true;
	}
	return false;
}

static size_t skip_spaces(const std::string &s, size_t i)
{
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
		i++;
	return i;
}

// "uniform ... <name>;" on one line
static bool declares(const std::string &code, const char *name)
{
	return any_word(code, name, [&](size_t start, size_t end) {
		size_t k = skip_spaces(code, end);
		if (k >= code.size() || code[k] != ';')
			return false;
		size_t line = code.rfind('\n', start);
		line = line == std::string::npos ? 0 : line + 1;
		return std::string_view(code).substr(line, start - line).find("uniform") != std::string_view::npos;
	});
}

// "void <name>(" anywhere
static bool defines_function(const std::string &code, const char *name)
{
	return any_word(code, name, [&](size_t start, size_t end) {
		size_t k = skip_spaces(code, end);
		if (k >= code.size() || code[k] != '(')
			return false;
		size_t b = start;
		while (b > 0 && (code[b - 1] == ' ' || code[b - 1] == '\t' || code[b - 1] == '\n'))
			b--;
		return b >= 4 && code.compare(b - 4, 4, "void") == 0 && (b == 4 || !is_ident(code[b - 5]));
	});
}

// Where the fragment shader writes its colour.
static std::string output_variable(const std::string &code)
{
	if (code.find("gl_FragColor") != std::string::npos)
		return "gl_FragColor";
	if (code.find("gl_FragData[0]") != std::string::npos)
		return "gl_FragData[0]";
	static const std::regex out_decl(R"(\bout\s+(?:(?:lowp|mediump|highp)\s+)?vec4\s+(\w+)\s*;)");
	std::smatch m;
	if (std::regex_search(code, m, out_decl))
		return m[1].str();
	return {};
}

// Appends the file table of `c` to `files` and renumbers its #line
// directives to match. Entries 0 and 1 (Luanti, Prisma API) are shared.
static std::string renumber(const pack::Code &c, std::vector<std::string> &files)
{
	std::vector<int> map(c.files.size());
	for (size_t k = 0; k < c.files.size(); k++) {
		if (k < 2) {
			map[k] = (int)k;
		} else {
			map[k] = (int)files.size();
			files.push_back(c.files[k]);
		}
	}
	std::string out;
	std::istringstream in(c.code);
	std::string line;
	while (std::getline(in, line)) {
		int number, source;
		if (line.starts_with("#line ") && std::sscanf(line.c_str(), "#line %d %d", &number, &source) == 2 &&
				source >= 0 && source < (int)map.size())
			line = "#line " + std::to_string(number) + " " + std::to_string(map[source]);
		out += line + "\n";
	}
	return out;
}

static const pack::GameStage *pack_stage(const pack::Pack *pack, const std::string &name, GLenum type)
{
	if (!pack)
		return nullptr;
	auto it = pack->game.find(name);
	if (it == pack->game.end())
		return nullptr;
	return type == GL_VERTEX_SHADER ? &it->second.vertex : &it->second.fragment;
}

// ---------------------------------------------------------------- anchored patches
/*
	patch_*.glsl:

	    // Code before the first directive goes before Luanti's file (after the
	    // Prisma API): includes, functions, globals.
	    #prisma before "text of a line of Luanti's file"
	    ...code inserted right before the first line that contains that text...
	    #prisma after "another line"
	    ...code inserted right after it...

	A line that is not found (e.g. Luanti changed it) only skips that block,
	with a warning in the log.
*/

struct PatchBlock {
	bool after = false;
	std::string anchor;
	std::string code;
};

static std::string trim_copy(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t");
	size_t b = s.find_last_not_of(" \t\r");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Splits a (renumbered) patch file into its prelude and its blocks, keeping
// #line directives right so errors still point at the pack's file.
static std::string split_patch(const std::string &code, const std::string &name, std::vector<PatchBlock> &blocks)
{
	std::string prelude;
	int current = -1; // -1 = prelude, else index into blocks
	int line = 1, source = 1;
	std::istringstream in(code);
	std::string l;
	auto section = [&]() -> std::string & { return current < 0 ? prelude : blocks[current].code; };
	while (std::getline(in, l)) {
		int n, src;
		if (l.starts_with("#line ") && std::sscanf(l.c_str(), "#line %d %d", &n, &src) == 2) {
			line = n;
			source = src;
			section() += l + "\n";
			continue;
		}
		std::string t = trim_copy(l);
		if (t.starts_with("#prisma ")) {
			std::string rest = t.substr(8);
			size_t q1 = rest.find('"'), q2 = rest.rfind('"');
			std::string kind = q1 == std::string::npos ? "" : trim_copy(rest.substr(0, q1));
			if (q1 == std::string::npos || q2 <= q1 || (kind != "before" && kind != "after")) {
				if (g_warned.insert(name + "|" + t).second)
					log::error("%s: malformed directive (expected #prisma before \"...\" or #prisma after \"...\"): %s",
						name.c_str(), t.c_str());
				line++;
				continue;
			}
			blocks.push_back({kind == "after", rest.substr(q1 + 1, q2 - q1 - 1), ""});
			current = (int)blocks.size() - 1;
			line++;
			section() += "#line " + std::to_string(line) + " " + std::to_string(source) + "\n";
			continue;
		}
		section() += l + "\n";
		line++;
	}
	return prelude;
}

// Luanti's file with the blocks inserted. Luanti numbers its lines from 0
// ("#line 0" ends its header), and that numbering is restored after each block.
static std::string apply_patch(const std::string &body, const std::vector<PatchBlock> &blocks, const std::string &name,
	std::vector<std::string> &missing)
{
	std::vector<std::string> lines;
	size_t pos = 0;
	while (pos <= body.size()) {
		size_t end = body.find('\n', pos);
		if (end == std::string::npos)
			end = body.size();
		lines.push_back(body.substr(pos, end - pos));
		pos = end + 1;
	}
	std::vector<std::string> before(lines.size()), after(lines.size());
	for (const PatchBlock &b : blocks) {
		size_t i = 0;
		while (i < lines.size() && lines[i].find(b.anchor) == std::string::npos)
			i++;
		if (i == lines.size()) {
			missing.push_back(b.anchor);
			if (g_warned.insert(name + "|" + b.anchor).second)
				log::error("%s: line \"%s\" not found; this patch is not applied", name.c_str(), b.anchor.c_str());
			continue;
		}
		(b.after ? after : before)[i] += b.code;
	}
	std::string out = "#line 0 0\n";
	for (size_t i = 0; i < lines.size(); i++) {
		if (!before[i].empty())
			out += before[i] + "#line " + std::to_string(i) + " 0\n";
		out += lines[i] + "\n";
		if (!after[i].empty())
			out += after[i] + "#line " + std::to_string(i + 1) + " 0\n";
	}
	return out;
}

// Luanti's own post-processing effects that the pack replaces (pack.toml
// hide): they are switched off in the generated header of second_stage, so
// the result is the same whatever the player has in Luanti's settings.
static std::string hide_post_effects(const std::string &header, const std::string &name, const pack::Pack *pack,
	std::string &what)
{
	if (!pack || name != "second_stage")
		return header;
	std::string out;
	std::istringstream in(header);
	std::string line;
	bool changed = false;
	while (std::getline(in, line)) {
		bool drop = (pack->hide_exposure && line.starts_with("#define ENABLE_AUTO_EXPOSURE")) ||
			(pack->hide_bloom && line.starts_with("#define ENABLE_BLOOM"));
		if (pack->hide_tonemapping && line.starts_with("#define ENABLE_TONE_MAPPING")) {
			line = "#define ENABLE_TONE_MAPPING 0";
			changed = true;
		}
		if (drop) {
			changed = true;
			continue;
		}
		out += line + "\n";
	}
	if (changed)
		what = "Luanti effects hidden";
	return changed ? out : header;
}

Composed compose(GLenum type, const std::string &name, const std::string &luanti, const pack::Pack *pack)
{
	Composed c;
	const pack::GameStage *st = pack_stage(pack, name, type);
	bool replace = st && !st->replace.empty();
	bool extend = st && !st->extend.empty();
	bool patch = st && !st->patch.empty() && !replace; // a replacement is edited directly
	// Without dynamic shadows nodes_shader drops cameraPosition/cameraOffset
	// and Luanti stops sending them; Prisma needs them for the camera.
	bool keep_alive = type == GL_VERTEX_SHADER && name == "nodes_shader";
	bool post = pack && name == "second_stage" && (pack->hide_bloom || pack->hide_exposure || pack->hide_tonemapping);
	if (!replace && !extend && !keep_alive && !patch && !post)
		return c;

	// Luanti's generated header ends with "#line 0", then comes the file.
	static const std::string marker = "#line 0\n";
	size_t cut = luanti.find(marker);
	if (cut == std::string::npos) {
		if (g_warned.insert(name).second)
			log::error("%s: unexpected shader format, can't modify it", name.c_str());
		return c;
	}
	std::string hidden;
	const std::string header = post ? hide_post_effects(luanti.substr(0, cut) + "\n", name, pack, hidden)
		: luanti.substr(0, cut);
	const std::string body = luanti.substr(cut + marker.size());

	c.files = {name, "<Prisma API>"};
	std::string out = header;
	if (replace || extend || patch) {
		out += "#line 1 1\n" + std::string(api::GLSL) + options::glsl();
		if (type == GL_FRAGMENT_SHADER)
			out += api::SHADOW_GLSL;
	}
	std::vector<PatchBlock> blocks;
	if (patch)
		out += split_patch(renumber(st->patch, c.files), name, blocks);
	bool wrap = extend || keep_alive;
	if (wrap)
		out += "#define main prisma_luanti_main\n";

	std::string main_code = replace ? renumber(st->replace, c.files)
		: patch ? apply_patch(body, blocks, name, c.missing_anchors)
		: "#line 0 0\n" + body;
	out += main_code;

	if (wrap) {
		std::string ext = extend ? renumber(st->extend, c.files) : "";
		std::string all = main_code + ext;
		out += "\n#undef main\n" + ext + "\n#line 1 1\n";
		if (keep_alive && !declares(all, "cameraOffset"))
			out += "uniform highp vec3 cameraOffset;\n";
		if (keep_alive && !declares(all, "cameraPosition"))
			out += "uniform vec3 cameraPosition;\n";
		out += "void main(void)\n{\n\tprisma_luanti_main();\n";
		if (extend && type == GL_VERTEX_SHADER && defines_function(ext, "prisma_vertex"))
			out += "\tprisma_vertex();\n";
		if (extend && type == GL_FRAGMENT_SHADER && defines_function(ext, "prisma_fragment")) {
			std::string target = output_variable(all);
			if (target.empty()) {
				if (g_warned.insert(name + "/out").second)
					log::error("%s: can't find where the color is written; prisma_fragment will not be called", name.c_str());
			} else {
				out += "\tvec4 prisma_color = " + target + ";\n\tprisma_fragment(prisma_color);\n\t" + target +
					" = prisma_color;\n";
			}
		}
		if (keep_alive) {
			out += "\t// Never true: only makes the camera uniforms count as used.\n"
				   "\tif (cameraPosition.x > 3.0e38 || cameraOffset.x > 3.0e38)\n"
				   "\t\tgl_Position = vec4(0.0);\n";
		}
		out += "}\n";
	}

	c.code = std::move(out);
	c.modified = true;
	std::vector<std::string> what;
	if (!hidden.empty())
		what.push_back(hidden);
	if (replace)
		what.push_back("replaced");
	if (patch)
		what.push_back("patched");
	if (extend)
		what.push_back("extended");
	for (size_t i = 0; i < what.size(); i++)
		c.what += (i == 0 ? "" : i + 1 == what.size() ? " and " : ", ") + what[i];
	return c;
}

} // namespace prisma::compose
