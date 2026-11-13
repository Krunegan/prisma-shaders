// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace prisma {

// A path as UTF-8 text, and back. Paths are std::filesystem::path all
// through the core; text (logs, GLSL, the menu) is UTF-8.
std::string to_utf8(const std::filesystem::path &path);
std::filesystem::path utf8_path(std::string_view text);
// With '/' as separator on every system
std::string to_utf8_generic(const std::filesystem::path &path);

// fopen() for any path, also outside the system's code page on Windows.
std::FILE *open_file(const std::filesystem::path &path, const char *mode);

// Directory of the module containing this code (prisma_core.dll / .so).
std::filesystem::path core_dir();
// Directory of the host executable (luanti.exe / luanti).
std::filesystem::path host_exe_dir();
// Product version stored in the host executable's resources, or "" if absent.
std::string host_version();

// Seconds since the core was loaded.
double uptime();

} // namespace prisma
