// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/tracking/shader_catalog.hpp"
#include "common/log.hpp"
#include "common/util.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace prisma::catalog {

struct Entry {
	std::string name;
	std::string stage;
	std::vector<std::string> lines; // significant lines only
};

static std::vector<Entry> g_entries;
static fs::path g_dir;
static bool g_loaded = false;

static std::string_view trim(std::string_view s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string_view::npos)
		return {};
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

// Lines that carry identity: drop blanks, lone braces and trivially short ones
// that every shader shares.
template <typename F>
static void for_each_significant_line(std::string_view text, F &&f)
{
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string_view::npos)
			end = text.size();
		std::string_view line = trim(text.substr(pos, end - pos));
		if (line.size() >= 6)
			f(line);
		pos = end + 1;
	}
}

static std::string read_file(const fs::path &p)
{
	std::ifstream in(p, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static void add_entry(const fs::path &file, const std::string &name, const std::string &stage)
{
	Entry e{name, stage, {}};
	std::string text = read_file(file);
	for_each_significant_line(text, [&](std::string_view l) { e.lines.emplace_back(l); });
	if (!e.lines.empty())
		g_entries.push_back(std::move(e));
}

void set_dir(const fs::path &shaders_dir)
{
	g_dir = shaders_dir;
	g_loaded = false;
}

static void load(const fs::path &shaders_dir)
{
	g_loaded = true;
	g_entries.clear();
	std::error_code ec;
	const fs::path &root = shaders_dir;
	for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
			it.increment(ec)) {
		if (!it->is_regular_file())
			continue;
		const fs::path &p = it->path();
		std::string file = to_utf8(p.filename());
		std::string dir = to_utf8_generic(fs::relative(p.parent_path(), root));
		std::string ext = to_utf8(p.extension());

		if (file == "opengl_vertex.glsl")
			add_entry(p, dir, "vertex");
		else if (file == "opengl_fragment.glsl")
			add_entry(p, dir, "fragment");
		else if (ext == ".vsh" || ext == ".fsh")
			add_entry(p, dir + "/" + to_utf8(p.stem()), ext == ".vsh" ? "vertex" : "fragment");
	}
	log::info("Shader catalog: %zu files in %s", g_entries.size(), to_utf8(shaders_dir).c_str());
}

size_t size()
{
	return g_entries.size();
}

Match identify(std::string_view source, std::string_view stage)
{
	if (!g_loaded)
		load(g_dir);
	std::unordered_set<std::string_view> have;
	for_each_significant_line(source, [&](std::string_view l) { have.insert(l); });

	Match best;
	size_t best_len = 0;
	for (const Entry &e : g_entries) {
		if (!stage.empty() && e.stage != stage)
			continue;
		size_t hits = 0;
		for (const std::string &l : e.lines)
			hits += have.count(l);
		float score = float(hits) / float(e.lines.size());
		// On a tie prefer the longer file: a short file (e.g. a tiny vertex
		// shader) can be fully contained in a bigger one.
		if (score > best.score || (score == best.score && e.lines.size() > best_len)) {
			best = {e.name, e.stage, score};
			best_len = e.lines.size();
		}
	}
	if (best.score < 0.85f)
		return {"", "", best.score};
	return best;
}

} // namespace prisma::catalog
