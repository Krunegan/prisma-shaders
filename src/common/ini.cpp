// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/ini.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace prisma::ini {

namespace fs = std::filesystem;

static bool same(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z')
			x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z')
			y += 'a' - 'A';
		if (x != y)
			return false;
	}
	return true;
}

static std::string_view trim(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
		s.remove_suffix(1);
	return s;
}

#ifdef _WIN32
static bool valid_utf8(const std::string &s)
{
	for (size_t i = 0; i < s.size();) {
		unsigned char c = (unsigned char)s[i];
		int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		if (more < 0 || i + more >= s.size())
			return false;
		for (int k = 1; k <= more; k++)
			if (((unsigned char)s[i + k] >> 6) != 2)
				return false;
		i += more + 1;
	}
	return true;
}
#endif

static std::vector<std::string> read_lines(const fs::path &file)
{
	std::ifstream in(file, std::ios::binary);
	if (!in)
		return {};
	std::stringstream ss;
	ss << in.rdbuf();
	std::string text = ss.str();
	if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
		text.erase(0, 3);
#ifdef _WIN32
	// Written by WritePrivateProfileStringW (Prisma 0.6 and older): the ANSI code page
	if (!valid_utf8(text)) {
		int n = MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), nullptr, 0);
		std::wstring wide(n, L'\0');
		MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), wide.data(), n);
		int m = WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, nullptr, 0, nullptr, nullptr);
		text.assign(m, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, text.data(), m, nullptr, nullptr);
	}
#endif
	std::vector<std::string> lines;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		lines.push_back(std::move(line));
		start = end + 1;
	}
	return lines;
}

static bool write_lines(const fs::path &file, const std::vector<std::string> &lines)
{
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
#ifdef _WIN32
	const char *eol = "\r\n";
#else
	const char *eol = "\n";
#endif
	for (const std::string &l : lines)
		out << l << eol;
	return bool(out);
}

// "[name]" -> name; nullopt-like empty view with ok = false otherwise
static bool section_header(std::string_view line, std::string_view &name)
{
	line = trim(line);
	if (line.size() < 2 || line.front() != '[')
		return false;
	size_t close = line.find(']');
	if (close == std::string_view::npos)
		return false;
	name = trim(line.substr(1, close - 1));
	return true;
}

static bool is_comment(std::string_view line)
{
	line = trim(line);
	return line.empty() || line.front() == ';' || line.front() == '#';
}

// Line index of `key` in `section`, or -1. `section_end` gets the line after
// the section's last key (or -1 if there's no such section).
static int find(const std::vector<std::string> &lines, std::string_view section, std::string_view key,
	int &section_end)
{
	bool inside = false;
	section_end = -1;
	for (int i = 0; i < (int)lines.size(); i++) {
		std::string_view name;
		if (section_header(lines[i], name)) {
			inside = same(name, section);
			if (inside)
				section_end = i + 1;
			continue;
		}
		if (!inside || is_comment(lines[i]))
			continue;
		section_end = i + 1;
		size_t eq = lines[i].find('=');
		if (eq != std::string::npos && same(trim(std::string_view(lines[i]).substr(0, eq)), key))
			return i;
	}
	return -1;
}

std::string get(const fs::path &file, std::string_view section, std::string_view key, std::string_view fallback)
{
	std::vector<std::string> lines = read_lines(file);
	int end = -1;
	int i = find(lines, section, key, end);
	if (i < 0)
		return std::string(fallback);
	std::string_view line = lines[i];
	return std::string(trim(line.substr(line.find('=') + 1)));
}

int get_int(const fs::path &file, std::string_view section, std::string_view key, int fallback)
{
	std::string v = get(file, section, key, "");
	if (v.empty())
		return fallback;
	char *end = nullptr;
	long n = std::strtol(v.c_str(), &end, 10);
	return end == v.c_str() ? 0 : (int)n; // like GetPrivateProfileInt: no number is 0
}

bool set(const fs::path &file, std::string_view section, std::string_view key, std::string_view value)
{
	std::vector<std::string> lines = read_lines(file);
	int end = -1;
	int i = find(lines, section, key, end);
	std::string entry = std::string(key) + "=" + std::string(value);
	if (i >= 0) {
		lines[i] = entry;
	} else if (end >= 0) {
		lines.insert(lines.begin() + end, entry);
	} else {
		if (!lines.empty() && !trim(lines.back()).empty())
			lines.push_back("");
		lines.push_back("[" + std::string(section) + "]");
		lines.push_back(entry);
	}
	return write_lines(file, lines);
}

bool remove(const fs::path &file, std::string_view section, std::string_view key)
{
	std::vector<std::string> lines = read_lines(file);
	int end = -1;
	int i = find(lines, section, key, end);
	if (i < 0)
		return true;
	lines.erase(lines.begin() + i);
	return write_lines(file, lines);
}

} // namespace prisma::ini
