// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace prisma::log {

// Opens (and truncates) the main log file. Safe to call once.
void open(const std::filesystem::path &path);

// printf-style, UTF-8. Every line is prefixed with the uptime and flushed,
// so the log survives a crash of the host process.
void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Error lines (and the indented detail lines that follow them) since the last
// clear, for the in-game menu.
std::vector<std::string> recent_errors();
void clear_recent_errors();

} // namespace prisma::log
