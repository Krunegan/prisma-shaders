// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/pack/pack.hpp"
#include "core/pack/api.hpp"
#include "common/pack_rules.hpp"
#include "common/toml.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace prisma::pack {

static fs::path g_library_dir;

void set_library_dir(const fs::path &dir)
{
	g_library_dir = dir;
}

const fs::path &library_dir()
{
	if (g_library_dir.empty())
		g_library_dir = core_dir() / "glsl";
	return g_library_dir;
}

bool is_builtin_sampler(const std::string &name)
{
	static const char *names[] = {"colorTex", "depthTex", "skyTex", "shadowTex", "shadowTexPrev", "gbufferTex",
		"gbufferLightTex", "gbufferMaterialTex", "overheadTex"};
	for (const char *n : names)
		if (name == n)
			return true;
	return false;
}

static bool read_file(const fs::path &p, std::string &out)
{
	std::ifstream in(p, std::ios::binary);
	if (!in)
		return false;
	std::ostringstream ss;
	ss << in.rdbuf();
	out = ss.str();
	out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
	return true;
}

// Whether `path` is inside `dir`, after resolving ".." and any symbolic links.
// Everything a pack refers to must be inside its folder.
static bool is_inside(const fs::path &path, const fs::path &dir)
{
	std::error_code ec;
	fs::path p = fs::weakly_canonical(path, ec);
	if (ec)
		return false;
	fs::path d = fs::weakly_canonical(dir, ec);
	if (ec)
		return false;
	fs::path rel = p.lexically_relative(d);
	return !rel.empty() && *rel.begin() != "..";
}

struct Preprocessor {
	fs::path root;
	std::vector<std::string> *files;
	std::vector<std::string> *errors;
	std::vector<fs::path> stack;
	std::string *target = nullptr; // passes: `#prisma target <buffer>` in the pass file itself
	std::vector<std::string> *chain = nullptr; // passes: `#prisma chain a b c ...`
	bool *fallback = nullptr; // passes: `#prisma fallback`

	std::string display(const fs::path &p) const
	{
		std::error_code ec;
		// The standard library by its include name: <prisma/bloom.glsl>
		fs::path lib = library_dir().lexically_normal();
		fs::path in_lib = p.lexically_normal().lexically_relative(lib);
		if (!in_lib.empty() && *in_lib.begin() != "..")
			return "<" + to_utf8_generic(in_lib) + ">";
		fs::path rel = fs::relative(p, root, ec);
		return to_utf8_generic((ec ? p : rel));
	}

	int index_of(const fs::path &p)
	{
		std::string name = display(p);
		for (size_t i = 0; i < files->size(); i++)
			if ((*files)[i] == name)
				return (int)i;
		files->push_back(name);
		return (int)files->size() - 1;
	}

	fs::path resolve(const fs::path &from, const std::string &target) const
	{
		fs::path t = utf8_path(target);
		for (const fs::path &base : {from.parent_path(), root, root / "include"}) {
			fs::path candidate = (base / t).lexically_normal();
			if (fs::is_regular_file(candidate))
				return candidate;
		}
		return {};
	}

