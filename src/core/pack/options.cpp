// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/pack/options.hpp"
#include "common/ini.hpp"
#include "core/hooks/glapi.hpp"
#include "common/log.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace prisma::options {

static std::vector<pack::OptionDef> g_defs;
static std::vector<double> g_values;
static std::filesystem::path g_ini;
static std::string g_section;
static std::string g_glsl;

static std::string number(double v, bool integer)
{
	char buf[64];
	if (integer)
		std::snprintf(buf, sizeof(buf), "%lld", (long long)std::llround(v));
	else
		std::snprintf(buf, sizeof(buf), "%.9g", v);
	std::string s = buf;
	// GLSL needs a decimal point for float literals
	if (!integer && s.find_first_of(".eE") == std::string::npos)
		s += ".0";
	return s;
}

static bool is_integer(const pack::OptionDef &d)
{
	return d.type != pack::OptionDef::Float;
}

static double clamp(const pack::OptionDef &d, double v)
{
	if (d.type == pack::OptionDef::Bool)
		return v != 0 ? 1 : 0;
	v = std::clamp(v, d.min, d.max);
	if (is_integer(d))
		v = std::round(v);
	return v;
}

// What prisma.ini stores for value `v` of option `d`: a choice by the name of
// its value, so reordering `values` in pack.toml keeps the player's choice;
// anything else as a number.
static std::string saved_text(const pack::OptionDef &d, double v)
{
	if (d.type == pack::OptionDef::Choice && v >= 0 && (size_t)v < d.values.size())
		return d.values[(size_t)v];
	return number(v, is_integer(d));
}

// The value saved as `text`, or nothing if it no longer fits the option
// (a choice value that is gone, a number out of range or not a number).
static bool parse_saved(const pack::OptionDef &d, const std::string &text, double &out)
{
	if (d.type == pack::OptionDef::Choice) {
		auto it = std::find(d.values.begin(), d.values.end(), text);
		if (it == d.values.end())
			return false;
		out = double(it - d.values.begin());
		return true;
	}
	char *end = nullptr;
	double v = std::strtod(text.c_str(), &end);
	if (end == text.c_str() || *end != 0 || !std::isfinite(v))
		return false;
	if (d.type == pack::OptionDef::Bool ? (v != 0 && v != 1) : (v < d.min || v > d.max))
		return false;
	out = v;
	return true;
}

static void rebuild_glsl()
{
	std::string members, defines;
	for (size_t i = 0; i < g_defs.size(); i++) {
		const pack::OptionDef &d = g_defs[i];
		if (d.define) {
			std::string name = d.id;
			std::transform(name.begin(), name.end(), name.begin(), [](char c) { return (char)std::toupper(c); });
			defines += "#define " + name + " " + number(g_values[i], is_integer(d)) + "\n";
			if (d.type == pack::OptionDef::Choice)
				for (size_t k = 0; k < d.values.size(); k++) {
					// e.g. CALIDAD_ALTA 2, to write #if CALIDAD == CALIDAD_ALTA
					std::string v;
					for (char c : d.values[k])
						v += std::isalnum((unsigned char)c) ? (char)std::toupper(c) : '_';
					defines += "#define " + name + "_" + v + " " + std::to_string(k) + "\n";
				}
		} else {
			const char *type = d.type == pack::OptionDef::Float ? "float" :
				d.type == pack::OptionDef::Bool ? "bool" : "int";
			members += std::string("\t") + type + " " + d.id + ";\n";
		}
	}
	g_glsl = "// ---- Pack options ----\n" + defines;
	if (!members.empty())
		g_glsl += "struct PrismaOptions {\n" + members + "};\nuniform PrismaOptions options;\n";
}

void set_pack(const pack::Pack &pack, const std::filesystem::path &ini)
{
	g_defs = pack.options;
	g_ini = ini;
	g_section = "options:" + pack.name;
	g_values.assign(g_defs.size(), 0);
	for (size_t i = 0; i < g_defs.size(); i++) {
		const pack::OptionDef &d = g_defs[i];
		std::string text = ini::get(g_ini, g_section, d.id);
		double v = d.def;
		if (!text.empty() && !parse_saved(d, text, v)) {
			log::info("Pack %s, option %s: the saved value \"%s\" no longer fits the option; using the default",
				pack.name.c_str(), d.id.c_str(), text.c_str());
			ini::remove(g_ini, g_section, d.id);
			v = d.def;
		}
		g_values[i] = clamp(d, v);
	}
	rebuild_glsl();
}

const std::vector<pack::OptionDef> &defs()
{
	return g_defs;
}

double value(size_t index)
{
	return index < g_values.size() ? g_values[index] : 0;
}

bool set(size_t index, double v)
{
	if (index >= g_defs.size())
		return false;
	const pack::OptionDef &d = g_defs[index];
	v = clamp(d, v);
	if (v == g_values[index])
		return false;
	g_values[index] = v;
	ini::set(g_ini, g_section, d.id, saved_text(d, v));
	if (!d.define)
		return false;
	rebuild_glsl();
	return true;
}

void reset_all()
{
	for (size_t i = 0; i < g_defs.size(); i++) {
		g_values[i] = clamp(g_defs[i], g_defs[i].def);
		ini::remove(g_ini, g_section, g_defs[i].id);
	}
	rebuild_glsl();
}

const std::string &glsl()
{
	return g_glsl;
}

void Locations::query(GLuint program)
{
	loc.assign(g_defs.size(), -1);
	for (size_t i = 0; i < g_defs.size(); i++)
		if (!g_defs[i].define)
			loc[i] = gl::GetUniformLocation(program, ("options." + g_defs[i].id).c_str());
}

void upload(const Locations &l)
{
	for (size_t i = 0; i < l.loc.size() && i < g_defs.size(); i++) {
		if (l.loc[i] < 0)
			continue;
		if (g_defs[i].type == pack::OptionDef::Float)
			gl::Uniform1f(l.loc[i], (float)g_values[i]);
		else
			gl::Uniform1i(l.loc[i], (int)g_values[i]);
	}
}

} // namespace prisma::options
