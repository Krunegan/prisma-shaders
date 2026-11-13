// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/luanticonf.hpp"
#include "common/util.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace prisma::luanticonf {

namespace fs = std::filesystem;

static const char *const k_header_forced = "# prisma-forzados:";
static const char *const k_header_user = "# prisma-origen:";

static std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t\r");
	return s.substr(a, b - a + 1);
}

static bool file_exists(const fs::path &path)
{
	std::error_code ec;
	return fs::is_regular_file(path, ec);
}

// ---------------------------------------------------------------- Conf

bool Conf::load(const fs::path &path)
{
	entries.clear();
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		std::string t = trim(line);
		size_t eq = t.find('=');
		if (t.empty() || t[0] == '#' || eq == std::string::npos) {
			entries.push_back({"", "", line});
			continue;
		}
		Entry e;
		e.key = trim(t.substr(0, eq));
		e.value = trim(t.substr(eq + 1));
		// Multi-line values, as Luanti reads them: """...""" and groups { }
		if (e.value == "\"\"\"" || (e.value.starts_with("\"\"\"") && !e.value.ends_with("\"\"\""))) {
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				e.value += "\n" + line;
				if (trim(line).ends_with("\"\"\""))
					break;
			}
		} else if (e.value == "{") {
			int depth = 1;
			while (depth > 0 && std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				e.value += "\n" + line;
				std::string l = trim(line);
				if (l.ends_with("{"))
					depth++;
				else if (l == "}")
					depth--;
			}
		}
		entries.push_back(e);
	}
	return true;
}

bool Conf::save(const fs::path &path) const
{
	std::ostringstream out;
	for (const Entry &e : entries) {
		if (e.key.empty())
			out << e.raw << "\n";
		else
			out << e.key << " = " << e.value << "\n";
	}
	// It is written aside and then renamed: a cut in the middle does not leave the
	// player's file damaged.
	fs::path tmp = path;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		std::string text = out.str();
		f.write(text.data(), (std::streamsize)text.size());
		if (!f)
			return false;
	}
	std::error_code ec;
	fs::rename(tmp, path, ec); // replaces the existing one
	return !ec;
}

void Conf::set(const std::string &key, const std::string &value)
{
	for (Entry &e : entries)
		if (e.key == key) {
			e.value = value;
			return;
		}
	entries.push_back({key, value, ""});
}

void Conf::remove(const std::string &key)
{
	std::erase_if(entries, [&](const Entry &e) { return e.key == key; });
}

std::map<std::string, std::string> Conf::values() const
{
	std::map<std::string, std::string> m;
	for (const Entry &e : entries)
		if (!e.key.empty())
			m[e.key] = e.value;
	return m;
}

// ---------------------------------------------------------------- session

// Where Luanti keeps its minetest.conf. A "portable" version (the
// Windows ones, and on Linux those built with RUN_IN_PLACE) keeps it next to the
// bin folder; otherwise in the user's folder: %APPDATA%\Luanti on
// Windows, ~/.luanti (or ~/.minetest, the older one) on Linux.
static fs::path user_conf_path(const fs::path &luanti)
{
	fs::path root = fs::absolute(luanti).parent_path().parent_path();
	std::vector<fs::path> user;
#ifdef _WIN32
	if (const wchar_t *a = _wgetenv(L"APPDATA"))
		user.push_back(fs::path(a) / "Luanti" / "minetest.conf");
#else
	if (const char *home = std::getenv("HOME")) {
		user.push_back(fs::path(home) / ".luanti" / "minetest.conf");
		user.push_back(fs::path(home) / ".minetest" / "minetest.conf");
	}
#endif
	std::vector<fs::path> candidates = {root / "minetest.conf", root.parent_path() / "minetest.conf"};
	candidates.insert(candidates.end(), user.begin(), user.end());
	for (const fs::path &p : candidates)
		if (file_exists(p))
			return p;
	if (file_exists(root / "minetest.conf.example") || user.empty())
		return root / "minetest.conf";
#ifndef _WIN32
	// None exists yet: the one Luanti will create (~/.minetest if that folder
	// is already there from an older version)
	std::error_code ec;
	if (user.size() > 1 && fs::is_directory(user[1].parent_path(), ec) && !fs::is_directory(user[0].parent_path(), ec))
		return user[1];
#endif
	return user[0];
}