	std::string run(const fs::path &file)
	{
		if (std::find(stack.begin(), stack.end(), file) != stack.end()) {
			errors->push_back(display(file) + ": #include circular");
			return {};
		}
		std::string text;
		if (!read_file(file, text)) {
			errors->push_back(display(file) + ": can't read");
			return {};
		}
		stack.push_back(file);
		int idx = index_of(file);
		std::string out = "#line 1 " + std::to_string(idx) + "\n";

		std::istringstream in(text);
		std::string line;
		int number = 0;
		bool in_comment = false; // inside /* */: directives there are only text
		while (std::getline(in, line)) {
			number++;
			bool commented = in_comment;
			for (size_t i = 0; i + 1 < line.size(); i++) {
				if (!in_comment && line[i] == '/' && line[i + 1] == '/')
					break;
				if (!in_comment && line[i] == '/' && line[i + 1] == '*') {
					in_comment = true;
					i++;
				} else if (in_comment && line[i] == '*' && line[i + 1] == '/') {
					in_comment = false;
					i++;
				}
			}
			if (commented) {
				out += line + "\n";
				continue;
			}
			size_t a = line.find_first_not_of(" \t");
			std::string_view body = a == std::string::npos ? std::string_view() : std::string_view(line).substr(a);
			if (body.starts_with("#include")) {
				// "file" from the pack, <file> from Prisma's standard library
				size_t q1 = body.find_first_of("\"<");
				char close = q1 == std::string_view::npos ? '"' : body[q1] == '<' ? '>' : '"';
				size_t q2 = q1 == std::string_view::npos ? q1 : body.find(close, q1 + 1);
				if (q2 == std::string_view::npos) {
					errors->push_back(display(file) + ":" + std::to_string(number) + ": malformed #include");
				} else if (close == '"' && fs::path(utf8_path(body.substr(q1 + 1, q2 - q1 - 1))).filename() ==
						"prisma_api.glsl") {
					// For code editors only (prisma_check --stub): everything in it
					// is already in the header. It may not even exist yet.
					out += "// " + line + "\n";
				} else {
					std::string target(body.substr(q1 + 1, q2 - q1 - 1));
					fs::path child;
					if (close == '>') {
						fs::path candidate = (library_dir() / utf8_path(target)).lexically_normal();
						if (fs::is_regular_file(candidate))
							child = candidate;
					} else {
						child = resolve(file, target);
					}
					// "file" may only reach the pack (or, from the library's own
					// files, the library); <file> only the library
					bool allowed = child.empty() || is_inside(child, library_dir()) ||
						(close == '"' && is_inside(child, root));
					if (child.empty())
						errors->push_back(display(file) + ":" + std::to_string(number) + ": not found " +
							(close == '>' ? "<" + target + "> in Prisma's library (" +
								to_utf8(library_dir()) + ")" : "\"" + target + "\""));
					else if (!allowed)
						errors->push_back(display(file) + ":" + std::to_string(number) + ": #include " +
							(close == '>' ? "<" + target + "> is outside Prisma's library" :
								"\"" + target + "\" is outside the pack's folder"));
					else
						out += run(child);
				}
				out += "#line " + std::to_string(number + 1) + " " + std::to_string(idx) + "\n";
				continue;
			}
			if (target && stack.size() == 1 && body.starts_with("#prisma target")) {
				std::string rest(body.substr(14));
				size_t a2 = rest.find_first_not_of(" \t"), b2 = rest.find_last_not_of(" \t");
				*target = a2 == std::string::npos ? "" : rest.substr(a2, b2 - a2 + 1);
				out += "// " + line + "\n";
				continue;
			}
			if (chain && stack.size() == 1 && body.starts_with("#prisma chain")) {
				std::istringstream words{std::string(body.substr(13))};
				std::string w;
				chain->clear();
				while (words >> w)
					chain->push_back(w);
				out += "// " + line + "\n";
				continue;
			}
			if (fallback && stack.size() == 1 && body.starts_with("#prisma fallback")) {
				*fallback = true;
				out += "// " + line + "\n";
				continue;
			}
			// The header already declares the version; keep line numbers intact.
			if (body.starts_with("#version")) {
				out += "// " + line + "\n";
				continue;
			}
			out += line + "\n";
		}
		stack.pop_back();
		return out;
	}
};

