// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/log.hpp"
#include "common/util.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace prisma::log {

static std::FILE *g_file = nullptr;
static std::mutex g_mutex;
static std::vector<std::string> g_recent;
static bool g_in_error = false; // the "    detail" lines that follow belong to it

void open(const std::filesystem::path &path)
{
	std::lock_guard lock(g_mutex);
	if (!g_file)
		g_file = open_file(path, "wb");
}

static void write(const char *level, const char *fmt, va_list args)
{
	std::lock_guard lock(g_mutex);
	char text[2048];
	std::vsnprintf(text, sizeof(text), fmt, args);

	bool is_error = *level != 0;
	bool is_detail = !is_error && text[0] == ' ';
	if (is_error || (is_detail && g_in_error)) {
		if (g_recent.size() < 200)
			g_recent.push_back(text);
		g_in_error = true;
	} else {
		g_in_error = false;
	}

	if (!g_file)
		return;
	std::fprintf(g_file, "[%9.3f] %s%s\n", uptime(), level, text);
	std::fflush(g_file);
}

void info(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	write("", fmt, args);
	va_end(args);
}

void error(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	write("ERROR: ", fmt, args);
	va_end(args);
}

std::vector<std::string> recent_errors()
{
	std::lock_guard lock(g_mutex);
	return g_recent;
}

void clear_recent_errors()
{
	std::lock_guard lock(g_mutex);
	g_recent.clear();
	g_in_error = false;
}

} // namespace prisma::log