void finish(const fs::path &prisma_dir, std::vector<std::string> &log)
{
	const fs::path prisma_conf = prisma_dir / "luanti_prisma.conf";
	const fs::path snapshot = prisma_dir / "luanti_prisma.start";
	std::error_code ec;
	Conf start, end, user;
	if (!start.load(snapshot))
		return;
	fs::path user_conf;
	std::map<std::string, bool> forced;
	for (const Conf::Entry &e : start.entries) {
		if (!e.key.empty())
			continue;
		if (e.raw.starts_with(k_header_user))
			user_conf = utf8_path(trim(e.raw.substr(std::strlen(k_header_user))));
		else if (e.raw.starts_with(k_header_forced)) {
			std::stringstream list(e.raw.substr(std::strlen(k_header_forced)));
			std::string k;
			while (std::getline(list, k, ','))
				if (!trim(k).empty())
					forced[trim(k)] = true;
		}
	}
	if (user_conf.empty() || !end.load(prisma_conf)) {
		fs::remove(snapshot, ec);
		return;
	}
	user.load(user_conf);

	auto before = start.values(), after = end.values();
	int changes = 0;
	for (const auto &[k, v] : after) {
		if (forced.count(k))
			continue;
		auto it = before.find(k);
		if (it == before.end() || it->second != v) {
			user.set(k, v);
			changes++;
		}
	}
	for (const auto &[k, v] : before)
		if (!forced.count(k) && !after.count(k)) {
			user.remove(k);
			changes++;
		}
	if (changes > 0) {
		if (user.save(user_conf))
			log.push_back("Saved to " + to_utf8(user_conf) + " " + std::to_string(changes) +
				" setting(s) you changed during the session");
		else
			log.push_back("Could not write " + to_utf8(user_conf) + ": your changes from the session are still in " +
				to_utf8(prisma_conf));
	}
	fs::remove(snapshot, ec);
}

Session prepare(const fs::path &prisma_dir, const std::map<std::string, std::string> &forced, const fs::path &luanti,
	std::vector<std::string> &args, std::vector<std::string> &log)
{
	Session s;
	s.forced = forced;
	if (s.forced.empty())
		return s;

	// The player may have chosen their own file with --config
	for (size_t i = 0; i + 1 < args.size(); i++)
		if (args[i] == "--config") {
			s.user_conf = utf8_path(args[i + 1]);
			args.erase(args.begin() + (std::ptrdiff_t)i, args.begin() + (std::ptrdiff_t)i + 2);
			break;
		}
	if (s.user_conf.empty())
		s.user_conf = user_conf_path(luanti);
	s.prisma_conf = prisma_dir / "luanti_prisma.conf";
	s.snapshot = prisma_dir / "luanti_prisma.start";

	Conf conf;
	conf.load(s.user_conf);
	std::string list;
	for (const auto &[k, v] : s.forced) {
		conf.set(k, v);
		list += (list.empty() ? "" : ", ") + k + " = " + v;
	}
	Conf start = conf;
	std::string names;
	for (const auto &[k, v] : s.forced)
		names += (names.empty() ? "" : ", ") + k;
	start.entries.insert(start.entries.begin(), {
		{"", "", k_header_user + std::string(" ") + to_utf8(s.user_conf)},
		{"", "", k_header_forced + std::string(" ") + names},
	});
	if (!conf.save(s.prisma_conf) || !start.save(s.snapshot)) {
		log.push_back("Could not write " + to_utf8(s.prisma_conf) + ": Luanti starts with its own settings");
		std::error_code ec;
		fs::remove(s.snapshot, ec);
		return s;
	}
	args.insert(args.begin(), {"--config", to_utf8(s.prisma_conf)});
	s.active = true;
	log.push_back("Luanti settings set by the pack: " + list);
	log.push_back("Your settings: " + to_utf8(s.user_conf));
	return s;
}

} // namespace prisma::luanticonf