// GLSL 4.30's keywords and the words it reserves for future use (section 3.6
// of the specification): a buffer or an option with one of these names would
// break the declarations Prisma writes for it.
static const char *const k_glsl_reserved[] = {
	"attribute", "const", "uniform", "varying", "buffer", "shared", "coherent", "volatile", "restrict", "readonly",
	"writeonly", "atomic_uint", "layout", "centroid", "flat", "smooth", "noperspective", "patch", "sample", "break",
	"continue", "do", "for", "while", "switch", "case", "default", "if", "else", "subroutine", "in", "out", "inout",
	"float", "double", "int", "void", "bool", "true", "false", "invariant", "precise", "discard", "return", "mat2",
	"mat3", "mat4", "dmat2", "dmat3", "dmat4", "mat2x2", "mat2x3", "mat2x4", "dmat2x2", "dmat2x3", "dmat2x4",
	"mat3x2", "mat3x3", "mat3x4", "dmat3x2", "dmat3x3", "dmat3x4", "mat4x2", "mat4x3", "mat4x4", "dmat4x2", "dmat4x3",
	"dmat4x4", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "bvec2", "bvec3", "bvec4", "dvec2", "dvec3",
	"dvec4", "uint", "uvec2", "uvec3", "uvec4", "lowp", "mediump", "highp", "precision", "sampler1D", "sampler2D",
	"sampler3D", "samplerCube", "sampler1DShadow", "sampler2DShadow", "samplerCubeShadow", "sampler1DArray",
	"sampler2DArray", "sampler1DArrayShadow", "sampler2DArrayShadow", "isampler1D", "isampler2D", "isampler3D",
	"isamplerCube", "isampler1DArray", "isampler2DArray", "usampler1D", "usampler2D", "usampler3D", "usamplerCube",
	"usampler1DArray", "usampler2DArray", "sampler2DRect", "sampler2DRectShadow", "isampler2DRect", "usampler2DRect",
	"samplerBuffer", "isamplerBuffer", "usamplerBuffer", "sampler2DMS", "isampler2DMS", "usampler2DMS",
	"sampler2DMSArray", "isampler2DMSArray", "usampler2DMSArray", "samplerCubeArray", "samplerCubeArrayShadow",
	"isamplerCubeArray", "usamplerCubeArray", "image1D", "iimage1D", "uimage1D", "image2D", "iimage2D", "uimage2D",
	"image3D", "iimage3D", "uimage3D", "image2DRect", "iimage2DRect", "uimage2DRect", "imageCube", "iimageCube",
	"uimageCube", "imageBuffer", "iimageBuffer", "uimageBuffer", "image1DArray", "iimage1DArray", "uimage1DArray",
	"image2DArray", "iimage2DArray", "uimage2DArray", "imageCubeArray", "iimageCubeArray", "uimageCubeArray",
	"image2DMS", "iimage2DMS", "uimage2DMS", "image2DMSArray", "iimage2DMSArray", "uimage2DMSArray", "struct",
	"common", "partition", "active", "asm", "class", "union", "enum", "typedef", "template", "this", "resource",
	"goto", "inline", "noinline", "public", "static", "extern", "external", "interface", "long", "short", "half",
	"fixed", "unsigned", "superp", "input", "output", "hvec2", "hvec3", "hvec4", "fvec2", "fvec3", "fvec4",
	"sampler3DRect", "filter", "sizeof", "cast", "namespace", "using"
};

// A name a pack may give to a buffer or an option: a GLSL identifier that is
// not a keyword or a reserved word, does not start with gl_ and has no __.
static bool is_identifier(const std::string &s)
{
	if (s.empty() || std::isdigit((unsigned char)s[0]))
		return false;
	for (char c : s)
		if (!std::isalnum((unsigned char)c) && c != '_')
			return false;
	if (s.starts_with("gl_") || s.find("__") != std::string::npos)
		return false;
	for (const char *w : k_glsl_reserved)
		if (s == w)
			return false;
	return true;
}

// The <id> of every [<group>.<id>] table or <group>.<id>.<key> key of
// pack.toml, in the order they first appear
static std::vector<std::string> ids_in(const toml::Document &doc, const std::string &group)
{
	std::vector<std::string> ids;
	const std::string prefix = group + ".";
	for (const std::string &name : doc.names) {
		if (!name.starts_with(prefix))
			continue;
		std::string id = name.substr(prefix.size(), name.find('.', prefix.size()) - prefix.size());
		if (std::find(ids.begin(), ids.end(), id) == ids.end())
			ids.push_back(id);
	}
	return ids;
}

