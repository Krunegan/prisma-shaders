// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/launcher.hpp"
#include "common/ini.hpp"
#include "common/pack_rules.hpp"
#include "common/toml.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace prisma::launcher {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- prisma.ini

void Config::load(const fs::path &ini)
{
	luanti = ini::get(ini, "prisma", "luanti", luanti);
	arguments = ini::get(ini, "prisma", "arguments", arguments);
	pack = ini::get(ini, "prisma", "pack", pack);
	packs_dir = ini::get(ini, "prisma", "packs", packs_dir);
	menu_key = ini::get(ini, "keys", "menu", menu_key);
	toggle_key = ini::get(ini, "keys", "toggle", toggle_key);
	apply_luanti_settings = ini::get_int(ini, "prisma", "apply_luanti_settings", 1) != 0;
	show_window = ini::get_int(ini, "prisma", "show_window", 1) != 0;
}

void Config::save(const fs::path &ini) const
{
	auto put = [&](const char *section, const char *key, const std::string &value) {
		// Empty optional entries are removed so as not to clutter the file
		if (value.empty())
			ini::remove(ini, section, key);
		else
			ini::set(ini, section, key, value);
	};
	put("prisma", "luanti", luanti);
	put("prisma", "arguments", arguments);
	put("prisma", "pack", pack);
	put("prisma", "packs", packs_dir);
	put("prisma", "apply_luanti_settings", apply_luanti_settings ? "" : "0");
	put("prisma", "show_window", show_window ? "" : "0");
	put("keys", "menu", menu_key);
	put("keys", "toggle", toggle_key);
}

fs::path Config::packs_folder(const fs::path &prisma_dir) const
{
	return packs_dir.empty() ? prisma_dir / "shaderpacks" : utf8_path(packs_dir);
}

// ---------------------------------------------------------------- packs

PackInfo read_pack(const fs::path &pack_dir, const std::string &folder_name)
{
	PackInfo p;
	p.folder = folder_name;
	p.title = folder_name;
	std::ifstream in(pack_dir / "pack.toml", std::ios::binary);
	if (!in) {
		p.error = "missing pack.toml";
		return p;
	}
	std::stringstream text;
	text << in.rdbuf();
	toml::Document doc;
	std::string error;
	if (!toml::parse(text.str(), doc, error)) {
		p.error = "pack.toml: " + error;
		return p;
	}
	auto str = [&](const char *key) {
		const toml::Value *v = doc.get(key);
		return v && v->type == toml::Value::String ? v->str : std::string();
	};
	if (std::string t = str("name"); !t.empty())
		p.title = t;
	p.description = str("description");
	p.author = str("author");
	p.version = str("version");
	if (std::string why = pack::check_api(doc.get("api")); !why.empty()) {
		p.error = "pack.toml: " + why;
		return p;
	}
	for (const std::string &key : doc.order) {
		if (!key.starts_with("luanti."))
			continue;
		const std::string name = key.substr(7);
		const toml::Value &v = doc.values.at(key);
		if (std::string why = pack::check_luanti_setting(name, v); !why.empty()) {
			// Nothing of a pack that asks for a forbidden setting is applied
			p.error = "pack.toml: " + why;
			p.luanti.clear();
			return p;
		}
		if (v.type == toml::Value::Bool) {
			p.luanti.emplace_back(name, v.boolean ? "true" : "false");
		} else {
			// Written back plainly, whatever notation pack.toml used (hex, exponent...)
			char number[32];
			std::snprintf(number, sizeof(number), "%.9g", v.num);
			p.luanti.emplace_back(name, number);
		}
	}
	return p;
}

