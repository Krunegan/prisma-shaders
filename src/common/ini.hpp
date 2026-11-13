// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	prisma.ini and the like: [section] headers and key=value lines, with ';'
	or '#' starting a comment line. Section and key names ignore ASCII case,
	as Windows' profile functions did. Text is UTF-8 (a file that isn't, from
	older versions on Windows, is read in the system's code page).

	Writing keeps the rest of the file as it was: order, comments, blank lines.
*/
#include <filesystem>
#include <string>
#include <string_view>

namespace prisma::ini {

// The value of `key` (trimmed), or `fallback` if the file, the section or
// the key don't exist.
std::string get(const std::filesystem::path &file, std::string_view section, std::string_view key,
	std::string_view fallback = {});

// The leading integer of the value, like GetPrivateProfileInt ("4 ; comment"
// is 4), or `fallback`.
int get_int(const std::filesystem::path &file, std::string_view section, std::string_view key, int fallback);

// Sets `key` in `section` (both created if needed). False if the file can't
// be written.
bool set(const std::filesystem::path &file, std::string_view section, std::string_view key, std::string_view value);

// Removes `key` from `section`, if it is there.
bool remove(const std::filesystem::path &file, std::string_view section, std::string_view key);

} // namespace prisma::ini