// pack.toml: metadata and the options shown in the menu.
static void read_manifest(const fs::path &root, Pack &pack)
{
	std::string text;
	if (!read_file(root / "pack.toml", text)) {
		pack.errors.push_back("missing pack.toml (it must at least say api = " + std::to_string(api::VERSION) + ")");
		return;
	}
	toml::Document doc;
	std::string error;
	if (!toml::parse(text, doc, error)) {
		pack.errors.push_back("pack.toml: " + error);
		return;
	}
	auto str = [&](const std::string &key, const std::string &fallback = "") {
		const toml::Value *v = doc.get(key);
		return v && v->type == toml::Value::String ? v->str : fallback;
	};
	pack.title = str("name", pack.name);
	pack.description = str("description");
	pack.author = str("author");
	pack.version = str("version");
	if (std::string why = check_api(doc.get("api")); !why.empty())
		pack.errors.push_back("pack.toml: " + why);
	else
		pack.api = (int)doc.get("api")->num;
	// [luanti]: only Luanti's rendering settings (pack_rules.hpp)
	for (const std::string &key : doc.order)
		if (key.starts_with("luanti."))
			if (std::string why = check_luanti_setting(key.substr(7), doc.values.at(key)); !why.empty())
				pack.errors.push_back("pack.toml: " + why);
	// requires = [...]: what the pack needs from Luanti's own settings
	if (const toml::Value *v = doc.get("requires")) {
		if (v->type != toml::Value::Array)
			pack.errors.push_back("pack.toml: requires must be a list, e.g. requires = [\"shadows\"]");
		else
			for (const toml::Value &item : v->items) {
				if (item.type == toml::Value::String && item.str == "shadows")
					pack.needs.push_back(item.str);
				else
					pack.errors.push_back("pack.toml: requires: unknown value \"" + item.str +
						"\" (the only one is \"shadows\")");
			}
	}

	// What Prisma draws or hides for the pack
	auto setting = [&](const std::string &key, Setting &out) {
		const toml::Value *v = doc.get(key);
		if (!v)
			return;
		if (v->type == toml::Value::Number)
			out = {v->num, ""};
		else if (v->type == toml::Value::String)
			out = {0, v->str}; // checked once the options are read
		else
			pack.errors.push_back("pack.toml: " + key + " must be a number or the name of an option");
	};
	bool has_shadow_table = false;
	for (const std::string &key : doc.order)
		has_shadow_table |= key.starts_with("shadows.");
	if (has_shadow_table) {
		const toml::Value *enabled = doc.get("shadows.enabled");
		pack.shadows = !enabled || (enabled->type == toml::Value::Bool && enabled->boolean);
		setting("shadows.resolution", pack.shadow_resolution);
		setting("shadows.distance", pack.shadow_distance);
		if (const toml::Value *v = doc.get("shadows.hidden_sky_light")) {
			bool ok = v->type == toml::Value::Array && v->items.size() == 3;
			for (size_t i = 0; ok && i < 3; i++)
				ok = v->items[i].type == toml::Value::Number;
			double len = 0;
			if (ok) {
				for (size_t i = 0; i < 3; i++)
					len += v->items[i].num * v->items[i].num;
				len = std::sqrt(len);
				ok = len > 1e-6 && v->items[1].num / len > 0.02;
			}
			if (!ok)
				pack.errors.push_back("pack.toml: shadows.hidden_sky_light must be three numbers, a direction "
									  "towards the light above the horizon, e.g. [0.9, 0.15, 0.4]");
			else {
				pack.hidden_sky_light = true;
				for (size_t i = 0; i < 3; i++)
					pack.hidden_sky_dir[i] = v->items[i].num / len;
			}
		}
	}
	bool has_overhead_table = false;
	for (const std::string &key : doc.order)
		has_overhead_table |= key.starts_with("overhead.");
	if (has_overhead_table) {
		const toml::Value *enabled = doc.get("overhead.enabled");
		pack.overhead = !enabled || (enabled->type == toml::Value::Bool && enabled->boolean);
		setting("overhead.resolution", pack.overhead_resolution);
		setting("overhead.distance", pack.overhead_distance);
	}
	if (const toml::Value *v = doc.get("gbuffer"); v && v->type == toml::Value::Bool)
		pack.gbuffer = v->boolean;
	setting("gbuffer_distance", pack.gbuffer_distance);
	if (const toml::Value *v = doc.get("hide"); v && v->type == toml::Value::Array) {
		for (const toml::Value &item : v->items) {
			if (item.str == "clouds")
				pack.hide_clouds = true;
			else if (item.str == "shadows")
				pack.hide_shadows = true;
			else if (item.str == "bloom")
				pack.hide_bloom = true;
			else if (item.str == "volumetric_light")
				pack.hide_volumetric_light = true;
			else if (item.str == "exposure")
				pack.hide_exposure = true;
			else if (item.str == "tonemapping")
				pack.hide_tonemapping = true;
			else if (item.str == "underwater")
				pack.hide_underwater = true;
			else
				pack.errors.push_back("pack.toml: hide does not know \"" + item.str +
					"\" (clouds, shadows, bloom, volumetric_light, exposure, tonemapping, underwater)");
		}
	}

	// Buffers, in the order they first appear in the file
	std::vector<std::string> buffer_ids = ids_in(doc, "buffers");
	static const char *reserved[] = {"prisma", "options", "texcoord", "fragColor", "targetSize"};
	for (const std::string &id : buffer_ids) {
		const std::string prefix = "buffers." + id + ".";
		auto where = "pack.toml, buffer " + id + ": ";
		bool bad = !is_identifier(id) || is_builtin_sampler(id) || id.starts_with("PRISMA_");
		for (const char *r : reserved)
			bad |= id == r;
		if (bad) {
			pack.errors.push_back(where + "the name must be a free GLSL identifier");
			continue;
		}
		Buffer b;
		b.id = id;
		if (const toml::Value *v = doc.get(prefix + "scale"); v && v->type == toml::Value::Number)
			b.scale = v->num;
		if (const toml::Value *v = doc.get(prefix + "size"); v && v->type == toml::Value::Array &&
				v->items.size() == 2) {
			b.width = (int)v->items[0].num;
			b.height = (int)v->items[1].num;
		}
		b.format = str(prefix + "format", b.format);
		b.linear = str(prefix + "filter", "linear") != "nearest";
		if (const toml::Value *v = doc.get(prefix + "mipmaps"); v && v->type == toml::Value::Bool)
			b.mipmaps = v->boolean;
		if (b.format != "rgba8" && b.format != "rgba16f" && b.format != "rgba32f")
			pack.errors.push_back(where + "format must be rgba8, rgba16f or rgba32f");
		else if (b.scale <= 0 || b.scale > 4 || b.width < 0 || b.height < 0 || b.width > 8192 || b.height > 8192)
			pack.errors.push_back(where + "size out of range");
		else
			pack.buffers.push_back(b);
	}
	if (pack.buffers.size() > 11)
		pack.errors.push_back("pack.toml: at most 11 buffers");

	// Sounds, driven by the texels of one of the buffers
	pack.sound_buffer = str("sound_buffer");
	std::vector<std::string> sound_ids = ids_in(doc, "sounds");
	if (!sound_ids.empty()) {
		const Buffer *control = nullptr;
		for (const Buffer &b : pack.buffers)
			if (b.id == pack.sound_buffer)
				control = &b;
		if (!control)
			pack.errors.push_back("pack.toml: sounds need sound_buffer = \"<buffer>\", the name of one of "
				"the [buffers.*] (their texels control the sounds)");
		else if (control->width < 1 || control->height < 1 || control->width * control->height > 256)
			pack.errors.push_back("pack.toml: the sound buffer (" + control->id +
				") must have a small fixed size (size = [width, height], at most 256 texels)");
		for (const std::string &id : sound_ids) {
			const std::string prefix = "sounds." + id + ".";
			auto where = "pack.toml, sound " + id + ": ";
			Sound s;
			s.id = id;
			std::vector<std::string> names;
			if (const toml::Value *v = doc.get(prefix + "files"); v && v->type == toml::Value::Array)
				for (const toml::Value &item : v->items)
					names.push_back(item.str);
			if (const toml::Value *v = doc.get(prefix + "file"); v && v->type == toml::Value::String)
				names.push_back(v->str);
			bool ok = !names.empty();
			if (names.empty())
				pack.errors.push_back(where + "missing file = \"...\" (or files = [...])");
			for (const std::string &n : names) {
				fs::path f = (root / utf8_path(n)).lexically_normal();
				std::string ext = to_utf8(f.extension());
				std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
				if (!is_inside(f, root)) {
					pack.errors.push_back(where + "file " + n + " is outside the pack's folder");
					ok = false;
				} else if (ext != ".ogg" && ext != ".wav") {
					// The only decoders Prisma has (CMakeLists.txt, miniaudio)
					pack.errors.push_back(where + "file " + n + ": only .ogg and .wav files are supported");
					ok = false;
				} else if (!fs::is_regular_file(f)) {
					pack.errors.push_back(where + "missing file " + n);
					ok = false;
				}
				s.files.push_back(f);
			}
			if (const toml::Value *v = doc.get(prefix + "loop"); v && v->type == toml::Value::Bool)
				s.loop = v->boolean;
			if (const toml::Value *v = doc.get(prefix + "volume"); v && v->type == toml::Value::Number)
				s.volume = v->num;
			if (const toml::Value *v = doc.get(prefix + "voices"); v && v->type == toml::Value::Number)
				s.voices = (int)v->num;
			const toml::Value *c = doc.get(prefix + "control");
			if (!c || c->type != toml::Value::Number) {
				pack.errors.push_back(where + "missing control = <column of the sound buffer>");
				ok = false;
			} else {
				s.control = (int)c->num;
			}
			if (control && (s.control < 0 || s.control >= control->width)) {
				pack.errors.push_back(where + "control is outside the buffer " + control->id);
				ok = false;
			}
			if (s.voices < 1 || s.voices > 16 || s.volume < 0) {
				pack.errors.push_back(where + "voices goes from 1 to 16 and volume cannot be negative");
				ok = false;
			}
			if (ok)
				pack.sounds.push_back(std::move(s));
		}
	}

	// Options, in the order they first appear in the file
	std::vector<std::string> ids = ids_in(doc, "options");
	for (const std::string &id : ids) {
		const std::string prefix = "options." + id + ".";
		auto where = "pack.toml, option " + id + ": ";
		if (!is_identifier(id)) {
			pack.errors.push_back(where + "the name must be a GLSL identifier (letters, digits and _) that is not "
				"a GLSL keyword or reserved word");
			continue;
		}
		OptionDef o;
		o.id = id;
		o.label = str(prefix + "label", id);
		o.description = str(prefix + "description");
		o.group = str(prefix + "group");
		std::string type = str(prefix + "type", "float");
		const toml::Value *def = doc.get(prefix + "default");
		const toml::Value *values = doc.get(prefix + "values");
		if (type == "float" || type == "int") {
			o.type = type == "float" ? OptionDef::Float : OptionDef::Int;
			auto num = [&](const char *k, double fallback) {
				const toml::Value *v = doc.get(prefix + k);
				return v && v->type == toml::Value::Number ? v->num : fallback;
			};
			o.min = num("min", 0);
			o.max = num("max", o.type == OptionDef::Int ? 10 : 1);
			o.step = num("step", o.type == OptionDef::Int ? 1 : 0);
			o.def = def && def->type == toml::Value::Number ? def->num : o.min;
			if (o.max < o.min) {
				pack.errors.push_back(where + "max is less than min");
				continue;
			}
		} else if (type == "bool") {
			o.type = OptionDef::Bool;
			o.def = def && def->type == toml::Value::Bool && def->boolean ? 1 : 0;
		} else if (type == "choice") {
			o.type = OptionDef::Choice;
			o.define = true; // choices select code paths
			if (!values || values->type != toml::Value::Array || values->items.empty()) {
				pack.errors.push_back(where + "a choice option needs values = [ ... ]");
				continue;
			}
			for (const toml::Value &v : values->items)
				o.values.push_back(v.type == toml::Value::String ? v.str : v.str.empty() ? std::to_string(v.num) : v.str);
			o.def = 0;
			if (def && def->type == toml::Value::String) {
				auto it = std::find(o.values.begin(), o.values.end(), def->str);
				if (it != o.values.end())
					o.def = double(it - o.values.begin());
			} else if (def && def->type == toml::Value::Number) {
				o.def = def->num;
			}
			o.min = 0;
			o.max = double(o.values.size() - 1);
			if (const toml::Value *n = doc.get(prefix + "numbers")) {
				bool ok = n->type == toml::Value::Array && n->items.size() == o.values.size();
				for (size_t i = 0; ok && i < n->items.size(); i++)
					ok = n->items[i].type == toml::Value::Number;
				if (!ok) {
					pack.errors.push_back(where + "numbers must be a list of numbers, one for each of the values");
					continue;
				}
				for (const toml::Value &v : n->items)
					o.numbers.push_back(v.num);
			}
		} else {
			pack.errors.push_back(where + "unknown type \"" + type + "\" (float, int, bool or choice)");
			continue;
		}
		if (const toml::Value *v = doc.get(prefix + "define"); v && v->type == toml::Value::Bool)
			o.define = v->boolean;
		pack.options.push_back(std::move(o));
	}

	for (const Setting *s : {&pack.shadow_resolution, &pack.shadow_distance, &pack.overhead_resolution,
			 &pack.overhead_distance, &pack.gbuffer_distance}) {
		if (s->option.empty())
			continue;
		const OptionDef *found = nullptr;
		for (const OptionDef &o : pack.options)
			if (o.id == s->option)
				found = &o;
		const char *table = s == &pack.overhead_resolution || s == &pack.overhead_distance ? "[overhead]"
			: s == &pack.gbuffer_distance ? "gbuffer_distance" : "[shadows]";
		if (!found) {
			pack.errors.push_back(std::string("pack.toml: ") + table + " uses option \"" + s->option + "\", which does not exist");
			continue;
		}
		// A choice stands for a number through numbers = [...] or its values' text
		if (found->type == OptionDef::Choice && found->numbers.empty())
			for (const std::string &v : found->values) {
				char *end = nullptr;
				std::strtod(v.c_str(), &end);
				if (v.empty() || *end != '\0') {
					pack.warnings.push_back(std::string("pack.toml: ") + table + " uses option \"" + s->option +
						"\", whose value \"" + v + "\" is not a number: give the option numbers = [...]");
					break;
				}
			}
	}
}