std::vector<PackInfo> list_packs(const fs::path &folder)
{
	std::vector<PackInfo> out;
	std::error_code ec;
	for (const fs::directory_entry &e : fs::directory_iterator(folder, ec)) {
		if (!e.is_directory(ec))
			continue;
		// Hidden folders: a download on its way, or the one it replaces (downloads.cpp)
		if (to_utf8(e.path().filename()).starts_with('.'))
			continue;
		if (!fs::exists(e.path() / "pack.toml", ec) && !fs::exists(e.path() / "passes", ec))
			continue;
		out.push_back(read_pack(e.path(), to_utf8(e.path().filename())));
	}
	auto lower = [](std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	};
	std::sort(out.begin(), out.end(), [&](const PackInfo &a, const PackInfo &b) { return lower(a.title) < lower(b.title); });
	return out;
}

std::map<std::string, std::string> luanti_settings(const PackInfo &pack)
{
	return std::map<std::string, std::string>(pack.luanti.begin(), pack.luanti.end());
}

// ---------------------------------------------------------------- Luanti

bool is_executable(const std::string &path)
{
	if (path.empty())
		return false;
	std::error_code ec;
	fs::path p = utf8_path(path);
	if (!fs::is_regular_file(p, ec))
		return false;
#ifdef _WIN32
	return true;
#else
	return access(p.c_str(), X_OK) == 0;
#endif
}

std::string find_luanti()
{
	const char *path = std::getenv("PATH");
#ifdef _WIN32
	const char separator = ';';
	const char *names[] = {"luanti.exe", "minetest.exe"};
	std::string list = path ? path : "";
#else
	const char separator = ':';
	const char *names[] = {"luanti", "minetest"};
	std::string list = path ? path : "/usr/local/bin:/usr/bin:/usr/games";
#endif
	for (const char *name : names) {
		size_t start = 0;
		while (start <= list.size()) {
			size_t end = list.find(separator, start);
			if (end == std::string::npos)
				end = list.size();
			std::string dir = list.substr(start, end - start);
			if (!dir.empty()) {
				std::string candidate = to_utf8(utf8_path(dir) / name);
				if (is_executable(candidate))
					return candidate;
			}
			start = end + 1;
		}
	}
	return {};
}

// ---------------------------------------------------------------- texto

bool key_used_by_luanti(const std::string &name)
{
	if (name.size() == 1)
		return true; // letters and digits
	return name.size() >= 2 && (name[0] == 'F' || name[0] == 'f') && std::isdigit((unsigned char)name[1]);
}

std::string version_warning(const std::string &version)
{
	if (version.empty() || version == PRISMA_LUANTI_VERSION)
		return "";
	return "Prisma supports Luanti " PRISMA_LUANTI_VERSION " only; this is Luanti " + version +
		". Shaders may look wrong or fail to load.";
}

#ifdef _WIN32

std::vector<std::string> split_arguments(const std::string &line)
{
	std::vector<std::string> out;
	if (line.find_first_not_of(" \t") == std::string::npos)
		return out;
	// CommandLineToArgvW treats the first element as the program, so give it one
	int n = 0;
	std::wstring full = L"x " + utf8_path(line).wstring();
	LPWSTR *argv = CommandLineToArgvW(full.c_str(), &n);
	if (!argv)
		return out;
	for (int i = 1; i < n; i++)
		out.push_back(to_utf8(fs::path(argv[i])));
	LocalFree(argv);
	return out;
}

#else

std::vector<std::string> split_arguments(const std::string &line)
{
	std::vector<std::string> out;
	std::string cur;
	bool have = false;
	char quote = 0;
	for (size_t i = 0; i < line.size(); i++) {
		char c = line[i];
		if (quote) {
			if (c == quote)
				quote = 0;
			else if (c == '\\' && quote == '"' && i + 1 < line.size())
				cur += line[++i];
			else
				cur += c;
		} else if (c == '"' || c == '\'') {
			quote = c;
			have = true;
		} else if (c == '\\' && i + 1 < line.size()) {
			cur += line[++i];
			have = true;
		} else if (c == ' ' || c == '\t') {
			if (have)
				out.push_back(cur);
			cur.clear();
			have = false;
		} else {
			cur += c;
			have = true;
		}
	}
	if (have)
		out.push_back(cur);
	return out;
}

#endif

} // namespace prisma::launcher