Pack load(const fs::path &dir)
{
	Pack pack;
	fs::path root(dir);
	pack.name = to_utf8(root.filename());
	if (!fs::is_directory(root)) {
		pack.errors.push_back("pack does not exist: " + to_utf8(dir));
		return pack;
	}
	read_manifest(root, pack);

	// Passes (optional: a pack may only change the game's shaders)
	std::vector<fs::path> pass_files;
	std::error_code ec;
	for (auto &e : fs::directory_iterator(root / "passes", ec))
		if (e.is_regular_file() && (e.path().extension() == ".frag" || e.path().extension() == ".comp"))
			pass_files.push_back(e.path());
	std::sort(pass_files.begin(), pass_files.end());

	auto preprocess = [&](const fs::path &file, std::vector<std::string> first_files, std::string *target = nullptr,
						  std::vector<std::string> *chain = nullptr, bool *fallback = nullptr) {
		Code c;
		c.files = std::move(first_files);
		Preprocessor pp{root, &c.files, &pack.errors, {}, target, chain, fallback};
		c.code = pp.run(file);
		return c;
	};
	auto is_buffer = [&](const std::string &id) {
		for (const Buffer &b : pack.buffers)
			if (b.id == id)
				return true;
		return false;
	};

	for (const fs::path &f : pass_files) {
		Pass pass;
		std::vector<std::string> chain;
		pass.name = to_utf8(f.filename());
		pass.compute = f.extension() == ".comp";
		pass.code = preprocess(f, {"<Prisma header>"}, &pass.target, &chain, &pass.fallback);
		if (!pass.target.empty() && !is_buffer(pass.target))
			pack.errors.push_back(pass.name + ": #prisma target " + pass.target + ": there is no [buffers." +
				pass.target + "] in pack.toml");
		if (pass.compute && (pass.target.empty() || !chain.empty() || pass.fallback)) {
			pack.errors.push_back(pass.name + ": a compute pass writes to a buffer: it needs #prisma target "
				"<buffer> (and no #prisma chain or #prisma fallback)");
			continue;
		}
		if (chain.empty()) {
			pack.passes.push_back(std::move(pass));
			continue;
		}

		// `#prisma chain a b c`: one pass per step, each reading the previous
		// texture of the list and writing the next one (a -> b, then b -> c).
		const std::string where = pass.name + ": #prisma chain: ";
		bool ok = true;
		if (!pass.target.empty()) {
			pack.errors.push_back(where + "a pass cannot have #prisma target and #prisma chain at the same time");
			ok = false;
		}
		if (chain.size() < 2) {
			pack.errors.push_back(where + "at least two textures are needed (where the first step reads from and where "
				"it writes to)");
			ok = false;
		}
		for (size_t i = 0; i < chain.size() && ok; i++) {
			if (is_buffer(chain[i]) || (i == 0 && is_builtin_sampler(chain[i])))
				continue;
			pack.errors.push_back(where + chain[i] + (i == 0 ? " is neither a pack buffer nor a Prisma texture "
				"(colorTex, depthTex...)" : " is not a pack buffer (steps only write to [buffers.*])"));
			ok = false;
		}
		if (!ok)
			continue;
		const size_t steps = chain.size() - 1;
		for (size_t i = 1; i <= steps; i++) {
			Pass step;
			step.name = pass.name + " (" + chain[i] + ")";
			step.target = chain[i];
			step.code.files = pass.code.files;
			step.code.code = "#define PRISMA_CHAIN_SOURCE " + chain[i - 1] + "\n#define PRISMA_CHAIN_TARGET " +
				chain[i] + "\n#define PRISMA_CHAIN_STEP " + std::to_string(i - 1) + "\n#define PRISMA_CHAIN_STEPS " +
				std::to_string(steps) + "\n" + pass.code.code;
			pack.passes.push_back(std::move(step));
		}
	}

	// Changes to the game's shaders
	fs::path game = root / "game";
	for (auto it = fs::recursive_directory_iterator(game, ec); !ec && it != fs::recursive_directory_iterator();
			it.increment(ec)) {
		if (!it->is_regular_file())
			continue;
		const fs::path &f = it->path();
		std::string shader = to_utf8_generic(fs::relative(f.parent_path(), game));
		std::string file = to_utf8(f.filename());
		// Source string 0 is Luanti's own code, 1 is the Prisma API block
		std::vector<std::string> first = {shader, "<Prisma API>"};
		GameShader &gs = pack.game[shader];
		if (file == "opengl_vertex.glsl")
			gs.vertex.replace = preprocess(f, first);
		else if (file == "opengl_fragment.glsl")
			gs.fragment.replace = preprocess(f, first);
		else if (file == "extend_vertex.glsl")
			gs.vertex.extend = preprocess(f, first);
		else if (file == "extend_fragment.glsl")
			gs.fragment.extend = preprocess(f, first);
		else if (file == "patch_vertex.glsl")
			gs.vertex.patch = preprocess(f, first);
		else if (file == "patch_fragment.glsl")
			gs.fragment.patch = preprocess(f, first);
		// Any other file is only there to be #included.
	}
	for (auto it = pack.game.begin(); it != pack.game.end();) {
		const GameShader &gs = it->second;
		bool used = false;
		for (const GameStage *st : {&gs.vertex, &gs.fragment})
			used |= !st->replace.empty() || !st->extend.empty() || !st->patch.empty();
		it = used ? std::next(it) : pack.game.erase(it);
	}
	return pack;
}

bool uses_compute(const Pack &pack)
{
	for (const Pass &p : pack.passes)
		if (p.compute)
			return true;
	return false;
}

std::string signature(const fs::path &dir)
{
	std::string sig;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
			it.increment(ec)) {
		if (!it->is_regular_file(ec))
			continue;
		long long time = (long long)fs::last_write_time(it->path(), ec).time_since_epoch().count();
		sig += to_utf8(it->path()) + "|" + std::to_string(time) + "|" +
			std::to_string((unsigned long long)it->file_size(ec)) + "\n";
	}
	return sig;
}

std::string translate_log(const std::string &log, const std::vector<std::string> &files)
{
	// Intel/AMD/Mesa: "ERROR: 2:15: ..."   NVIDIA: "2(15) : error ..."
	static const std::regex location(R"((\b|^)(\d+)(?::(\d+):|\((\d+)\)))");
	std::string out;
	std::istringstream in(log);
	std::string line;
	while (std::getline(in, line)) {
		std::smatch m;
		if (std::regex_search(line, m, location)) {
			size_t file = std::stoul(m[2].str());
			std::string number = m[3].matched ? m[3].str() : m[4].str();
			if (file < files.size())
				line = m.prefix().str() + files[file] + ":" + number + ":" + m.suffix().str();
		}
		out += line + "\n";
	}
	return out;
}

} // namespace prisma::pack
